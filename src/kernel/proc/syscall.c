// Syscall layer

#include <stdint.h>
#include "syscall.h"
#include "process.h"
#include "sched.h"
#include "../fs/vfs.h"
#include "../acpi/extended.h"
#include "../mm/pmm.h"
#include "../../drivers/timer/hpet.h"
#include "../mm/vmm.h"
#include "../../boot/efi_handoff.h"
#include "../../boot/early_serial.h"

void kprint_write(const char *buf, uint64_t len);
void kprint_serial_only(const char *fmt, ...);
void kprint_serial_write(const char *buf, uint64_t len);
uint64_t kread_serial(char *buf, uint64_t len);
int ps2_kbd_read(char *buf, uint32_t len);
void ps2_mouse_get(int32_t *x, int32_t *y, uint8_t *btns,
                   int32_t *dx, int32_t *dy);
void acpi_reset(void);
void acpi_shutdown(void);
extern void syscall_entry(void);
extern struct fry_handoff *g_handoff;

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
/*
 * SYS_EXIT must not free the currently active process CR3/stack while still
 * executing on them.  Use a dedicated kernel-owned stack for exit teardown.
 */
static uint8_t g_sys_exit_stack[16384] __attribute__((aligned(16)));

#define USER_TOP USER_VA_TOP
#define PAGE_SIZE 4096ULL
#define FB_USER_BASE 0x0000000100000000ULL

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

