// Syscall layer

#include <stdint.h>
#include <errno.h>
#include "syscall.h"
#include "process.h"
#include "elf.h"
#include "sched.h"
#include "../fs/vfs.h"
#include "../acpi/extended.h"
#include "../mm/pmm.h"
#include "../../drivers/timer/hpet.h"
#include "../mm/vmm.h"
#include "../../boot/efi_handoff.h"
#include "../../boot/early_serial.h"
#include "../../include/tater_trace.h"
#include "../../shared/wifi_abi.h"

void kprint_write(const char *buf, uint64_t len);
void kprint_serial_only(const char *fmt, ...);
void kprint_serial_write(const char *buf, uint64_t len);
uint64_t kread_serial(char *buf, uint64_t len);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
int ps2_kbd_read(char *buf, uint32_t len);
void ps2_mouse_get(int32_t *x, int32_t *y, uint8_t *btns,
                   int32_t *dx, int32_t *dy);
void acpi_reset(void);
void acpi_shutdown(void);
extern void syscall_entry(void);
extern struct fry_handoff *g_handoff;
int wifi_9260_get_user_status(struct fry_wifi_status *out);
int wifi_9260_get_scan_entries(struct fry_wifi_scan_entry *out,
                               uint32_t max_entries, uint32_t *count_out);
int wifi_9260_connect_user(const char *ssid, const char *passphrase);
int wifi_9260_get_debug_log(char *buf, uint32_t bufsz);

// Set by sched_tick() to the kernel_stack_top of the current process.
// syscall_entry switches to this stack so that sched_yield/context_switch
// never saves a user-space RSP as the process's kernel saved context.
extern uint64_t g_syscall_kstack_top;
// Scratch storage for the user RSP during the stack-switch in syscall_entry.
// Only touched with IF=0 (SFMASK clears IF on syscall), so no race on BSP.
static uint64_t g_syscall_user_rsp __attribute__((used)) = 0;
static int32_t g_gui_slot_hint = -1;
static uint32_t g_gui_pid_hint = 0;
static uint8_t g_first_user_syscall_seen = 0;
static uint8_t g_first_init_syscall_seen = 0;
static uint8_t g_first_init_gui_spawn_seen = 0;
static uint8_t g_first_gui_fb_seen = 0;
static uint32_t g_spawn_attempt_count = 0;  /* visual spawn tracker */
/*
 * SYS_EXIT must not free the currently active process CR3/stack while still
 * executing on them.  Use a dedicated kernel-owned stack for exit teardown.
 */
static uint8_t g_sys_exit_stack[16384] __attribute__((aligned(16)));

#define USER_TOP USER_VA_TOP
#define PAGE_SIZE 4096ULL
#define FB_USER_BASE 0x0000000100000000ULL
#define VM_USER_BASE 0x0000100000000000ULL
#define VM_USER_LIMIT 0x00007FFF00000000ULL

#define FRY_PROT_READ  0x01u
#define FRY_PROT_WRITE 0x02u
#define FRY_PROT_EXEC  0x04u

#define FRY_MAP_SHARED  0x01u
#define FRY_MAP_PRIVATE 0x02u
#define FRY_MAP_FIXED   0x10u
#define FRY_MAP_ANON    0x20u
#define FRY_MAP_FILE    0x40u
#define FRY_MAP_RESERVE 0x80u
#define FRY_MAP_GUARD   0x100u

static int vm_unmap_region_range(struct fry_process *p, uint64_t base, uint64_t length);

struct readdir_ctx {
    char *buf;
    uint32_t len;
    uint32_t pos;
};
struct readdir_ex_ctx {
    uint8_t *buf;
    uint32_t len;
    uint32_t pos;
};
struct fry_dirent_hdr {
    uint16_t rec_len;
    uint16_t name_len;
    uint32_t attr;
    uint64_t size;
};

static uint32_t read_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static int is_taterwin_msg(const char *buf, uint64_t len) {
    /* tw_msg_header_t: u32 type, u32 magic("TWIN"=0x5457494E). */
    if (!buf || len < 8) return 0;
    uint32_t type = read_le32(buf);
    uint32_t magic = read_le32(buf + 4);
    if (magic != 0x5457494EU) return 0;
    return type >= 1 && type <= 7;
}

static int user_ptr_ok(uint64_t ptr, uint64_t len) {
    if (ptr >= USER_TOP) return 0;
    if (len == 0) return 1;
    if (ptr + len < ptr) return 0;
    return (ptr + len) <= USER_TOP;
}

static int user_buf_accessible(struct fry_process *p, uint64_t ptr, uint64_t len, int want_write) {
    if (!p || !p->cr3) return 0;
    if (!user_ptr_ok(ptr, len)) return 0;
    if (len == 0) return 1;

    uint64_t va = ptr & ~(PAGE_SIZE - 1ULL);
    uint64_t last = (ptr + len - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (;;) {
        uint64_t flags = vmm_query_user_flags(p->cr3, va);
        if ((flags & VMM_FLAG_PRESENT) == 0) return 0;
        if ((flags & VMM_FLAG_USER) == 0) return 0;
        if (want_write && (flags & VMM_FLAG_WRITE) == 0) return 0;
        if (va == last) break;
        if (va > USER_TOP - PAGE_SIZE) return 0;
        va += PAGE_SIZE;
    }
    return 1;
}

static int user_buf_mapped(struct fry_process *p, uint64_t ptr, uint64_t len) {
    return user_buf_accessible(p, ptr, len, 0);
}

static int user_buf_writable(struct fry_process *p, uint64_t ptr, uint64_t len) {
    return user_buf_accessible(p, ptr, len, 1);
}

static int copy_user_string(struct fry_process *p, uint64_t uptr, char *dst, uint32_t max) {
    if (!p || !dst || max == 0) return -1;
    if (!user_ptr_ok(uptr, 1)) return -1;
    uint32_t i = 0;
    uint64_t cur_page = ~0ULL;
    while (i + 1 < max) {
        uint64_t va = uptr + i;
        if (!user_ptr_ok(va, 1)) return -1;
        uint64_t page = va & ~(PAGE_SIZE - 1ULL);
        if (page != cur_page) {
            uint64_t flags = vmm_query_user_flags(p->cr3, va);
            if ((flags & (VMM_FLAG_PRESENT | VMM_FLAG_USER)) != (VMM_FLAG_PRESENT | VMM_FLAG_USER)) {
                return -1;
            }
            cur_page = page;
        }
        char c = *(const char *)(uintptr_t)va;
        dst[i] = c;
        if (c == 0) return 0;
        i++;
    }
    dst[max - 1] = 0;
    return 0;
}

int copyin(struct fry_process *p, uint64_t src_user, void *dst_kern, uint64_t len) {
    if (!user_buf_mapped(p, src_user, len)) return -EFAULT;
    const uint8_t *src = (const uint8_t *)(uintptr_t)src_user;
    uint8_t *dst = (uint8_t *)dst_kern;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

int copyout(struct fry_process *p, const void *src_kern, uint64_t dst_user, uint64_t len) {
    if (!user_buf_writable(p, dst_user, len)) return -EFAULT;
    const uint8_t *src = (const uint8_t *)src_kern;
    uint8_t *dst = (uint8_t *)(uintptr_t)dst_user;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

static int streq_lit(const char *a, const char *b) {
    if (!a || !b) return 0;
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static void boot_diag_stage(uint64_t stage) {
    struct fry_handoff *handoff = g_handoff;
    if (!TATER_BOOT_VISUAL_DEBUG) return;
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;

    uint64_t x0 = stage * 20ULL;
    if (x0 >= handoff->fb_width) return;

    uint64_t mw = 12ULL;
    uint64_t mh = 12ULL;
    uint64_t remain_w = handoff->fb_width - x0;
    if (remain_w < mw) mw = remain_w;
    if (handoff->fb_height < mh) mh = handoff->fb_height;

    /* Use VMM_FB_BASE (0xFFFFFFFFB0000000) — mapped during vmm_init, lives in
       PML4[511] which is copied to every user address space.  Safe to access
       from syscall context under user CR3. */
    volatile uint32_t *fb = (volatile uint32_t *)0xFFFFFFFFB0000000ULL;
    for (uint64_t y = 0; y < mh; y++) {
        uint64_t row = y * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < mw; x++) {
            fb[row + x] = 0x00F0F0F0u;
        }
    }
}

/* Colored diagnostic square — same layout as boot_diag_stage but with
   a caller-chosen color.  Row 0 = white boot markers, row 1 (y offset 20)
   = spawn-attempt markers so they're visually separate. */
static void boot_diag_color(uint64_t col, uint64_t row_idx, uint32_t color) {
    struct fry_handoff *handoff = g_handoff;
    if (!TATER_BOOT_VISUAL_DEBUG) return;
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;

    uint64_t x0 = col * 20ULL;
    uint64_t y0 = row_idx * 20ULL;
    if (x0 + 12 > handoff->fb_width) return;
    if (y0 + 12 > handoff->fb_height) return;

    volatile uint32_t *fb = (volatile uint32_t *)0xFFFFFFFFB0000000ULL;
    for (uint64_t y = 0; y < 12; y++) {
        uint64_t row = (y0 + y) * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < 12; x++) {
            fb[row + x] = color;
        }
    }
}

/* Spawn-failure classifier for bare-metal debugging:
   blue   = open/path lookup failure
   magenta= corrupt/invalid container or ELF
   cyan   = memory / address-space / process allocation failure
   orange = short read / bounds / translation style failure
   white  = other / unexpected */
static uint32_t boot_diag_spawn_error_color(int rc) {
    switch (rc) {
        case ELF_LOAD_ERR_OPEN:
            return 0x000080FFu;
        case ELF_LOAD_ERR_BAD_MAGIC:
        case ELF_LOAD_ERR_BAD_CRC:
        case ELF_LOAD_ERR_BAD_ELF_HEADER:
        case ELF_LOAD_ERR_BAD_ELF_MAGIC:
            return 0x00FF00FFu;
        case ELF_LOAD_ERR_NOMEM:
        case ELF_LOAD_ERR_VMM_SPACE:
        case ELF_LOAD_ERR_SEG_ALLOC:
        case ELF_LOAD_ERR_SEG_TRANSLATE:
        case ELF_LOAD_ERR_STACK_ALLOC:
        case PROCESS_LAUNCH_ERR_CREATE_USER:
            return 0x0000FFFFu;
        case ELF_LOAD_ERR_SHORT_HEADER:
        case ELF_LOAD_ERR_READ:
        case ELF_LOAD_ERR_BOUNDS:
            return 0x00FF8000u;
        default:
            return 0x00FFFFFFu;
    }
}

static const char *path_basename_lit(const char *path) {
    const char *base = path;
    if (!path) return "";
    while (*path) {
        if (*path == '/') base = path + 1;
        path++;
    }
    return base;
}

static int process_name_is_init(const char *name) {
    return streq_lit(path_basename_lit(name), "INIT.FRY");
}

static int process_name_is_gui(const char *name) {
    return streq_lit(path_basename_lit(name), "GUI.FRY");
}

static void note_user_boot_progress(struct fry_process *cur) {
    if (!cur || cur->is_kernel) return;
    if (!g_first_user_syscall_seen) {
        g_first_user_syscall_seen = 1;
        boot_diag_stage(36);
        if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_FIRST_USER_SYSCALL\n");
    }
    if (!g_first_init_syscall_seen && process_name_is_init(cur->name)) {
        g_first_init_syscall_seen = 1;
        boot_diag_stage(37);
        if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_INIT_SYSCALL\n");
    }
}

static void sbrk_rollback_pages(struct fry_process *p, uint64_t va_start, uint64_t va_end) {
    if (!p || !p->cr3) return;
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va);
        if (!pa) continue;
        vmm_unmap_user(p->cr3, va);
        pmm_free_page(pa & 0x000FFFFFFFFFF000ULL);
    }
}

static int gui_process_running(void) {
    if (g_gui_slot_hint >= 0 && g_gui_slot_hint < (int32_t)PROC_MAX) {
        struct fry_process *hp = &procs[g_gui_slot_hint];
        if (hp->pid == g_gui_pid_hint &&
            hp->state != PROC_UNUSED &&
            hp->state != PROC_DEAD &&
            process_name_is_gui(hp->name)) {
            return 1;
        }
    }
    g_gui_slot_hint = -1;
    g_gui_pid_hint = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD) continue;
        if (process_name_is_gui(procs[i].name)) {
            g_gui_slot_hint = (int32_t)i;
            g_gui_pid_hint = procs[i].pid;
            return 1;
        }
    }
    return 0;
}

static int readdir_cb(const char *name, void *c) {
    struct readdir_ctx *rc = (struct readdir_ctx *)c;
    uint32_t i = 0;
    while (name[i]) {
        if (rc->pos + 1 >= rc->len) return 1;
        rc->buf[rc->pos++] = name[i++];
    }
    if (rc->pos + 1 < rc->len) {
        rc->buf[rc->pos++] = '\n';
    }
    if (rc->pos + 1 >= rc->len) return 1;
    return 0;
}
static int readdir_ex_cb(const char *name, uint64_t size, uint32_t attr, void *c) {
    struct readdir_ex_ctx *rc = (struct readdir_ex_ctx *)c;
    uint32_t name_len = 0;
    while (name[name_len]) name_len++;
    uint32_t max_name = 0xFFFF - (uint32_t)sizeof(struct fry_dirent_hdr) - 1;
    if (name_len > max_name) name_len = max_name;
    uint32_t rec_len = (uint32_t)sizeof(struct fry_dirent_hdr) + name_len + 1;
    if (rec_len < sizeof(struct fry_dirent_hdr)) return 1;
    if (rc->pos + rec_len > rc->len) return 1;
    struct fry_dirent_hdr *h = (struct fry_dirent_hdr *)(rc->buf + rc->pos);
    h->rec_len = (uint16_t)rec_len;
    h->name_len = (uint16_t)name_len;
    h->attr = attr;
    h->size = size;
    uint8_t *dst = (uint8_t *)(h + 1);
    for (uint32_t i = 0; i < name_len; i++) dst[i] = (uint8_t)name[i];
    dst[name_len] = 0;
    rc->pos += rec_len;
    if (rc->pos >= rc->len) return 1;
    return 0;
}

__attribute__((noreturn))
static void syscall_exit_finish(uint32_t pid) {
    proc_free(pid);
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((noreturn))
static void syscall_exit_current(uint32_t code) {
    struct fry_process *cur = proc_current();
    if (!cur) {
        for (;;) __asm__ volatile("hlt");
    }

    cur->exit_code = code;
    uint32_t pid = cur->pid;
    uint64_t kcr3 = vmm_get_kernel_pml4_phys();
    uint64_t exit_sp = ((uint64_t)(uintptr_t)&g_sys_exit_stack[sizeof(g_sys_exit_stack)]) & ~0xFULL;

    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %1, %%rsp\n"
        "mov %2, %%edi\n"
        "call *%3\n"
        :
        : "r"(kcr3),
          "r"(exit_sp),
          "r"(pid),
          "r"(syscall_exit_finish)
        : "rdi", "memory");

    __builtin_unreachable();
}

/*
 * Syscall number allocation policy (Phase 0 ABI discipline):
 *
 *   0 -  31  Core POSIX-like syscalls (file I/O, process, memory).
 *             These numbers are STABLE and must never be renumbered.
 *  32 -  36  Filesystem diagnostics / extended readdir.
 *  37 -  50  Driver-specific syscalls (WiFi, Ethernet, etc.).
 *             Numbers in this range may be reclaimed when a driver
 *             is removed; the number itself must not be reused for
 *             an unrelated purpose within the same major release.
 *  51 -  63  Reserved for future driver syscalls.
 *  52 -  54  VM syscalls (mmap/munmap/mprotect) — STABLE.
 *  64 - 127  Reserved for future POSIX-compat expansion.
 * 128 - 255  Available for experimental / debug syscalls.
 * 256+       Undefined; returns -ENOSYS.
 *
 * Error convention: every syscall returns 0 on success or a positive
 * value (e.g. fd, pid, byte count), and a negative errno on failure.
 * Pointer-returning syscalls (mmap) encode the errno as (void *)-errno
 * and userspace checks with FRY_IS_ERR().
 */
enum {
    /* --- Core POSIX-like (0-31, STABLE) --- */
    SYS_WRITE = 0,
    SYS_READ = 1,
    SYS_EXIT = 2,
    SYS_SPAWN = 3,
    SYS_SLEEP = 4,
    SYS_OPEN = 5,
    SYS_CLOSE = 6,
    SYS_GETPID = 7,
    SYS_STAT = 8,
    SYS_READDIR = 9,
    SYS_GETTIME = 10,
    SYS_REBOOT = 11,
    SYS_SHUTDOWN = 12,
    SYS_WAIT = 13,
    SYS_PROCCOUNT = 14,
    SYS_SETBRIGHT = 15,
    SYS_GETBRIGHT = 16,
    SYS_GETBATTERY = 17,
    SYS_FB_INFO = 18,
    SYS_FB_MAP = 19,
    SYS_MOUSE_GET = 20,
    SYS_PROC_OUTPUT = 21,
    SYS_SBRK = 22,
    SYS_SHM_ALLOC = 23,
    SYS_SHM_MAP = 24,
    SYS_PROC_INPUT = 25,
    SYS_KILL = 26,
    SYS_SHM_FREE = 27,
    SYS_ACPI_DIAG = 28,
    SYS_CREATE = 29,
    SYS_MKDIR = 30,
    SYS_UNLINK = 31,

    /* --- FS diagnostics / extended (32-36) --- */
    SYS_STORAGE_INFO = 32,
    SYS_PATH_FS_INFO = 33,
    SYS_MOUNTS_INFO = 34,
    SYS_READDIR_EX = 35,
    SYS_MOUNTS_DEBUG = 36,

    /* --- Driver-specific (37-50) --- */
    SYS_WIFI_STATUS = 37,
    SYS_WIFI_SCAN = 38,
    SYS_WIFI_CONNECT = 39,
    SYS_WIFI_DEBUG = 40,
    SYS_WIFI_CPU_STATUS = 41,
    SYS_WIFI_INIT_LOG = 42,
    SYS_WIFI_DEBUG2 = 43,
    SYS_WIFI_HANDOFF = 44,
    SYS_WIFI_DEBUG3 = 45,
    SYS_WIFI_REINIT = 46,
    SYS_WIFI_CMD_TRACE = 47,
    SYS_WIFI_SRAM = 48,
    SYS_WIFI_DEEP_DIAG = 49,
    SYS_WIFI_VERIFY = 50,
    SYS_ETH_DIAG = 51,

    /* --- VM syscalls (STABLE) --- */
    SYS_MMAP = 52,
    SYS_MUNMAP = 53,
    SYS_MPROTECT = 54
};

struct fry_fb_info {
    uint64_t phys;
    uint64_t size;
    uint64_t user_base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

#define SHM_MAX 128
#define SHM_USER_BASE 0x20000000000ULL
#define SHM_SLOT_STRIDE 0x10000000ULL
struct shm_region {
    uint64_t phys_base;
    uint32_t page_count;
    uint32_t owner_pid;
    uint32_t map_count;
    uint32_t mapped_pids[PROC_MAX]; /* slot-indexed pid guard against slot reuse */
    int used;
};
static struct shm_region shm_regions[SHM_MAX];

static int shm_proc_slot_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].pid == pid &&
            procs[i].state != PROC_UNUSED &&
            procs[i].state != PROC_DEAD) {
            return (int)i;
        }
    }
    return -1;
}

static void shm_untrack_slot(struct shm_region *r, int slot, uint32_t pid) {
    if (!r) return;
    if (slot < 0 || slot >= (int)PROC_MAX) return;
    if (r->mapped_pids[slot] != pid) return;
    r->mapped_pids[slot] = 0;
    if (r->map_count > 0) r->map_count--;
}

static void shm_release_pages(struct shm_region *r) {
    if (!r || !r->phys_base || !r->page_count) return;
    pmm_free_pages(r->phys_base, r->page_count);
}