static int user_buf_mapped(struct fry_process *p, uint64_t ptr, uint64_t len) {
    if (!p || !p->cr3) return 0;
    if (!user_ptr_ok(ptr, len)) return 0;
    if (len == 0) return 1;

    uint64_t va = ptr & ~(PAGE_SIZE - 1ULL);
    uint64_t last = (ptr + len - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (;;) {
        if (vmm_virt_to_phys_user(p->cr3, va) == 0) return 0;
        if (va == last) break;
        if (va > USER_TOP - PAGE_SIZE) return 0;
        va += PAGE_SIZE;
    }
    return 1;
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
            if (vmm_virt_to_phys_user(p->cr3, va) == 0) return -1;
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
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;
    if (!handoff->boot_identity_limit || handoff->fb_base >= handoff->boot_identity_limit) return;

    uint64_t x0 = stage * 20ULL;
    if (x0 >= handoff->fb_width) return;

    uint64_t mw = 12ULL;
    uint64_t mh = 12ULL;
    uint64_t remain_w = handoff->fb_width - x0;
    if (remain_w < mw) mw = remain_w;
    if (handoff->fb_height < mh) mh = handoff->fb_height;

    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)handoff->fb_base;
    for (uint64_t y = 0; y < mh; y++) {
        uint64_t row = y * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < mw; x++) {
            fb[row + x] = 0x00F0F0F0u;
        }
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
        early_serial_puts("K_FIRST_USER_SYSCALL\n");
    }
    if (!g_first_init_syscall_seen && process_name_is_init(cur->name)) {
        g_first_init_syscall_seen = 1;
        boot_diag_stage(37);
        early_serial_puts("K_INIT_SYSCALL\n");
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

enum {
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
    SYS_STORAGE_INFO = 32,
    SYS_PATH_FS_INFO = 33,
    SYS_MOUNTS_INFO = 34,
    SYS_READDIR_EX = 35,
    SYS_MOUNTS_DEBUG = 36
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

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    struct fry_process *cur = proc_current();
    note_user_boot_progress(cur);
    switch (num) {
        case SYS_WRITE: {
            int fd = (int)a1;
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-1;
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
            return (uint64_t)-1;
        }
        case SYS_READ: {
            int fd = (int)a1;
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-1;
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
            return (uint64_t)-1;
        }
        case SYS_EXIT:
            if (cur) {
                syscall_exit_current((uint32_t)a1);
            }
            return 0;
        case SYS_SPAWN: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            int rc = process_launch(path);
            if (rc >= 0 &&
                cur &&
                !g_first_init_gui_spawn_seen &&
                process_name_is_init(cur->name) &&
                process_name_is_gui(path)) {
                g_first_init_gui_spawn_seen = 1;
                boot_diag_stage(38);
                early_serial_puts("K_INIT_GUI_SPAWN\n");
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
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            if (!cur) return (uint64_t)-1;
            int flags = (int)a2;
            for (int fd = 3; fd < 64; fd++) {
                if (!cur->fd_ptrs[fd]) {
                    struct vfs_file *f = vfs_open(path);
                    if (!f && (flags & 0x40)) {  /* O_CREAT */
                        vfs_create(path, 1);  /* TOTFS_TYPE_FILE */
                        f = vfs_open(path);
                    }
                    if (!f) return (uint64_t)-1;
                    cur->fd_ptrs[fd] = f;
                    cur->fd_table[fd] = 1;
                    return (uint64_t)fd;
                }
            }
            return (uint64_t)-1;
        }
        case SYS_CLOSE:
            if (!cur) return (uint64_t)-1;
            if (a1 >= 3 && a1 < 64 && cur->fd_ptrs[a1]) {
                vfs_close((struct vfs_file *)cur->fd_ptrs[a1]);
                cur->fd_ptrs[a1] = 0;
                cur->fd_table[a1] = -1;
                return 0;
            }
            return (uint64_t)-1;
        case SYS_GETPID:
            return cur ? cur->pid : 0;
        case SYS_STAT:
            if (!user_buf_mapped(cur, a2, sizeof(struct vfs_stat))) return (uint64_t)-1;
            {
                char path[128];
                if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
                struct vfs_stat st;
                if (vfs_stat(path, &st) != 0) return (uint64_t)-1;
                struct vfs_stat *u = (struct vfs_stat *)(uintptr_t)a2;
                *u = st;
                return 0;
        }
        case SYS_READDIR: {
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-1;
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            struct readdir_ctx ctx = {(char *)(uintptr_t)a2, (uint32_t)a3, 0};
            if (vfs_readdir(path, readdir_cb, &ctx) != 0) return (uint64_t)-1;
            if (ctx.pos < ctx.len) ctx.buf[ctx.pos] = 0;
            return ctx.pos;
        }
        case SYS_READDIR_EX: {
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-1;
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            struct readdir_ex_ctx ctx = {(uint8_t *)(uintptr_t)a2, (uint32_t)a3, 0};
            if (vfs_readdir_ex(path, readdir_ex_cb, &ctx) != 0) return (uint64_t)-1;
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
            if (ret < 0) return (uint64_t)-1;
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
            if (!user_buf_mapped(cur, a1, sizeof(struct fry_battery_status))) return (uint64_t)-1;
            {
                struct fry_battery_status st;
                if (acpi_battery_get(&st) != 0) return (uint64_t)-1;
                struct fry_battery_status *u = (struct fry_battery_status *)(uintptr_t)a1;
                *u = st;
                return 0;
            }
        case SYS_FB_INFO: {
            if (cur && !g_first_gui_fb_seen && process_name_is_gui(cur->name)) {
                g_first_gui_fb_seen = 1;
                boot_diag_stage(39);
                early_serial_puts("K_GUI_FB\n");
            }
            if (!user_buf_mapped(cur, a1, sizeof(struct fry_fb_info))) return (uint64_t)-1;
            if (!g_handoff || !g_handoff->fb_base || !g_handoff->fb_width || !g_handoff->fb_height) {
                return (uint64_t)-1;
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
                early_serial_puts("K_GUI_FB\n");
            }
            if (!g_handoff || !g_handoff->fb_base || !g_handoff->fb_width || !g_handoff->fb_height) {
                return (uint64_t)-1;
            }
            if (!cur) return (uint64_t)-1;
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
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-1;
            char *ubuf = (char *)(uintptr_t)a2;
            struct fry_process *tp = 0;
            for (uint32_t _i = 0; _i < PROC_MAX; _i++) {
                if (procs[_i].pid == tpid &&
                    procs[_i].state != PROC_UNUSED) {
                    tp = &procs[_i];
                    break;
                }
            }
            if (!tp) return (uint64_t)-2; /* never existed or fully freed */
            uint64_t nr = 0;
            while (nr < a3 && tp->outbuf_head != tp->outbuf_tail) {
                ubuf[nr++] = (char)tp->outbuf[tp->outbuf_head];
                tp->outbuf_head = (tp->outbuf_head + 1u) % PROC_OUTBUF;
            }
            if (nr > 0) return nr;
            if (tp->state == PROC_DEAD) return (uint64_t)-2; /* dead + empty */
            return 0; /* alive, no output yet */
        }
        case SYS_MOUSE_GET: {
            // struct fry_mouse_state {
            //   int32_t x, y, dx, dy;   // offsets 0,4,8,12
            //   uint8_t btns, _pad[3];  // offset 16
            // }  size = 20 bytes
            if (!user_buf_mapped(cur, a1, 20)) return (uint64_t)-1;
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
            if (!user_buf_mapped(cur, a2, a3)) return (uint64_t)-1;
            const uint8_t *ubuf = (const uint8_t *)(uintptr_t)a2;
            struct fry_process *tp = 0;
            for (uint32_t _i = 0; _i < PROC_MAX; _i++) {
                if (procs[_i].pid == tpid && procs[_i].state != PROC_UNUSED) {
                    tp = &procs[_i];
                    break;
                }
            }
            if (!tp || tp->state == PROC_DEAD) return (uint64_t)-1;
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
            if (!cur) return (uint64_t)-1;
            int64_t inc = (int64_t)a1;
            uint64_t old_end = cur->heap_end;
            if (inc == 0) return old_end;
            if (inc < 0) return (uint64_t)-1; // No shrinking for now
            uint64_t new_end = old_end + (uint64_t)inc;
            if (new_end < old_end || new_end > USER_TOP) return (uint64_t)-1;
            uint64_t old_page_end = (old_end + 4095ULL) & ~4095ULL;
            uint64_t new_page_end = (new_end + 4095ULL) & ~4095ULL;
            if (new_page_end > USER_TOP) return (uint64_t)-1;
            for (uint64_t va = old_page_end; va < new_page_end; va += 4096ULL) {
                /* Heap growth must only map fresh pages in this range. */
                if (vmm_virt_to_phys_user(cur->cr3, va) != 0) {
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-1;
                }
                uint64_t pa = pmm_alloc_page();
                if (!pa) {
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-1;
                }
                vmm_map_user(cur->cr3, va, pa, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
                uint64_t mapped_pa = vmm_virt_to_phys_user(cur->cr3, va);
                if ((mapped_pa & 0x000FFFFFFFFFF000ULL) != pa) {
                    if (mapped_pa) {
                        vmm_unmap_user(cur->cr3, va);
                    }
                    pmm_free_page(pa);
                    sbrk_rollback_pages(cur, old_page_end, va);
                    return (uint64_t)-1;
                }
                uint8_t *kv = (uint8_t *)vmm_phys_to_virt(pa);
                for (int i = 0; i < 4096; i++) kv[i] = 0;
            }
            cur->heap_end = new_end;
            return old_end;
        }
        case SYS_SHM_ALLOC: {
            if (!cur) return (uint64_t)-1;
            uint64_t size = (uint64_t)a1;
            uint32_t pages = (size + 4095ULL) / 4096ULL;
            if (pages == 0) return (uint64_t)-1;
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
                    if (!phys) return (uint64_t)-1;
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
            return (uint64_t)-1;
        }
        case SYS_SHM_MAP: {
            int id = (int)a1;
            if (id < 0 || id >= SHM_MAX || !shm_regions[id].used || !cur) return (uint64_t)-1;
            int slot = shm_proc_slot_by_pid(cur->pid);
            if (slot < 0) return (uint64_t)-1;
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
            if (!cur || id < 0 || id >= SHM_MAX || !shm_regions[id].used) return (uint64_t)-1;
            if (shm_regions[id].owner_pid != cur->pid) return (uint64_t)-1;
            shm_destroy_region(id);
            return 0;
        }
        case SYS_KILL: {
            uint32_t tpid = (uint32_t)a1;
            /* Don't allow killing pid 0, 1, or self */
            if (tpid <= 1 || (cur && tpid == cur->pid)) return (uint64_t)-1;
            struct fry_process *tp = 0;
            for (uint32_t _i = 0; _i < PROC_MAX; _i++) {
                if (procs[_i].pid == tpid && procs[_i].state != PROC_UNUSED) {
                    tp = &procs[_i];
                    break;
                }
            }
            if (!tp) return (uint64_t)-1;
            if (tp->state == PROC_DEAD) return 0;
            proc_free(tpid);
            sched_yield();
            return 0;
        }
        case SYS_ACPI_DIAG: {
            if (!user_buf_mapped(cur, a1, sizeof(struct fry_acpi_diag))) return (uint64_t)-1;
            struct fry_acpi_diag diag;
            if (acpi_get_diag(&diag) != 0) return (uint64_t)-1;
            struct fry_acpi_diag *u = (struct fry_acpi_diag *)(uintptr_t)a1;
            *u = diag;
            return 0;
        }
        case SYS_CREATE: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            uint16_t type = (uint16_t)a2;
            return (uint64_t)vfs_create(path, type);
        }
        case SYS_MKDIR: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            return (uint64_t)vfs_mkdir(path);
        }
        case SYS_UNLINK: {
            char path[128];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            return (uint64_t)vfs_unlink(path);
        }
        case SYS_STORAGE_INFO: {
            if (!user_buf_mapped(cur, a1, sizeof(struct vfs_storage_info))) return (uint64_t)-1;
            struct vfs_storage_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_storage_info(&info) != 0) return (uint64_t)-1;
            struct vfs_storage_info *u = (struct vfs_storage_info *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_PATH_FS_INFO: {
            if (!user_buf_mapped(cur, a2, sizeof(struct vfs_path_fs_info))) return (uint64_t)-1;
            char path[192];
            if (copy_user_string(cur, a1, path, sizeof(path)) != 0) return (uint64_t)-1;
            struct vfs_path_fs_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_path_fs_info(path, &info) != 0) return (uint64_t)-1;
            struct vfs_path_fs_info *u = (struct vfs_path_fs_info *)(uintptr_t)a2;
            *u = info;
            return 0;
        }
        case SYS_MOUNTS_INFO: {
            if (!user_buf_mapped(cur, a1, sizeof(struct vfs_mounts_info))) return (uint64_t)-1;
            struct vfs_mounts_info info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_mounts_info(&info) != 0) return (uint64_t)-1;
            struct vfs_mounts_info *u = (struct vfs_mounts_info *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        case SYS_MOUNTS_DEBUG: {
            if (!user_buf_mapped(cur, a1, sizeof(struct vfs_mounts_dbg))) return (uint64_t)-1;
            struct vfs_mounts_dbg info;
            uint8_t *p = (uint8_t *)&info;
            for (uint32_t i = 0; i < sizeof(info); i++) p[i] = 0;
            if (vfs_get_mounts_dbg(&info) != 0) return (uint64_t)-1;
            struct vfs_mounts_dbg *u = (struct vfs_mounts_dbg *)(uintptr_t)a1;
            *u = info;
            return 0;
        }
        default:
            return (uint64_t)-1;
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