static void shm_reset_region(struct shm_region *r) {
    if (!r) return;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        r->mapped_pids[i] = 0;
    }
    r->phys_base = 0;
    r->page_count = 0;
    r->owner_pid = 0;
    r->map_count = 0;
    r->used = 0;
}

static void shm_unmap_from_all_processes(int id, struct shm_region *r) {
    if (!r) return;
    uint64_t virt_base = SHM_USER_BASE + (uint64_t)id * SHM_SLOT_STRIDE;
    uint64_t kernel_cr3 = vmm_get_kernel_pml4_phys();
    for (uint32_t slot = 0; slot < PROC_MAX; slot++) {
        uint32_t pid = r->mapped_pids[slot];
        if (!pid) continue;
        if (procs[slot].pid == pid &&
            procs[slot].state != PROC_UNUSED &&
            procs[slot].state != PROC_DEAD &&
            procs[slot].cr3 &&
            procs[slot].cr3 != kernel_cr3) {
            for (uint32_t p = 0; p < r->page_count; p++) {
                vmm_unmap_user(procs[slot].cr3, virt_base + (uint64_t)p * PAGE_SIZE);
            }
        }
        shm_untrack_slot(r, (int)slot, pid);
    }
}

static void shm_destroy_region(int id) {
    if (id < 0 || id >= SHM_MAX) return;
    struct shm_region *r = &shm_regions[id];
    if (!r->used) return;
    shm_unmap_from_all_processes(id, r);
    shm_release_pages(r);
    shm_reset_region(r);
}

void syscall_shm_process_exit(uint32_t pid) {
    int slot = shm_proc_slot_by_pid(pid);
    for (int id = 0; id < SHM_MAX; id++) {
        struct shm_region *r = &shm_regions[id];
        if (!r->used) continue;
        if (r->owner_pid == pid) {
            shm_destroy_region(id);
            continue;
        }
        if (slot >= 0) {
            shm_untrack_slot(r, slot, pid);
        }
    }
}

#define VM_SHARED_MAX 128
#define VM_BACKING_NONE UINT32_MAX

struct vm_shared_object {
    uint64_t *pages;
    uint32_t *page_refs;
    uint32_t page_count;
    uint8_t used;
    uint8_t _pad[3];
};

static struct vm_shared_object vm_shared_objects[VM_SHARED_MAX];

static int vm_region_alloc_slot(struct fry_process *p) {
    if (!p) return -1;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (!p->vm_regions[i].used) return i;
    }
    return -1;
}

static int vm_region_alloc_slots(struct fry_process *p, int needed, int *slot1, int *slot2) {
    if (slot1) *slot1 = -1;
    if (slot2) *slot2 = -1;
    if (needed <= 0) return 0;
    if (!p) return -1;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (p->vm_regions[i].used) continue;
        if (slot1 && *slot1 < 0) {
            *slot1 = i;
            needed--;
        } else if (slot2 && *slot2 < 0) {
            *slot2 = i;
            needed--;
        }
        if (needed == 0) return 0;
    }
    return -1;
}

static void vm_region_clear(struct fry_vm_region *r) {
    if (!r) return;
    r->base = 0;
    r->length = 0;
    r->prot = 0;
    r->flags = 0;
    r->backing_id = VM_BACKING_NONE;
    r->backing_page_start = 0;
    r->kind = FRY_VM_REGION_NONE;
    r->used = 0;
    r->committed = 0;
}

static void vm_region_fill(struct fry_vm_region *r, uint64_t base, uint64_t length,
                           uint32_t prot, uint32_t flags, uint16_t kind,
                           uint32_t backing_id, uint32_t backing_page_start,
                           uint8_t committed) {
    if (!r) return;
    r->base = base;
    r->length = length;
    r->prot = prot;
    r->flags = flags;
    r->backing_id = backing_id;
    r->backing_page_start = backing_page_start;
    r->kind = kind;
    r->used = 1;
    r->committed = committed;
}

static uint32_t vm_region_page_start_at(const struct fry_vm_region *r, uint64_t base) {
    if (!r || base < r->base) return 0;
    return r->backing_page_start + (uint32_t)((base - r->base) / PAGE_SIZE);
}

static void vm_region_fill_span_from_parent(struct fry_vm_region *dst,
                                            const struct fry_vm_region *src,
                                            uint64_t base, uint64_t length,
                                            uint32_t prot, uint32_t flags,
                                            uint8_t committed) {
    vm_region_fill(dst, base, length, prot, flags, src->kind, src->backing_id,
                   vm_region_page_start_at(src, base), committed);
}

static int vm_regions_can_merge(const struct fry_vm_region *a,
                                const struct fry_vm_region *b) {
    if (!a || !b || !a->used || !b->used) return 0;
    if (a->base + a->length != b->base) return 0;
    if (a->prot != b->prot || a->flags != b->flags) return 0;
    if (a->kind != b->kind || a->committed != b->committed) return 0;
    if (a->kind == FRY_VM_REGION_ANON_SHARED) {
        if (a->backing_id != b->backing_id) return 0;
        if (a->backing_page_start + (uint32_t)(a->length / PAGE_SIZE) != b->backing_page_start) {
            return 0;
        }
    }
    return 1;
}

static void vm_region_merge_neighbors(struct fry_process *p) {
    if (!p) return;
    int merged = 1;
    while (merged) {
        merged = 0;
        for (int i = 0; i < PROC_VMREG_MAX && !merged; i++) {
            if (!p->vm_regions[i].used) continue;
            for (int j = 0; j < PROC_VMREG_MAX; j++) {
                if (i == j || !p->vm_regions[j].used) continue;
                struct fry_vm_region *a = &p->vm_regions[i];
                struct fry_vm_region *b = &p->vm_regions[j];
                if (b->base < a->base) {
                    struct fry_vm_region *tmp = a;
                    a = b;
                    b = tmp;
                }
                if (!vm_regions_can_merge(a, b)) continue;
                a->length += b->length;
                vm_region_clear(b);
                merged = 1;
                break;
            }
        }
    }
}

static int vm_region_overlaps(const struct fry_vm_region *r, uint64_t base, uint64_t end) {
    if (!r || !r->used) return 0;
    uint64_t r_end = r->base + r->length;
    if (r_end < r->base) return 0;
    return !(end <= r->base || base >= r_end);
}

static int vm_any_region_overlap(const struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || length == 0) return 0;
    if (base + length < base) return 1;
    uint64_t end = base + length;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (vm_region_overlaps(&p->vm_regions[i], base, end)) return 1;
    }
    return 0;
}

static int vm_region_find_containing(const struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || length == 0) return -1;
    if (base + length < base) return -1;
    uint64_t end = base + length;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        if (!p->vm_regions[i].used) continue;
        uint64_t r_base = p->vm_regions[i].base;
        uint64_t r_end = r_base + p->vm_regions[i].length;
        if (r_end < r_base) continue;
        if (base >= r_base && end <= r_end) return i;
    }
    return -1;
}

static int vm_region_collect_covering_slots(const struct fry_process *p,
                                            uint64_t base, uint64_t length,
                                            int *slots, int max_slots) {
    if (!p || !slots || max_slots <= 0 || length == 0) return -1;
    if (base + length < base) return -1;

    uint64_t cursor = base;
    uint64_t end = base + length;
    int count = 0;

    while (cursor < end) {
        int slot = -1;
        uint64_t slot_end = 0;
        for (int i = 0; i < PROC_VMREG_MAX; i++) {
            const struct fry_vm_region *r = &p->vm_regions[i];
            if (!r->used) continue;
            uint64_t r_end = r->base + r->length;
            if (r_end < r->base) continue;
            if (cursor < r->base || cursor >= r_end) continue;
            slot = i;
            slot_end = r_end;
            break;
        }
        if (slot < 0) return -1;
        if (count >= max_slots) return -1;
        slots[count++] = slot;
        cursor = (slot_end < end) ? slot_end : end;
    }

    return count;
}

static int vm_prot_supported(uint32_t prot) {
    if ((prot & ~(FRY_PROT_READ | FRY_PROT_WRITE | FRY_PROT_EXEC)) != 0) return 0;
    if (prot == 0) return 1;
    if ((prot & FRY_PROT_READ) == 0) return 0;
    if ((prot & (FRY_PROT_WRITE | FRY_PROT_EXEC)) == (FRY_PROT_WRITE | FRY_PROT_EXEC)) return 0;
    return 1;
}

static int vm_flags_supported(uint32_t flags) {
    if ((flags & ~(FRY_MAP_SHARED | FRY_MAP_PRIVATE | FRY_MAP_FIXED |
                   FRY_MAP_ANON | FRY_MAP_FILE | FRY_MAP_RESERVE |
                   FRY_MAP_GUARD)) != 0) return 0;
    if ((flags & FRY_MAP_GUARD) != 0) {
        /* guard: must be private+anon, nothing else */
        if ((flags & ~(FRY_MAP_GUARD | FRY_MAP_PRIVATE | FRY_MAP_ANON | FRY_MAP_FIXED)) != 0) return 0;
        if ((flags & (FRY_MAP_PRIVATE | FRY_MAP_ANON)) != (FRY_MAP_PRIVATE | FRY_MAP_ANON)) return 0;
        return 1;
    }
    if ((flags & (FRY_MAP_SHARED | FRY_MAP_PRIVATE)) == 0) return 0;
    if ((flags & (FRY_MAP_SHARED | FRY_MAP_PRIVATE)) == (FRY_MAP_SHARED | FRY_MAP_PRIVATE)) return 0;
    if ((flags & (FRY_MAP_ANON | FRY_MAP_FILE)) == 0) return 0;
    if ((flags & (FRY_MAP_ANON | FRY_MAP_FILE)) == (FRY_MAP_ANON | FRY_MAP_FILE)) return 0;
    if ((flags & FRY_MAP_FILE) != 0 && (flags & FRY_MAP_SHARED) != 0) return 0;
    if ((flags & FRY_MAP_RESERVE) != 0 && (flags & FRY_MAP_FILE) != 0) return 0;
    if ((flags & FRY_MAP_RESERVE) != 0 && (flags & FRY_MAP_SHARED) != 0) return 0;
    return 1;
}

static uint64_t vm_prot_to_pte_flags(uint32_t prot) {
    uint64_t flags = 0;
    if (prot != 0) flags |= VMM_FLAG_USER;
    if (prot & FRY_PROT_WRITE) flags |= VMM_FLAG_WRITE;
    if ((prot & FRY_PROT_EXEC) == 0) flags |= VMM_FLAG_NO_EXECUTE;
    return flags;
}

static int vm_range_mapped(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || length == 0) return 0;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (vmm_virt_to_phys_user(p->cr3, va) == 0) return 0;
    }
    return 1;
}

static int vm_range_available(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p || !p->cr3 || length == 0) return 0;
    if (base < VM_USER_BASE) return 0;
    if (base + length < base) return 0;
    if (base + length > VM_USER_LIMIT) return 0;
    if (vm_any_region_overlap(p, base, length)) return 0;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (vmm_virt_to_phys_user(p->cr3, va) != 0) return 0;
    }
    return 1;
}

static uint64_t vm_find_free_range(struct fry_process *p, uint64_t length) {
    if (!p || length == 0) return 0;
    if (VM_USER_LIMIT <= VM_USER_BASE) return 0;
    if (length > VM_USER_LIMIT - VM_USER_BASE) return 0;

    uint64_t max_start = VM_USER_LIMIT - length;
    for (uint64_t base = VM_USER_BASE; base <= max_start; base += PAGE_SIZE) {
        if (vm_range_available(p, base, length)) return base;
    }
    return 0;
}

static void vm_zero_page(uint64_t phys) {
    uint8_t *dst = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) dst[i] = 0;
}

static void vm_unmap_pages_only(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p) return;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (!vmm_virt_to_phys_user(p->cr3, va)) continue;
        vmm_unmap_user(p->cr3, va);
    }
}

static void vm_release_private_pages(struct fry_process *p, uint64_t base, uint64_t length) {
    if (!p) return;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va);
        if (!pa) continue;
        vmm_unmap_user(p->cr3, va);
        pmm_free_page(pa & 0x000FFFFFFFFFF000ULL);
    }
}

static int vm_commit_private_pages(struct fry_process *p, uint64_t base,
                                   uint64_t length, uint32_t prot) {
    if (!p || !p->cr3 || length == 0) return -1;
    uint64_t pte_flags = vm_prot_to_pte_flags(prot);
    uint64_t mapped = 0;
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) {
            vm_release_private_pages(p, base, mapped);
            return -1;
        }
        vmm_map_user(p->cr3, va, pa, pte_flags);
        if ((vmm_virt_to_phys_user(p->cr3, va) & 0x000FFFFFFFFFF000ULL) != pa) {
            vmm_unmap_user(p->cr3, va);
            pmm_free_page(pa);
            vm_release_private_pages(p, base, mapped);
            return -1;
        }
        vm_zero_page(pa);
        mapped += PAGE_SIZE;
    }
    return 0;
}

static void vm_shared_reset(struct vm_shared_object *obj) {
    if (!obj) return;
    obj->pages = 0;
    obj->page_refs = 0;
    obj->page_count = 0;
    obj->used = 0;
}

static int vm_shared_alloc_slot(void) {
    for (int i = 0; i < VM_SHARED_MAX; i++) {
        if (!vm_shared_objects[i].used) return i;
    }
    return -1;
}

static int vm_shared_all_released(const struct vm_shared_object *obj) {
    if (!obj || !obj->used) return 1;
    for (uint32_t i = 0; i < obj->page_count; i++) {
        if (obj->page_refs[i] != 0) return 0;
    }
    return 1;
}

static void vm_shared_destroy(int id) {
    if (id < 0 || id >= VM_SHARED_MAX) return;
    struct vm_shared_object *obj = &vm_shared_objects[id];
    if (!obj->used) return;
    if (obj->pages) {
        for (uint32_t i = 0; i < obj->page_count; i++) {
            if (obj->pages[i]) {
                pmm_free_page(obj->pages[i]);
                obj->pages[i] = 0;
            }
        }
    }
    if (obj->pages) kfree(obj->pages);
    if (obj->page_refs) kfree(obj->page_refs);
    vm_shared_reset(obj);
}

static int vm_shared_create(uint32_t page_count) {
    int id = vm_shared_alloc_slot();
    if (id < 0) return -1;

    struct vm_shared_object *obj = &vm_shared_objects[id];
    vm_shared_reset(obj);
    obj->pages = (uint64_t *)kmalloc((uint64_t)page_count * sizeof(uint64_t));
    if (!obj->pages) return -1;
    obj->page_refs = (uint32_t *)kmalloc((uint64_t)page_count * sizeof(uint32_t));
    if (!obj->page_refs) {
        kfree(obj->pages);
        obj->pages = 0;
        return -1;
    }
    obj->page_count = page_count;
    obj->used = 1;
    for (uint32_t i = 0; i < page_count; i++) {
        obj->pages[i] = 0;
        obj->page_refs[i] = 0;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) {
            vm_shared_destroy(id);
            return -1;
        }
        vm_zero_page(pa);
        obj->pages[i] = pa;
        obj->page_refs[i] = 1;
    }
    return id;
}

static void vm_shared_release_range(uint32_t id, uint32_t page_start,
                                    uint32_t page_count) {
    if (id >= VM_SHARED_MAX) return;
    struct vm_shared_object *obj = &vm_shared_objects[id];
    if (!obj->used || page_start > obj->page_count) return;
    if (page_count > obj->page_count - page_start) return;

    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t idx = page_start + i;
        if (obj->page_refs[idx] == 0) continue;
        obj->page_refs[idx]--;
        if (obj->page_refs[idx] == 0 && obj->pages[idx]) {
            pmm_free_page(obj->pages[idx]);
            obj->pages[idx] = 0;
        }
    }
    if (vm_shared_all_released(obj)) vm_shared_destroy((int)id);
}

static int vm_map_shared_pages(struct fry_process *p, uint64_t base, uint64_t length,
                               uint32_t prot, uint32_t backing_id,
                               uint32_t page_start) {
    if (!p || backing_id >= VM_SHARED_MAX) return -1;
    struct vm_shared_object *obj = &vm_shared_objects[backing_id];
    if (!obj->used) return -1;
    uint32_t page_count = (uint32_t)(length / PAGE_SIZE);
    if (page_start > obj->page_count) return -1;
    if (page_count > obj->page_count - page_start) return -1;

    uint64_t pte_flags = vm_prot_to_pte_flags(prot) | VMM_FLAG_NOFREE;
    uint64_t mapped = 0;
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t pa = obj->pages[page_start + i];
        if (!pa) {
            vm_unmap_pages_only(p, base, mapped);
            return -1;
        }
        uint64_t va = base + (uint64_t)i * PAGE_SIZE;
        vmm_map_user(p->cr3, va, pa, pte_flags);
        if ((vmm_virt_to_phys_user(p->cr3, va) & 0x000FFFFFFFFFF000ULL) != pa) {
            vmm_unmap_user(p->cr3, va);
            vm_unmap_pages_only(p, base, mapped);
            return -1;
        }
        mapped += PAGE_SIZE;
    }
    return 0;
}

static int vm_map_anon_private_region(struct fry_process *p, uint64_t base, uint64_t length,
                                      uint32_t prot, uint32_t flags) {
    if (!p || !p->cr3 || length == 0) return -1;
    if (!vm_range_available(p, base, length)) return -1;

    if ((flags & FRY_MAP_RESERVE) == 0) {
        if (vm_commit_private_pages(p, base, length, prot) != 0) return -1;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) {
        if ((flags & FRY_MAP_RESERVE) == 0) vm_release_private_pages(p, base, length);
        return -1;
    }

    vm_region_fill(&p->vm_regions[slot], base, length,
                   ((flags & FRY_MAP_RESERVE) != 0) ? 0 : prot,
                   flags, FRY_VM_REGION_ANON_PRIVATE, VM_BACKING_NONE, 0,
                   ((flags & FRY_MAP_RESERVE) == 0));
    return 0;
}

static int vm_map_guard_region(struct fry_process *p, uint64_t base, uint64_t length,
                               uint32_t flags) {
    if (!p || !p->cr3 || length == 0) return -1;
    if (!vm_range_available(p, base, length)) return -1;
    int slot = vm_region_alloc_slot(p);
    if (slot < 0) return -1;
    vm_region_fill(&p->vm_regions[slot], base, length, 0, flags,
                   FRY_VM_REGION_GUARD, VM_BACKING_NONE, 0, 0);
    return 0;
}

static int vm_map_anon_shared_region(struct fry_process *p, uint64_t base, uint64_t length,
                                     uint32_t prot, uint32_t flags) {
    if (!p || !p->cr3 || length == 0) return -1;
    if (!vm_range_available(p, base, length)) return -1;

    int backing_id = vm_shared_create((uint32_t)(length / PAGE_SIZE));
    if (backing_id < 0) return -1;
    if (vm_map_shared_pages(p, base, length, prot, (uint32_t)backing_id, 0) != 0) {
        vm_shared_destroy(backing_id);
        return -1;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) {
        vm_unmap_pages_only(p, base, length);
        vm_shared_destroy(backing_id);
        return -1;
    }

    vm_region_fill(&p->vm_regions[slot], base, length, prot, flags,
                   FRY_VM_REGION_ANON_SHARED, (uint32_t)backing_id, 0, 1);
    return 0;
}

static int vm_map_file_region(struct fry_process *p, uint64_t base, uint64_t length,
                              uint32_t prot, uint32_t flags, int fd) {
    if (!p || fd < 3 || fd >= 64 || !p->fd_ptrs[fd]) return -1;
    if (!vm_range_available(p, base, length)) return -1;
    if (vm_commit_private_pages(p, base, length, prot) != 0) return -1;

    struct vfs_file file = *(struct vfs_file *)p->fd_ptrs[fd];
    uint64_t remaining = file.size;
    if (remaining > length) remaining = length;
    uint64_t va = base;

    while (remaining > 0) {
        uint64_t pa = vmm_virt_to_phys_user(p->cr3, va);
        if (!pa) {
            vm_release_private_pages(p, base, length);
            return -1;
        }
        uint32_t chunk = (remaining > PAGE_SIZE) ? (uint32_t)PAGE_SIZE : (uint32_t)remaining;
        int rd = vfs_read(&file, (void *)(uintptr_t)vmm_phys_to_virt(pa), chunk);
        if (rd < 0 || (uint32_t)rd != chunk) {
            vm_release_private_pages(p, base, length);
            return -1;
        }
        remaining -= chunk;
        va += PAGE_SIZE;
    }

    int slot = vm_region_alloc_slot(p);
    if (slot < 0) {
        vm_release_private_pages(p, base, length);
        return -1;
    }

    vm_region_fill(&p->vm_regions[slot], base, length, prot, flags,
                   FRY_VM_REGION_FILE_PRIVATE, VM_BACKING_NONE, 0, 1);
    return 0;
}

static int vm_release_region_pages(struct fry_process *p,
                                   const struct fry_vm_region *r,
                                   uint64_t base, uint64_t length) {
    if (!p || !r || length == 0) return -1;
    if (!r->committed) return 0;
    if (!vm_range_mapped(p, base, length)) return -1;

    if (r->kind == FRY_VM_REGION_ANON_SHARED) {
        vm_unmap_pages_only(p, base, length);
        vm_shared_release_range(r->backing_id,
                                vm_region_page_start_at(r, base),
                                (uint32_t)(length / PAGE_SIZE));
        return 0;
    }

    if (r->kind == FRY_VM_REGION_ANON_PRIVATE ||
        r->kind == FRY_VM_REGION_FILE_PRIVATE) {
        vm_release_private_pages(p, base, length);
        return 0;
    }

    return -1;
}

static int vm_unmap_region_range(struct fry_process *p, uint64_t base, uint64_t length) {
    uint64_t end = base + length;
    int slots[PROC_VMREG_MAX];
    int count = vm_region_collect_covering_slots(p, base, length, slots, PROC_VMREG_MAX);
    if (count < 0) return -1;

    int spill_slot = -1;
    if (count == 1) {
        const struct fry_vm_region *r = &p->vm_regions[slots[0]];
        uint64_t r_end = r->base + r->length;
        if (base > r->base && end < r_end) {
            if (vm_region_alloc_slots(p, 1, &spill_slot, 0) != 0) return -1;
        }
    }

    for (int i = 0; i < count; i++) {
        const struct fry_vm_region *r = &p->vm_regions[slots[i]];
        uint64_t overlap_base = (base > r->base) ? base : r->base;
        uint64_t r_end = r->base + r->length;
        uint64_t overlap_end = (end < r_end) ? end : r_end;
        if (overlap_end <= overlap_base) return -1;
        if (vm_release_region_pages(p, r, overlap_base, overlap_end - overlap_base) != 0) {
            return -1;
        }
    }

    if (count == 1) {
        int slot = slots[0];
        struct fry_vm_region r = p->vm_regions[slot];
        uint64_t r_end = r.base + r.length;
        if (base == r.base && end == r_end) {
            vm_region_clear(&p->vm_regions[slot]);
        } else if (base == r.base) {
            vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, end, r_end - end,
                                            r.prot, r.flags, r.committed);
        } else if (end == r_end) {
            vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, r.base,
                                            base - r.base, r.prot, r.flags, r.committed);
        } else {
            vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, r.base,
                                            base - r.base, r.prot, r.flags, r.committed);
            vm_region_fill_span_from_parent(&p->vm_regions[spill_slot], &r, end,
                                            r_end - end, r.prot, r.flags, r.committed);
        }
    } else {
        int first_slot = slots[0];
        int last_slot = slots[count - 1];
        struct fry_vm_region first = p->vm_regions[first_slot];
        struct fry_vm_region last = p->vm_regions[last_slot];
        uint64_t last_end = last.base + last.length;

        if (base > first.base) {
            vm_region_fill_span_from_parent(&p->vm_regions[first_slot], &first, first.base,
                                            base - first.base, first.prot, first.flags,
                                            first.committed);
        } else {
            vm_region_clear(&p->vm_regions[first_slot]);
        }

        for (int i = 1; i < count - 1; i++) {
            vm_region_clear(&p->vm_regions[slots[i]]);
        }

        if (end < last_end) {
            vm_region_fill_span_from_parent(&p->vm_regions[last_slot], &last, end,
                                            last_end - end, last.prot, last.flags,
                                            last.committed);
        } else {
            vm_region_clear(&p->vm_regions[last_slot]);
        }
    }
    vm_region_merge_neighbors(p);
    return 0;
}

static int vm_mprotect_region_range(struct fry_process *p, uint64_t base, uint64_t length,
                                    uint32_t prot) {
    int slot = vm_region_find_containing(p, base, length);
    if (slot < 0) return -1;
    struct fry_vm_region r = p->vm_regions[slot];
    uint64_t end = base + length;
    uint64_t r_end = r.base + r.length;
    int slot1 = -1;
    int slot2 = -1;
    if (base == r.base && end == r_end) {
    } else if (base == r.base || end == r_end) {
        if (vm_region_alloc_slots(p, 1, &slot1, 0) != 0) return -1;
    } else {
        if (vm_region_alloc_slots(p, 2, &slot1, &slot2) != 0) return -1;
    }

    if (!r.committed) {
        if (r.kind != FRY_VM_REGION_ANON_PRIVATE && r.kind != FRY_VM_REGION_GUARD) return -1;
        if (prot == 0) return 0;
        if (r.kind == FRY_VM_REGION_GUARD) return -1;
        if (vm_commit_private_pages(p, base, length, prot) != 0) return -1;

        uint32_t committed_flags = r.flags & ~FRY_MAP_RESERVE;
        if (base == r.base && end == r_end) {
            vm_region_fill(&p->vm_regions[slot], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        } else if (base == r.base) {
            vm_region_fill(&p->vm_regions[slot], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
            vm_region_fill_span_from_parent(&p->vm_regions[slot1], &r, end, r_end - end,
                                            0, r.flags, 0);
        } else if (end == r_end) {
            vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, r.base,
                                            base - r.base, 0, r.flags, 0);
            vm_region_fill(&p->vm_regions[slot1], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        } else {
            vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, r.base,
                                            base - r.base, 0, r.flags, 0);
            vm_region_fill(&p->vm_regions[slot1], base, length, prot, committed_flags,
                           r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
            vm_region_fill_span_from_parent(&p->vm_regions[slot2], &r, end, r_end - end,
                                            0, r.flags, 0);
        }
        vm_region_merge_neighbors(p);
        return 0;
    }

    if (!vm_range_mapped(p, base, length)) return -1;

    uint64_t pte_flags = vm_prot_to_pte_flags(prot);
    for (uint64_t va = base; va < base + length; va += PAGE_SIZE) {
        if (vmm_protect_user(p->cr3, va, pte_flags) != 0) return -1;
    }

    if (base == r.base && end == r_end) {
        vm_region_fill(&p->vm_regions[slot], r.base, r.length, prot, r.flags,
                       r.kind, r.backing_id, r.backing_page_start, 1);
    } else if (base == r.base) {
        vm_region_fill(&p->vm_regions[slot], base, length, prot, r.flags,
                       r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        vm_region_fill_span_from_parent(&p->vm_regions[slot1], &r, end, r_end - end,
                                        r.prot, r.flags, 1);
    } else if (end == r_end) {
        vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, r.base, base - r.base,
                                        r.prot, r.flags, 1);
        vm_region_fill(&p->vm_regions[slot1], base, length, prot, r.flags,
                       r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
    } else {
        vm_region_fill_span_from_parent(&p->vm_regions[slot], &r, r.base, base - r.base,
                                        r.prot, r.flags, 1);
        vm_region_fill(&p->vm_regions[slot1], base, length, prot, r.flags,
                       r.kind, r.backing_id, vm_region_page_start_at(&r, base), 1);
        vm_region_fill_span_from_parent(&p->vm_regions[slot2], &r, end, r_end - end,
                                        r.prot, r.flags, 1);
    }
    vm_region_merge_neighbors(p);
    return 0;
}

void syscall_vm_process_exit(struct fry_process *p) {
    if (!p) return;
    for (int i = 0; i < PROC_VMREG_MAX; i++) {
        struct fry_vm_region *r = &p->vm_regions[i];
        if (!r->used) continue;
        if (r->committed && r->kind == FRY_VM_REGION_ANON_SHARED) {
            vm_shared_release_range(r->backing_id, r->backing_page_start,
                                    (uint32_t)(r->length / PAGE_SIZE));
        }
        vm_region_clear(r);
    }
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    struct fry_process *cur = proc_current();
    note_user_boot_progress(cur);
    switch (num) {
        case SYS_WRITE: {
            int fd = (int)a1;
            if (!user_buf_mapped(cur, a2, a3)) {
                return (uint64_t)-EFAULT;
            }
            const char *buf = (const char *)(uintptr_t)a2;
            if (fd == 1) {
                int tw_msg = is_taterwin_msg(buf, a3);
                /* In GUI mode, user stdout should be window-routed only.
                   Keep direct console rendering only for no-GUI fallback mode. */
                if (!tw_msg && !gui_process_running()) {
                    kprint_write(buf, a3);
                }
                /* also capture into the per-process stdout ring buffer */
                if (cur) {
                    for (uint64_t _i = 0; _i < a3; _i++) {
                        uint32_t nt = (cur->outbuf_tail + 1u) % PROC_OUTBUF;
                        if (nt != cur->outbuf_head) {
                            cur->outbuf[cur->outbuf_tail] = (uint8_t)buf[_i];
                            cur->outbuf_tail = nt;
                        }
                        /* ring full: silently drop oldest byte */
                    }
                }
                return a3;
            }
            if (fd == 2) {
                kprint_serial_write(buf, a3);
                return a3;
            }
            if (fd >= 3 && cur && fd < 64 && cur->fd_ptrs[fd]) {
                return (uint64_t)vfs_write((struct vfs_file *)cur->fd_ptrs[fd], (const void *)buf, (uint32_t)a3);
            }
            return (uint64_t)-EBADF;
        }
        case SYS_READ: {
            int fd = (int)a1;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char *buf = (char *)(uintptr_t)a2;
            if (fd == 0) {
                if (cur && cur->inbuf_head != cur->inbuf_tail) {
                    uint64_t nr = 0;
                    while (nr < a3 && cur->inbuf_head != cur->inbuf_tail) {
                        buf[nr++] = (char)cur->inbuf[cur->inbuf_head];
                        cur->inbuf_head = (cur->inbuf_head + 1u) % PROC_INBUF;
                    }
                    return nr;
                }
                int n = ps2_kbd_read(buf, (uint32_t)a3);
                if (n > 0) return (uint64_t)n;
                uint64_t sn = kread_serial(buf, a3);
                if (sn > 0) return sn;
                // No input from either source - yield so the scheduler
                // can give other processes (or the idle loop) a turn.
                // This prevents the GUI from busy-spinning on SYS_READ.
                if (cur) sched_yield();
                return 0;
            }
            if (fd >= 3 && cur && fd < 64 && cur->fd_ptrs[fd]) {
                return (uint64_t)vfs_read((struct vfs_file *)cur->fd_ptrs[fd], buf, (uint32_t)a3);
            }
            return (uint64_t)-EBADF;
        }
        case SYS_EXIT:
            if (cur) {
                syscall_exit_current((uint32_t)a1);
            }
            return 0;
        case SYS_SPAWN: {
            char path[128];
            /* Row 1: yellow = spawn entered */
            uint32_t col = g_spawn_attempt_count;
            if (col < 80) boot_diag_color(col, 1, 0x00FFFF00u);
            if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("SPAWN_ENTER path=");
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) {
                /* Row 1: red = copy_user_string failed */
                if (col < 80) boot_diag_color(col, 1, 0x00FF0000u);
                if (col < 80) boot_diag_color(col, 2, 0x00FFFFFFu);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("(copy_fail)\n");
                kprint_serial_only("SPAWN_FAIL path=(copy_fail) rc=%d\n", -EFAULT);
                g_spawn_attempt_count++;
                return (uint64_t)-EFAULT;
            }
            if (TATER_BOOT_SERIAL_TRACE) {
                early_serial_puts(path);
                early_serial_puts("\n");
            }
            int rc = process_launch(path);
            if (rc >= 0) {
                /* Row 1: green = spawn succeeded */
                if (col < 80) boot_diag_color(col, 1, 0x0000FF00u);
                if (col < 80) boot_diag_color(col, 2, 0x0000FF00u);
                if (TATER_BOOT_SERIAL_TRACE) {
                    kprint_serial_only("SPAWN_OK path=%s pid=%d\n", path, rc);
                }
            } else {
                /* Row 1: red = spawn failed (file not found or ELF error) */
                if (col < 80) boot_diag_color(col, 1, 0x00FF0000u);
                if (col < 80) boot_diag_color(col, 2, boot_diag_spawn_error_color(rc));
                kprint_serial_only("SPAWN_FAIL path=%s rc=%d\n", path, rc);
            }
            g_spawn_attempt_count++;
            if (rc >= 0 &&
                cur &&
                !g_first_init_gui_spawn_seen &&
                process_name_is_init(cur->name) &&
                process_name_is_gui(path)) {
                g_first_init_gui_spawn_seen = 1;
                boot_diag_stage(38);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_INIT_GUI_SPAWN\n");
            }
            return (uint64_t)rc;
        }
        case SYS_SLEEP:
            if (cur) {
                sched_sleep(cur->pid, a1);
                sched_yield();
            }
            return 0;
        case SYS_OPEN: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            if (!cur) return (uint64_t)-ESRCH;
            int flags = (int)a2;
            for (int fd = 3; fd < 64; fd++) {
                if (!cur->fd_ptrs[fd]) {
                    struct vfs_file *f = vfs_open(path);
                    if (!f && (flags & 0x40)) {  /* O_CREAT */
                        vfs_create(path, 1);  /* TOTFS_TYPE_FILE */
                        f = vfs_open(path);
                    }
                    if (!f) return (uint64_t)-ENOENT;
                    cur->fd_ptrs[fd] = f;
                    cur->fd_table[fd] = 1;
                    cur->open_fds++;
                    return (uint64_t)fd;
                }
            }
            return (uint64_t)-EMFILE;
        }
        case SYS_CLOSE:
            if (!cur) return (uint64_t)-ESRCH;
            if (a1 >= 3 && a1 < 64 && cur->fd_ptrs[a1]) {
                vfs_close((struct vfs_file *)cur->fd_ptrs[a1]);
                cur->fd_ptrs[a1] = 0;
                cur->fd_table[a1] = -1;
                if (cur->open_fds > 0) cur->open_fds--;
                return 0;
            }
            return (uint64_t)-EBADF;
        case SYS_GETPID:
            return cur ? cur->pid : 0;
        case SYS_STAT:
            if (!user_buf_writable(cur, a2, sizeof(struct vfs_stat))) return (uint64_t)-EFAULT;
            {
                char path[128];
                if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
                struct vfs_stat st;
                if (vfs_stat(path, &st) != 0) return (uint64_t)-ENOENT;
                struct vfs_stat *u = (struct vfs_stat *)(uintptr_t)a2;
                *u = st;
                return 0;
        }
        case SYS_READDIR: {
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            struct readdir_ctx ctx = {(char *)(uintptr_t)a2, (uint32_t)a3, 0};
            if (vfs_readdir(path, readdir_cb, &ctx) != 0) return (uint64_t)-ENOENT;
            if (ctx.pos < ctx.len) ctx.buf[ctx.pos] = 0;
            return ctx.pos;
        }
        case SYS_READDIR_EX: {
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            struct readdir_ex_ctx ctx = {(uint8_t *)(uintptr_t)a2, (uint32_t)a3, 0};
            if (vfs_readdir_ex(path, readdir_ex_cb, &ctx) != 0) return (uint64_t)-ENOENT;
            return ctx.pos;
        }
        case SYS_GETTIME: {
            uint64_t freq = hpet_get_freq_hz();
            if (freq == 0) return 0;
            uint64_t ms = (hpet_read_counter() * 1000ULL) / freq;
            return ms;
        }
        case SYS_REBOOT:
            acpi_reset();
            return 0;
        case SYS_SHUTDOWN:
            acpi_shutdown();
            return 0;
        case SYS_WAIT: {
            int ret = process_wait((uint32_t)a1);
            if (ret < 0) return (uint64_t)-ESRCH;
            if (ret > 0) {
                sched_yield();
            }
            return 0;
        }
        case SYS_PROCCOUNT:
            return (uint64_t)process_count();
        case SYS_SETBRIGHT:
            return (uint64_t)acpi_backlight_set((uint32_t)a1);
        case SYS_GETBRIGHT:
            return (uint64_t)acpi_backlight_get();
        case SYS_GETBATTERY:
            if (!user_buf_writable(cur, a1, sizeof(struct fry_battery_status))) return (uint64_t)-EFAULT;
            {
                struct fry_battery_status st;
                if (acpi_battery_get(&st) != 0) return (uint64_t)-EIO;
                struct fry_battery_status *u = (struct fry_battery_status *)(uintptr_t)a1;
                *u = st;
                return 0;
            }
        case SYS_FB_INFO: {
            if (cur && !g_first_gui_fb_seen && process_name_is_gui(cur->name)) {
                g_first_gui_fb_seen = 1;
                boot_diag_stage(39);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_GUI_FB\n");
            }
            if (!user_buf_writable(cur, a1, sizeof(struct fry_fb_info))) return (uint64_t)-EFAULT;
            if (!g_handoff || !g_handoff->fb_base || !g_handoff->fb_width || !g_handoff->fb_height) {
                return (uint64_t)-ENXIO;
            }
            struct fry_fb_info *info = (struct fry_fb_info *)(uintptr_t)a1;
            uint64_t size = g_handoff->fb_stride * g_handoff->fb_height * 4ULL;
            info->phys = g_handoff->fb_base;
            info->size = size;
            info->user_base = FB_USER_BASE;
            info->width = (uint32_t)g_handoff->fb_width;
            info->height = (uint32_t)g_handoff->fb_height;
            info->stride = (uint32_t)g_handoff->fb_stride;
            info->format = g_handoff->fb_pixel_format;
            return 0;
        }
        case SYS_FB_MAP: {
            if (cur && !g_first_gui_fb_seen && process_name_is_gui(cur->name)) {
                g_first_gui_fb_seen = 1;
                boot_diag_stage(39);
                if (TATER_BOOT_SERIAL_TRACE) early_serial_puts("K_GUI_FB\n");
            }
            if (!g_handoff || !g_handoff->fb_base || !g_handoff->fb_width || !g_handoff->fb_height) {
                return (uint64_t)-ENXIO;
            }
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t size = g_handoff->fb_stride * g_handoff->fb_height * 4ULL;
            uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t phys = g_handoff->fb_base;
            for (uint64_t i = 0; i < pages; i++) {
                vmm_map_user(cur->cr3,
                             FB_USER_BASE + i * PAGE_SIZE,
                             phys + i * PAGE_SIZE,
                             VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_NOFREE);
                __asm__ volatile("invlpg (%0)" : : "r"(FB_USER_BASE + i * PAGE_SIZE) : "memory");
            }
            return FB_USER_BASE;
        }
        case SYS_PROC_OUTPUT: {
            /* a1=pid, a2=user buf, a3=maxlen
               Returns: >0 bytes read, 0 alive/no data,
                        (uint64_t)-2 process dead+empty, (uint64_t)-1 bad ptr */
            uint32_t tpid = (uint32_t)a1;
            if (!user_buf_writable(cur, a2, a3)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a2;
            struct fry_process *tp = 0;
            for (uint32_t _i = 0; _i < PROC_MAX; _i++) {
                if (procs[_i].pid == tpid &&
                    procs[_i].state != PROC_UNUSED) {
                    tp = &procs[_i];
                    break;
                }
            }
            if (!tp) return (uint64_t)-ESRCH; /* never existed or fully freed */
            uint64_t nr = 0;
            while (nr < a3 && tp->outbuf_head != tp->outbuf_tail) {
                ubuf[nr++] = (char)tp->outbuf[tp->outbuf_head];
                tp->outbuf_head = (tp->outbuf_head + 1u) % PROC_OUTBUF;
            }
            if (nr > 0) return nr;
            if (tp->state == PROC_DEAD) return (uint64_t)-ESRCH; /* dead + empty */
            return 0; /* alive, no output yet */
        }
        case SYS_MOUSE_GET: {
            // struct fry_mouse_state {
            //   int32_t x, y, dx, dy;   // offsets 0,4,8,12
            //   uint8_t btns, _pad[3];  // offset 16
            // }  size = 20 bytes
            if (!user_buf_writable(cur, a1, 20)) return (uint64_t)-EFAULT;
            int32_t mx, my, mdx, mdy;
            uint8_t mb;
            ps2_mouse_get(&mx, &my, &mb, &mdx, &mdy);
            int32_t *out = (int32_t *)(uintptr_t)a1;
            out[0] = mx;
            out[1] = my;
            out[2] = mdx;
            out[3] = mdy;
            *((uint8_t *)(out + 4)) = mb;
            return 0;
        }
        case SYS_PROC_INPUT: {
            /* a1=pid, a2=user buf, a3=len
               Returns: bytes written, -1 error */
            uint32_t tpid = (uint32_t)a1;
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-EFAULT;
            const uint8_t *ubuf = (const uint8_t *)(uintptr_t)a2;
            struct fry_process *tp = 0;
            for (uint32_t _i = 0; _i < PROC_MAX; _i++) {
                if (procs[_i].pid == tpid && procs[_i].state != PROC_UNUSED) {
                    tp = &procs[_i];
                    break;
                }
            }
            if (!tp || tp->state == PROC_DEAD) return (uint64_t)-ESRCH;
            uint64_t nw = 0;
            for (uint64_t _i = 0; _i < a3; _i++) {
                uint32_t nt = (tp->inbuf_tail + 1u) % PROC_INBUF;
                if (nt != tp->inbuf_head) {
                    tp->inbuf[tp->inbuf_tail] = ubuf[_i];
                    tp->inbuf_tail = nt;
                    nw++;
                } else {
                    break; // Buffer full
                }
            }
            return nw;
        }
        case SYS_SBRK: {
            if (!cur) return (uint64_t)-ESRCH;
            int64_t inc = (int64_t)a1;
            uint64_t old_end = cur->heap_end;
            if (inc == 0) return old_end;
            if (inc < 0) return (uint64_t)-EINVAL; // No shrinking for now
            uint64_t new_end = old_end + (uint64_t)inc;
            if (new_end < old_end || new_end > USER_TOP) return (uint64_t)-ENOMEM;
            uint64_t old_page_end = (old_end + 4095ULL) & ~4095ULL;
            uint64_t new_page_end = (new_end + 4095ULL) & ~4095ULL;
            if (new_page_end > USER_TOP) return (uint64_t)-ENOMEM;
            for (uint64_t va = old_page_end; va < new_page_end; va += 4096ULL) {
                /* Heap growth must only map fresh pages in this range. */
                if (vmm_virt_to_phys_user(cur->cr3, va) != 0) {
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-ENOMEM;
                }
                uint64_t pa = pmm_alloc_page();
                if (!pa) {
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-ENOMEM;
                }
                vmm_map_user(cur->cr3, va, pa, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
                uint64_t mapped_pa = vmm_virt_to_phys_user(cur->cr3, va);
                if ((mapped_pa & 0x000FFFFFFFFFF000ULL) != pa) {
                    if (mapped_pa) {
                        vmm_unmap_user(cur->cr3, va);
                    }
                    pmm_free_page(pa);
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-ENOMEM;
                }
                uint8_t *kv = (uint8_t *)vmm_phys_to_virt(pa);
                for (int i = 0; i < 4096; i++) kv[i] = 0;
            }
            cur->heap_end = new_end;
            return old_end;
        }
        case SYS_SHM_ALLOC: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t size = (uint64_t)a1;
            uint32_t pages = (size + 4095ULL) / 4096ULL;
            if (pages == 0) return (uint64_t)-EINVAL;
            for (int i = 0; i < SHM_MAX; i++) {
                if (!shm_regions[i].used) {
                    uint64_t phys = 0;
                    // Allocate contiguous physical pages for simple mapping
                    // Actually, let's just allocate page by page and map them.
                    // But for SHM, we usually want a contiguous physical range or 
                    // a list of pages. Let's just do a simple contiguous allocation from PMM
                    // if possible, or just a linked list. 
                    // Actually, our PMM is a bitmap, we can do contiguous.
                    // TODO: contiguous PMM alloc. 
                    // For now, I'll just support 1 page or hack it.
                    // Wait, let's just allocate pages and store them.
                    // To keep it simple, I'll just allocate one big chunk from PMM.
                    phys = pmm_alloc_pages(pages);
                    if (!phys) return (uint64_t)-ENOMEM;
                    for (uint32_t s = 0; s < PROC_MAX; s++) {
                        shm_regions[i].mapped_pids[s] = 0;
                    }
                    shm_regions[i].phys_base = phys;
                    shm_regions[i].page_count = pages;
                    shm_regions[i].owner_pid = cur->pid;
                    shm_regions[i].map_count = 0;
                    shm_regions[i].used = 1;
                    return (uint64_t)i;
                }
            }
            return (uint64_t)-ENFILE;
        }
        case SYS_SHM_MAP: {
            int id = (int)a1;
            if (id < 0 || id >= SHM_MAX || !shm_regions[id].used || !cur) return (uint64_t)-EINVAL;
            int slot = shm_proc_slot_by_pid(cur->pid);
            if (slot < 0) return (uint64_t)-ENOMEM;
            // Map at a high address
            uint64_t virt = SHM_USER_BASE + (uint64_t)id * SHM_SLOT_STRIDE; // 256MB apart
            for (uint32_t i = 0; i < shm_regions[id].page_count; i++) {
                vmm_map_user(cur->cr3, virt + i * 4096ULL, shm_regions[id].phys_base + i * 4096ULL,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE | VMM_FLAG_NOFREE);
            }
            if (shm_regions[id].mapped_pids[slot] != cur->pid) {
                shm_regions[id].mapped_pids[slot] = cur->pid;
                if (shm_regions[id].map_count != 0xFFFFFFFFU) {
                    shm_regions[id].map_count++;
                }
            }
            return virt;
        }
        case SYS_SHM_FREE: {
            int id = (int)a1;
            if (!cur || id < 0 || id >= SHM_MAX || !shm_regions[id].used) return (uint64_t)-EINVAL;
            if (shm_regions[id].owner_pid != cur->pid) return (uint64_t)-EPERM;
            shm_destroy_region(id);
            return 0;
        }
        case SYS_KILL: {
            uint32_t tpid = (uint32_t)a1;
            /* Don't allow killing pid 0, 1, or self */
            if (tpid <= 1 || (cur && tpid == cur->pid)) return (uint64_t)-EPERM;
            struct fry_process *tp = 0;
            for (uint32_t _i = 0; _i < PROC_MAX; _i++) {
                if (procs[_i].pid == tpid && procs[_i].state != PROC_UNUSED) {
                    tp = &procs[_i];
                    break;
                }
            }
            if (!tp) return (uint64_t)-ESRCH;
            if (tp->state == PROC_DEAD) return 0;
            proc_free(tpid);
            sched_yield();
            return 0;
        }
        case SYS_ACPI_DIAG: {
            if (!user_buf_writable(cur, a1, sizeof(struct fry_acpi_diag))) return (uint64_t)-EFAULT;
            struct fry_acpi_diag diag;
            if (acpi_get_diag(&diag) != 0) return (uint64_t)-EIO;
            struct fry_acpi_diag *u = (struct fry_acpi_diag *)(uintptr_t)a1;
            *u = diag;
            return 0;
        }
        case SYS_CREATE: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            uint16_t type = (uint16_t)a2;
            return (uint64_t)vfs_create(path, type);
        }
        case SYS_MKDIR: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            return (uint64_t)vfs_mkdir(path);
        }
        case SYS_UNLINK: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            return (uint64_t)vfs_unlink(path);
        }
        case SYS_STORAGE_INFO: {
            if (!user_buf_writable(cur, a1, sizeof(struct vfs_storage_info))) return (uint64_t)-EFAULT;
            struct vfs_storage_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_storage_info(&info) != 0) return (uint64_t)-EIO;
            struct vfs_storage_info *u = (struct vfs_storage_info *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_PATH_FS_INFO: {
            if (!user_buf_writable(cur, a2, sizeof(struct vfs_path_fs_info))) return (uint64_t)-EFAULT;
            char path[192];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-EFAULT;
            struct vfs_path_fs_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_path_fs_info(path, &info) != 0) return (uint64_t)-ENOENT;
            struct vfs_path_fs_info *u = (struct vfs_path_fs_info *)(uintptr_t)a2;
            *u = info;
            return 0;
        }
        case SYS_MOUNTS_INFO: {
            if (!user_buf_writable(cur, a1, sizeof(struct vfs_mounts_info))) return (uint64_t)-EFAULT;
            struct vfs_mounts_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_mounts_info(&info) != 0) return (uint64_t)-EIO;
            struct vfs_mounts_info *u = (struct vfs_mounts_info *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_MOUNTS_DEBUG: {
            if (!user_buf_writable(cur, a1, sizeof(struct vfs_mounts_dbg))) return (uint64_t)-EFAULT;
            struct vfs_mounts_dbg info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_mounts_dbg(&info) != 0) return (uint64_t)-EIO;
            struct vfs_mounts_dbg *u = (struct vfs_mounts_dbg *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_WIFI_STATUS: {
            if (!user_buf_writable(cur, a1, sizeof(struct fry_wifi_status))) return (uint64_t)-EFAULT;
            struct fry_wifi_status info;
            if (wifi_9260_get_user_status(&info) != 0) return (uint64_t)-EIO;
            struct fry_wifi_status *u = (struct fry_wifi_status *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_WIFI_SCAN: {
            if (!user_buf_writable(cur, a3, sizeof(uint32_t))) return (uint64_t)-EFAULT;
            if (a2 > FRY_WIFI_MAX_SCAN) a2 = FRY_WIFI_MAX_SCAN;
            if (a2 == 0) {
                *(uint32_t *)(uintptr_t)a3 = 0;
                return 0;
            }
            uint64_t bytes = a2 * (uint64_t)sizeof(struct fry_wifi_scan_entry);
            if (!user_buf_writable(cur, a1, bytes)) return (uint64_t)-EFAULT;
            uint32_t count = 0;
            int rc = wifi_9260_get_scan_entries((struct fry_wifi_scan_entry *)(uintptr_t)a1,
                                                (uint32_t)a2, &count);
            if (rc != 0) return (uint64_t)rc;
            *(uint32_t *)(uintptr_t)a3 = count;
            return 0;
        }
        case SYS_WIFI_CONNECT: {
            char ssid[FRY_WIFI_SSID_MAX + 1];
            char passphrase[96];
            if (copy_user_string(cur, a1, ssid, sizeof(ssid)) != 0) return (uint64_t)-EFAULT;
            if (copy_user_string(cur, a2, passphrase, sizeof(passphrase)) != 0) return (uint64_t)-EFAULT;
            return (uint64_t)wifi_9260_connect_user(ssid, passphrase);
        }
        case SYS_WIFI_DEBUG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            int n = wifi_9260_get_debug_log(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_CPU_STATUS: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 128 || bufsz > 8192) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_cpu_status(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_cpu_status(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_INIT_LOG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_init_log(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_init_log(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_DEBUG2: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_debug_log2(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_debug_log2(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_HANDOFF: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 192 || bufsz > 4096) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_handoff_status(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_handoff_status(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_DEBUG3: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz == 0 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_debug_log3(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_debug_log3(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_REINIT: {
            extern int wifi_9260_reinit_user(void);
            return (uint64_t)wifi_9260_reinit_user();
        }
        case SYS_WIFI_CMD_TRACE: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 128 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_cmd_trace(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_cmd_trace(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_SRAM: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_sram_dump(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_sram_dump(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_DEEP_DIAG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_deep_diag(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_deep_diag(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_WIFI_VERIFY: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int wifi_9260_get_verify_result(char *buf, uint32_t bufsz);
            int n = wifi_9260_get_verify_result(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_ETH_DIAG: {
            uint32_t bufsz = (uint32_t)a2;
            if (bufsz < 256 || bufsz > FRY_WIFI_DEBUG_MAX) return (uint64_t)-EINVAL;
            if (!user_buf_writable(cur, a1, bufsz)) return (uint64_t)-EFAULT;
            char *ubuf = (char *)(uintptr_t)a1;
            extern int i219_get_diag(char *buf, uint32_t bufsz);
            int n = i219_get_diag(ubuf, bufsz);
            return (uint64_t)n;
        }
        case SYS_MMAP: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t hint = a1;
            uint64_t length = a2;
            uint32_t prot = (uint32_t)a3;
            uint32_t flags = (uint32_t)a4;
            int fd = (int)a5;
            if (length == 0 || length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (length == 0) return (uint64_t)-EINVAL;
            if (!vm_prot_supported(prot) || !vm_flags_supported(flags)) return (uint64_t)-EINVAL;
            if ((flags & FRY_MAP_RESERVE) != 0 && prot != 0) return (uint64_t)-EINVAL;
            if ((flags & FRY_MAP_GUARD) != 0 && prot != 0) return (uint64_t)-EINVAL;
            if (hint != 0 && (hint & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;

            uint64_t base = 0;
            if ((flags & FRY_MAP_FIXED) != 0) {
                base = hint;
                if (!vm_range_available(cur, base, length)) return (uint64_t)-ENOMEM;
            } else if (hint != 0 && vm_range_available(cur, hint, length)) {
                base = hint;
            } else {
                base = vm_find_free_range(cur, length);
            }
            if (!base) return (uint64_t)-ENOMEM;
            if ((flags & FRY_MAP_GUARD) != 0) {
                if (vm_map_guard_region(cur, base, length, flags) != 0) return (uint64_t)-ENOMEM;
            } else if ((flags & FRY_MAP_ANON) != 0) {
                if ((flags & FRY_MAP_SHARED) != 0) {
                    if (vm_map_anon_shared_region(cur, base, length, prot, flags) != 0) return (uint64_t)-ENOMEM;
                } else {
                    if (vm_map_anon_private_region(cur, base, length, prot, flags) != 0) return (uint64_t)-ENOMEM;
                }
            } else {
                if (vm_map_file_region(cur, base, length, prot, flags, fd) != 0) return (uint64_t)-ENOMEM;
            }
            return base;
        }
        case SYS_MUNMAP: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t base = a1;
            uint64_t length = a2;
            if ((base & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;
            if (length == 0 || length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (length == 0) return (uint64_t)-EINVAL;
            return (uint64_t)vm_unmap_region_range(cur, base, length);
        }
        case SYS_MPROTECT: {
            if (!cur) return (uint64_t)-ESRCH;
            uint64_t base = a1;
            uint64_t length = a2;
            uint32_t prot = (uint32_t)a3;
            if ((base & (PAGE_SIZE - 1ULL)) != 0) return (uint64_t)-EINVAL;
            if (length == 0 || length > UINT64_MAX - (PAGE_SIZE - 1ULL)) return (uint64_t)-EINVAL;
            length = (length + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
            if (length == 0) return (uint64_t)-EINVAL;
            if (!vm_prot_supported(prot)) return (uint64_t)-EINVAL;
            return (uint64_t)vm_mprotect_region_range(cur, base, length, prot);
        }
        default:
            return (uint64_t)-ENOSYS;
    }
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void syscall_init(void) {
    // Enable SYSCALL/SYSRET: set EFER.SCE (bit 0) in MSR 0xC0000080
    uint32_t efer_lo, efer_hi;
    __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080u));
    wrmsr(0xC0000080, ((uint64_t)efer_hi << 32 | efer_lo) | 1ULL);

    // IA32_STAR: SYSCALL -> CS=0x08 SS=0x10; SYSRETQ -> CS=0x2B SS=0x23
    // STAR[47:32]=0x08 (kernel), STAR[63:48]=0x18 (+16|3=0x2B, +8|3=0x23)
    uint64_t star = ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(0xC0000081, star);
    wrmsr(0xC0000082, (uint64_t)(uintptr_t)&syscall_entry);
    // IA32_FMASK: clear per-user flags on SYSCALL entry.
    // IF  (bit 9): enter with IRQs masked until syscall_entry explicitly enables.
    // TF  (bit 8): prevent user single-step from trapping kernel syscall path.
    // DF (bit 10): guarantee forward string ops in kernel.
    wrmsr(0xC0000084, 0x700);

    // Seed the kernel stack pointer with the current RSP so there's always a
    // valid value even before the first sched_tick() switch.  sched_tick()
    // will overwrite this with the actual per-process kernel_stack_top as soon
    // as the first user process is scheduled.
    uint64_t cur_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(cur_rsp));
    g_syscall_kstack_top = cur_rsp;
}

// syscall entry
//
// The SYSCALL instruction does NOT switch the stack pointer; RSP remains the
// user-space RSP.  Any call to sched_yield() / context_switch() while on the
// user stack would save a user-space RSP into the process's saved_rsp, which
// then gets loaded back in kernel mode by the next sched_tick() → #PF storm.
//
// Fix: switch to the per-process kernel stack (g_syscall_kstack_top, kept in
// sync by sched_tick) immediately on entry, before touching anything else.
// The user RSP is spilled to g_syscall_user_rsp (IF=0 during entry, so safe
// on BSP), then pushed onto the kernel stack so it survives context switches.
// Re-enable IRQs while syscall_dispatch runs so timer/scheduler keep advancing
// even if a syscall performs slow storage I/O. IRQs are masked again before
// restoring the SYSRET frame.
// (SFMASK clears IF on SYSCALL entry; SYSRET restores user RFLAGS from R11.)
__asm__(
    ".global syscall_entry\n"
    "syscall_entry:\n"
    // IF is already 0 (SFMASK clears it at SYSCALL instruction).
    // RSP is still the user RSP.

    // 1. Spill user RSP into scratch global, then switch to kernel stack.
    "    mov %rsp, g_syscall_user_rsp(%rip)\n"
    "    mov g_syscall_kstack_top(%rip), %rsp\n"

    // 2. Save the values we'll need to restore for sysretq, plus user RSP.
    //    Push in this order so we can pop symmetrically at the end.
    "    push g_syscall_user_rsp(%rip)\n"  // [rbp-8]  : user RSP
    "    push %rcx\n"                       // user RIP (return address)
    "    push %r11\n"                       // user RFLAGS
    "    push %rbp\n"
    "    mov %rsp, %rbp\n"

    // 3. Shuffle registers into syscall_dispatch ABI (num,a1,a2,a3,a4,a5).
    "    mov %r8,  %r9\n"   // a5
    "    mov %r10, %r8\n"   // a4
    "    mov %rdx, %rcx\n"  // a3
    "    mov %rsi, %rdx\n"  // a2
    "    mov %rdi, %rsi\n"  // a1
    "    mov %rax, %rdi\n"  // num

    // Allow IRQ-driven timers/scheduler while syscall body runs.
    // We re-mask IRQs before restoring user return frame.
    "    sti\n"
    "    call syscall_dispatch\n"
    "    cli\n"

    // 4. Restore frame pointer and the three saved values.
    "    mov %rbp, %rsp\n"
    "    pop %rbp\n"
    "    pop %r11\n"         // user RFLAGS
    "    pop %rcx\n"         // user RIP
    "    pop %rsp\n"         // user RSP  (back to user stack)
    "    sysretq\n"
);
