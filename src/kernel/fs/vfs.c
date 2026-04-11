// VFS core
#include "vfs.h"
#include "fat32.h"
#include "totfs.h"
#include "ntfs.h"
#include "part.h"
#include "../../boot/efi_handoff.h"
#include "../../drivers/smp/spinlock.h"
void kprint(const char *fmt, ...);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
#define ENABLE_TOTFS 0
#define TATER_PHYSMAP 0xFFFF800000000000ULL
static struct vfs_mount *g_mounts;
static struct fat32_fs   g_fat32;
static struct fs_ops     fat32_ops;
#if ENABLE_TOTFS
static struct totfs_fs   g_totfs;
static struct fs_ops     totfs_ops;
static struct fs_ops     sec_totfs_ops;
#endif
static struct fs_ops     sec_fat32_ops;
static struct fs_ops     sec_ntfs_ops;
static int               sec_ops_inited = 0;
static int               g_nvme_recovering = 0;
static struct block_device *g_storage_bd = 0;
static uint8_t           g_root_from_ramdisk = 0;
static spinlock_t        g_vfs_lock = {0};
#define VFS_OPEN_FILE_POOL_SIZE 256u
static struct vfs_file   g_vfs_file_pool[VFS_OPEN_FILE_POOL_SIZE];
static uint8_t           g_vfs_file_pool_used[VFS_OPEN_FILE_POOL_SIZE];
static int mountpoint_exists(const char *mountpoint);
static int mountpoint_exists_locked(const char *mountpoint);
static int vfs_try_recover_mount(const char *path);
static int path_is_exact_nvme_mountpoint(const char *path);
static int streq(const char *a, const char *b);
static int mount_exists_lba(uint64_t lba);
static int sec_probe_root(struct fs_ops *ops, void *fs_data);
static struct vfs_mount *find_mount_locked(const char *path, uint32_t *out_best_len);
static struct vfs_mount *find_mount_ref(const char *path, const char **out_rel);
static struct vfs_mount *find_mount_exact_ref(const char *mountpoint);
#define SEC_MAX_MOUNTED 8u
// ── Ramdisk backend ──────────────────────────────────────────────────────────
static struct ramdisk_header *g_rd_hdr  = 0;
static uint64_t               g_rd_base = 0;  // virtual address of ramdisk
static uint64_t               g_rd_size = 0;  // ramdisk bytes
static struct block_device    g_rd_bd;
struct rd_readdir_ctx {
    int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx);
    void *ctx;
    char seen[32][32];
    uint32_t seen_count;
    uint8_t matched;
};

static uint64_t vfs_lock_irqsave(void) {
    return spin_lock_irqsave(&g_vfs_lock);
}

static void vfs_unlock_irqrestore(uint64_t flags) {
    spin_unlock_irqrestore(&g_vfs_lock, flags);
}

static void vfs_mem_zero(void *ptr, uint64_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < len; i++) p[i] = 0;
}

static struct vfs_file *vfs_file_alloc(void) {
    uint64_t irqf = vfs_lock_irqsave();
    for (uint32_t i = 0; i < VFS_OPEN_FILE_POOL_SIZE; i++) {
        if (!g_vfs_file_pool_used[i]) {
            g_vfs_file_pool_used[i] = 1;
            vfs_unlock_irqrestore(irqf);
            vfs_mem_zero(&g_vfs_file_pool[i], sizeof(g_vfs_file_pool[i]));
            return &g_vfs_file_pool[i];
        }
    }
    vfs_unlock_irqrestore(irqf);
    return 0;
}

static void vfs_file_free(struct vfs_file *f) {
    if (!f) return;
    uintptr_t base = (uintptr_t)&g_vfs_file_pool[0];
    uintptr_t end = (uintptr_t)(&g_vfs_file_pool[VFS_OPEN_FILE_POOL_SIZE]);
    uintptr_t ptr = (uintptr_t)f;
    if (ptr < base || ptr >= end) return;
    uint64_t idx = (uint64_t)(ptr - base) / (uint64_t)sizeof(struct vfs_file);
    if (idx >= VFS_OPEN_FILE_POOL_SIZE) return;
    if ((base + idx * (uint64_t)sizeof(struct vfs_file)) != ptr) return;

    uint64_t irqf = vfs_lock_irqsave();
    g_vfs_file_pool_used[idx] = 0;
    vfs_unlock_irqrestore(irqf);
    vfs_mem_zero(&g_vfs_file_pool[idx], sizeof(g_vfs_file_pool[idx]));
}

static int rd_bd_read_sector(struct block_device *bd, uint64_t lba, void *buf) {
    (void)bd;
    if (!buf || !g_rd_base || g_rd_size < 512) return -1;
    uint64_t off = lba * 512ull;
    if (off > g_rd_size || (g_rd_size - off) < 512ull) return -1;
    const uint8_t *src = (const uint8_t *)(uintptr_t)(g_rd_base + off);
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < 512u; i++) dst[i] = src[i];
    return 0;
}

static int rd_bd_write_sector(struct block_device *bd, uint64_t lba, const void *buf) {
    (void)bd;
    (void)lba;
    (void)buf;
    return -1;
}

static int rd_bd_read(void *ctx, uint64_t lba, void *buf, uint32_t count) {
    (void)ctx;
    if (!buf) return -1;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (rd_bd_read_sector(&g_rd_bd, lba + i, dst + (uint64_t)i * 512u) != 0) return -1;
    }
    return 0;
}

static int rd_bd_write(void *ctx, uint64_t lba, const void *buf, uint32_t count) {
    (void)ctx;
    (void)lba;
    (void)buf;
    (void)count;
    return -1;
}

static struct block_device *rd_block_device_init(uint64_t phys_base, uint64_t size) {
    if (!phys_base || size < 512) return 0;
    g_rd_base = TATER_PHYSMAP + phys_base;
    g_rd_size = size;
    g_rd_hdr = 0;
    g_rd_bd.sector_size = 512;
    g_rd_bd.total_sectors = size / 512;
    g_rd_bd.read_sector = rd_bd_read_sector;
    g_rd_bd.write_sector = rd_bd_write_sector;
    g_rd_bd.read = rd_bd_read;
    g_rd_bd.write = rd_bd_write;
    g_rd_bd.ctx = 0;
    return &g_rd_bd;
}

struct rd_file_state {
    uint32_t entry_idx;
    uint64_t pos;
};
static char rd_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}
static uint32_t rd_strlen(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}
static int rd_nameeq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = rd_upper(*a);
        char cb = rd_upper(*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
static uint32_t rd_comp_len(const char *s) {
    uint32_t n = 0;
    while (s[n] && s[n] != '/') n++;
    return n;
}
static int rd_copy_comp(char *dst, uint32_t dst_len, const char *src, uint32_t src_len) {
    if (!dst || dst_len == 0) return -1;
    if (src_len + 1 > dst_len) return -1;
    for (uint32_t i = 0; i < src_len; i++) dst[i] = src[i];
    dst[src_len] = 0;
    return 0;
}
static int rd_prefix_dir_match(const char *entry, const char *dir) {
    uint32_t i = 0;
    if (!entry || !dir) return 0;
    while (dir[i]) {
        if (rd_upper(entry[i]) != rd_upper(dir[i])) return 0;
        i++;
    }
    return entry[i] == '/';
}
static int rd_seen_name(struct rd_readdir_ctx *rc, const char *name) {
    if (!rc || !name) return 0;
    for (uint32_t i = 0; i < rc->seen_count; i++) {
        if (rd_nameeq(rc->seen[i], name)) return 1;
    }
    return 0;
}
static void rd_mark_seen(struct rd_readdir_ctx *rc, const char *name) {
    if (!rc || !name) return;
    if (rc->seen_count >= 32) return;
    (void)rd_copy_comp(rc->seen[rc->seen_count], sizeof(rc->seen[rc->seen_count]),
                       name, rd_comp_len(name));
    rc->seen_count++;
}
static int rd_entry_dir_attr(const char *remainder, uint64_t file_size, uint64_t *out_size, uint32_t *out_attr) {
    uint32_t clen = rd_comp_len(remainder);
    if (clen == 0) return -1;
    if (out_attr) *out_attr = remainder[clen] ? 0x10u : 0x20u;
    if (out_size) *out_size = remainder[clen] ? 0 : file_size;
    return (int)clen;
}
static int rd_open(void *fs_data, const char *path, struct vfs_file *out) {
    (void)fs_data;
    if (!g_rd_hdr) return -1;
    while (*path == '/') path++;
    for (uint32_t i = 0; i < g_rd_hdr->count; i++) {
        if (rd_nameeq(g_rd_hdr->entries[i].name, path)) {
            struct rd_file_state *s = (struct rd_file_state *)out->private;
            s->entry_idx = i;
            s->pos       = 0;
            out->size    = g_rd_hdr->entries[i].size;
            return 0;
        }
    }
    return -1;
}
static int rd_read(struct vfs_file *f, void *buf, uint32_t len) {
    if (!g_rd_hdr) return -1;
    struct rd_file_state  *s = (struct rd_file_state *)f->private;
    struct ramdisk_entry  *e = &g_rd_hdr->entries[s->entry_idx];
    uint64_t remaining = e->size - s->pos;
    if (remaining == 0) return 0;
    if ((uint64_t)len > remaining) len = (uint32_t)remaining;
    uint8_t *src = (uint8_t *)(uintptr_t)(g_rd_base + e->offset + s->pos);
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
    s->pos += len;
    return (int)len;
}
static int rd_write(struct vfs_file *f, const void *buf, uint32_t len) {
    (void)f; (void)buf; (void)len;
    return -1;
}
static int rd_close(struct vfs_file *f) { (void)f; return 0; }
static int rd_readdir(void *fs_data, const char *path,
                      int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                      void *ctx) {
    (void)fs_data;
    if (!g_rd_hdr) return -1;
    struct rd_readdir_ctx rc;
    rc.cb = cb;
    rc.ctx = ctx;
    rc.seen_count = 0;
    rc.matched = 0;
    if (path) while (*path == '/') path++;
    else path = "";
    for (uint32_t i = 0; i < g_rd_hdr->count; i++) {
        const char *entry_name = g_rd_hdr->entries[i].name;
        const char *remainder = entry_name;
        char comp[32];
        uint64_t size = 0;
        uint32_t attr = 0;
        int comp_len;
        if (*path) {
            if (!rd_prefix_dir_match(entry_name, path)) continue;
            remainder = entry_name + rd_strlen(path) + 1;
        }
        comp_len = rd_entry_dir_attr(remainder, g_rd_hdr->entries[i].size, &size, &attr);
        if (comp_len <= 0) continue;
        rc.matched = 1;
        if (rd_copy_comp(comp, sizeof(comp), remainder, (uint32_t)comp_len) != 0) continue;
        if (rd_seen_name(&rc, comp)) continue;
        rd_mark_seen(&rc, comp);
        if (cb(comp, size, attr, ctx)) return 0;
    }
    if (*path && !rc.matched) return -1;
    return 0;
}
static int rd_stat(void *fs_data, const char *path, struct vfs_stat *out) {
    (void)fs_data;
    if (!g_rd_hdr) return -1;
    if (!path) path = "";
    while (*path == '/') path++;
    if (!*path) {
        if (out) {
            out->size = 0;
            out->attr = 0x10;
        }
        return 0;
    }
    for (uint32_t i = 0; i < g_rd_hdr->count; i++) {
        if (rd_nameeq(g_rd_hdr->entries[i].name, path)) {
            if (out) { out->size = g_rd_hdr->entries[i].size; out->attr = 0x20; }
            return 0;
        }
    }
    for (uint32_t i = 0; i < g_rd_hdr->count; i++) {
        if (rd_prefix_dir_match(g_rd_hdr->entries[i].name, path)) {
            if (out) {
                out->size = 0;
                out->attr = 0x10;
            }
            return 0;
        }
    }
    return -1;
}
static struct fs_ops rd_ops;
static int rd_mount_packed(void) {
    uint64_t irqf = vfs_lock_irqsave();
    g_mounts = 0;
    vfs_unlock_irqrestore(irqf);
    rd_ops.open    = rd_open;
    rd_ops.read    = rd_read;
    rd_ops.write   = rd_write;
    rd_ops.close   = rd_close;
    rd_ops.readdir = rd_readdir;
    rd_ops.stat    = rd_stat;
    vfs_mount("/", &rd_ops, 0);
    kprint("VFS: ramdisk mounted (%u files)\n", (unsigned)g_rd_hdr->count);
    return 0;
}

static int rd_header_valid(uint64_t base, uint64_t size) {
    if (!base || size < sizeof(struct ramdisk_header)) return 0;
    struct ramdisk_header *hdr = (struct ramdisk_header *)(uintptr_t)base;
    if (hdr->magic != RAMDISK_MAGIC) return 0;
    if (hdr->count == 0 || hdr->count > RAMDISK_MAXFILES) return 0;

    uint64_t data_floor = sizeof(struct ramdisk_header);
    for (uint32_t i = 0; i < hdr->count; i++) {
        struct ramdisk_entry *e = &hdr->entries[i];
        if (e->name[0] == 0) return 0;
        if (e->offset < data_floor) return 0;
        if (e->offset > size) return 0;
        if (e->size > (size - e->offset)) return 0;
    }
    return 1;
}

int vfs_init_ramdisk(uint64_t phys_base, uint64_t size) {
    struct block_device *rd_bd = rd_block_device_init(phys_base, size);
    if (!rd_bd) return -1;
    g_root_from_ramdisk = 0;

    /*
     * Prefer the native packed ramdisk format produced by the EFI loader.
     * This keeps live-boot app discovery deterministic and independent from
     * block-device probing heuristics.
     */
    if (rd_header_valid(g_rd_base, size)) {
        g_rd_hdr = (struct ramdisk_header *)(uintptr_t)g_rd_base;
        if (rd_mount_packed() == 0) {
            g_root_from_ramdisk = 1;
            return 0;
        }
        return -1;
    }

    /*
     * Preferred path: treat ramdisk as a generic FAT32-backed block device.
     * This keeps filesystem logic independent from NVMe and uses RAM-speed I/O.
     */
    (void)fat32_init(rd_bd);
    uint64_t irqf = vfs_lock_irqsave();
    struct block_device *prev_storage_bd = g_storage_bd;
    vfs_unlock_irqrestore(irqf);
    if (vfs_init(rd_bd) == 0) {
        kprint("VFS: ramdisk FAT32 mounted (%llu sectors)\n",
               (unsigned long long)rd_bd->total_sectors);
        g_root_from_ramdisk = 1;
        return 0;
    }
    irqf = vfs_lock_irqsave();
    g_storage_bd = prev_storage_bd;
    vfs_unlock_irqrestore(irqf);

    /*
     * Legacy fallback: packed-file ramdisk_header format.
     * Keep this for compatibility with older boot payload layouts.
     */
    if (size < sizeof(struct ramdisk_header)) return -1;
    g_rd_hdr = (struct ramdisk_header *)(uintptr_t)g_rd_base;
    if (g_rd_hdr->magic != RAMDISK_MAGIC) {
        kprint("VFS: ramdisk bad magic %08x\n", (unsigned)g_rd_hdr->magic);
        g_rd_hdr = 0;
        return -1;
    }
    if (rd_mount_packed() == 0) {
        g_root_from_ramdisk = 1;
        return 0;
    }
    return -1;
}

void vfs_set_storage_device(struct block_device *bd) {
    uint64_t irqf = vfs_lock_irqsave();
    g_storage_bd = bd;
    vfs_unlock_irqrestore(irqf);
}
static int fat32_open_vfs(void *fs_data, const char *path, struct vfs_file *out) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    int rc = fat32_open(fs, path, (struct fat32_file *)out->private);
    if (rc != 0) return rc;
    out->size = ((struct fat32_file *)out->private)->size;
    return 0;
}
static int fat32_read_vfs(struct vfs_file *f, void *buf, uint32_t len) {
    return fat32_read((struct fat32_file *)f->private, buf, len);
}
static int fat32_write_vfs(struct vfs_file *f, const void *buf, uint32_t len) {
    return fat32_write((struct fat32_file *)f->private, buf, len);
}
static int fat32_close_vfs(struct vfs_file *f) {
    (void)f;
    return 0;
}
struct fat32_readdir_wrap {
    int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx);
    void *ctx;
};
static int fat32_readdir_wrap_cb(const char *name, uint8_t attr, uint32_t size, void *ctx) {
    struct fat32_readdir_wrap *w = (struct fat32_readdir_wrap *)ctx;
    if (!w || !w->cb) return 1;
    return w->cb(name, (uint64_t)size, (uint32_t)attr, w->ctx);
}
static int fat32_readdir_vfs(void *fs_data, const char *path,
                             int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                             void *ctx) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    uint32_t cl = fs->root_cluster;
    if (path && *path) {
        struct fat32_file f;
        if (fat32_open(fs, path, &f) != 0) return -1;
        if (!(f.attr & FAT32_ATTR_DIR)) return -1;
        cl = f.first_cluster;
    }
    struct fat32_readdir_wrap w = { cb, ctx };
    return fat32_readdir(fs, cl, fat32_readdir_wrap_cb, &w);
}
static int fat32_stat_vfs(void *fs_data, const char *path, struct vfs_stat *out) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    if (!path || !*path) {
        if (out) {
            out->size = 0;
            out->attr = FAT32_ATTR_DIR;
        }
        return 0;
    }
    uint32_t sz = 0;
    uint8_t attr = 0;
    if (fat32_stat(fs, path, &sz, &attr) != 0) return -1;
    if (out) {
        out->size = sz;
        out->attr = attr;
    }
    return 0;
}
/* Phase 6: FAT32 seek/truncate/create/mkdir/unlink/rename VFS adapters */
static int64_t fat32_seek_vfs(struct vfs_file *f, int64_t offset, int whence) {
    return fat32_seek((struct fat32_file *)f->private, offset, whence);
}
static int fat32_truncate_vfs(struct vfs_file *f, uint64_t length) {
    if (length > 0xFFFFFFFFULL) return -1;
    int rc = fat32_truncate((struct fat32_file *)f->private, (uint32_t)length);
    if (rc == 0) f->size = length;
    return rc;
}
static int fat32_create_vfs(void *fs_data, const char *path, uint16_t type) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    return fat32_create(fs, path, (type == 0) ? FAT32_ATTR_DIR : 0);
}
static int fat32_mkdir_vfs(void *fs_data, const char *path) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    return fat32_mkdir(fs, path);
}
static int fat32_unlink_vfs(void *fs_data, const char *path) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    return fat32_unlink(fs, path);
}
static int fat32_rename_vfs(void *fs_data, const char *old_path, const char *new_path) {
    struct fat32_fs *fs = (struct fat32_fs *)fs_data;
    return fat32_rename(fs, old_path, new_path);
}
static int fat32_any_path_exists(struct fat32_fs *fs, const char *const *paths, uint32_t count) {
    if (!fs || !paths) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (paths[i] && fat32_stat(fs, paths[i], 0, 0) == 0) return 1;
    }
    return 0;
}
static uint32_t fat32_userspace_score(struct fat32_fs *fs) {
    uint32_t score = 0;
    if (!fs) return 0;
    static const char *init_system[] = { "/system/INIT.FRY" };
    static const char *gui_system[] = { "/system/GUI.FRY" };
    static const char *shell_apps[] = { "/apps/SHELL.TOT" };
    static const char *sysinfo_apps[] = { "/apps/SYSINFO.FRY" };
    static const char *uptime_apps[] = { "/apps/UPTIME.FRY" };
    static const char *ps_apps[] = { "/apps/PS.FRY" };
    static const char *fileman_apps[] = { "/apps/FILEMAN.FRY" };
    static const char *gui_root[] = { "/GUI.FRY" };
    static const char *shell_root[] = { "/SHELL.TOT" };
    static const char *gui_fry_dir[] = { "/fry/GUI.FRY", "/FRY/GUI.FRY" };
    static const char *shell_fry_dir[] = { "/fry/SHELL.TOT", "/FRY/SHELL.TOT" };
    static const char *gui_efi_dir[] = { "/EFI/fry/GUI.FRY", "/EFI/FRY/GUI.FRY" };
    static const char *shell_efi_dir[] = { "/EFI/fry/SHELL.TOT", "/EFI/FRY/SHELL.TOT" };
    static const char *gui_efi_boot[] = {
        "/EFI/BOOT/GUI.FRY", "/EFI/BOOT/fry/GUI.FRY", "/EFI/BOOT/FRY/GUI.FRY"
    };
    static const char *shell_efi_boot[] = {
        "/EFI/BOOT/SHELL.TOT", "/EFI/BOOT/fry/SHELL.TOT", "/EFI/BOOT/FRY/SHELL.TOT"
    };
    if (fat32_any_path_exists(fs, init_system, sizeof(init_system) / sizeof(init_system[0]))) score += 12;
    if (fat32_any_path_exists(fs, gui_system, sizeof(gui_system) / sizeof(gui_system[0]))) score += 10;
    if (fat32_any_path_exists(fs, shell_apps, sizeof(shell_apps) / sizeof(shell_apps[0]))) score += 10;
    if (fat32_any_path_exists(fs, sysinfo_apps, sizeof(sysinfo_apps) / sizeof(sysinfo_apps[0]))) score += 2;
    if (fat32_any_path_exists(fs, uptime_apps, sizeof(uptime_apps) / sizeof(uptime_apps[0]))) score += 2;
    if (fat32_any_path_exists(fs, ps_apps, sizeof(ps_apps) / sizeof(ps_apps[0]))) score += 2;
    if (fat32_any_path_exists(fs, fileman_apps, sizeof(fileman_apps) / sizeof(fileman_apps[0]))) score += 2;
    if (fat32_any_path_exists(fs, gui_root, sizeof(gui_root) / sizeof(gui_root[0]))) score += 8;
    if (fat32_any_path_exists(fs, shell_root, sizeof(shell_root) / sizeof(shell_root[0]))) score += 8;
    if (fat32_any_path_exists(fs, gui_fry_dir, sizeof(gui_fry_dir) / sizeof(gui_fry_dir[0]))) score += 6;
    if (fat32_any_path_exists(fs, shell_fry_dir, sizeof(shell_fry_dir) / sizeof(shell_fry_dir[0]))) score += 6;
    if (fat32_any_path_exists(fs, gui_efi_dir, sizeof(gui_efi_dir) / sizeof(gui_efi_dir[0]))) score += 4;
    if (fat32_any_path_exists(fs, shell_efi_dir, sizeof(shell_efi_dir) / sizeof(shell_efi_dir[0]))) score += 4;
    if (fat32_any_path_exists(fs, gui_efi_boot, sizeof(gui_efi_boot) / sizeof(gui_efi_boot[0]))) score += 3;
    if (fat32_any_path_exists(fs, shell_efi_boot, sizeof(shell_efi_boot) / sizeof(shell_efi_boot[0]))) score += 3;
    return score;
}
static int lba_in_range(struct block_device *bd, uint64_t lba) {
    if (!bd) return 0;
    if (bd->total_sectors == 0) return 1;
    return lba < bd->total_sectors;
}
static int find_best_gpt_fat32_part(struct block_device *bd, uint64_t *out_lba, uint32_t *out_score) {
    if (!bd || !out_lba) return -1;
    struct gpt_info info;
    if (gpt_read_header(bd, &info) != 0) return -1;
    uint64_t best_lba = 0;
    uint32_t best_score = 0;
    for (uint32_t i = 0; i < info.part_count; i++) {
        struct gpt_part p;
        if (gpt_read_part(bd, &info, i, &p) != 0) continue;
        if (p.first_lba == 0 || p.last_lba == 0) continue;
        if (!fat32_probe_bpb(bd, p.first_lba)) continue;
        struct fat32_fs probe_fs;
        if (fat32_mount(&probe_fs, bd, p.first_lba) != 0) continue;
        uint32_t score = fat32_userspace_score(&probe_fs);
        if (!best_lba || score > best_score) {
            best_score = score;
            best_lba = p.first_lba;
        }
    }
    if (best_lba) {
        *out_lba = best_lba;
        if (out_score) *out_score = best_score;
        return 0;
    }
    return -1;
}
struct mbr_part_entry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t first_lba;
    uint32_t sectors;
} __attribute__((packed));
static int find_best_mbr_fat32_part(struct block_device *bd, uint64_t *out_lba, uint32_t *out_score) {
    if (!bd || !out_lba || !bd->read) return -1;
    if (bd->sector_size < 512 || bd->sector_size > 4096) return -1;
    uint8_t sector[4096];
    if (bd->read(bd->ctx, 0, sector, 1) != 0) return -1;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return -1;
    uint64_t best_lba = 0;
    uint32_t best_score = 0;
    const struct mbr_part_entry *ents =
        (const struct mbr_part_entry *)(const void *)(sector + 0x1BE);
    for (uint32_t i = 0; i < 4; i++) {
        const struct mbr_part_entry *e = &ents[i];
        if (e->type == 0 || e->type == 0xEE) continue;
        if (e->first_lba == 0 || e->sectors == 0) continue;
        if (!lba_in_range(bd, e->first_lba)) continue;
        if (!fat32_probe_bpb(bd, e->first_lba)) continue;
        struct fat32_fs probe_fs;
        if (fat32_mount(&probe_fs, bd, e->first_lba) != 0) continue;
        uint32_t score = fat32_userspace_score(&probe_fs);
        if (!best_lba || score > best_score) {
            best_score = score;
            best_lba = e->first_lba;
        }
    }
    if (best_lba) {
        *out_lba = best_lba;
        if (out_score) *out_score = best_score;
        return 0;
    }
    return -1;
}
static int find_best_raw_fat32_lba(struct block_device *bd, uint64_t *out_lba, uint32_t *out_score) {
    if (!bd || !out_lba) return -1;
    static const uint64_t candidates[] = {
        0, 1, 2, 6, 32, 63, 64, 128, 256, 512,
        1024, 2048, 4096, 8192, 16384, 32768, 65536
    };
    uint64_t best_lba = 0;
    uint32_t best_score = 0;
    for (uint32_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uint64_t lba = candidates[i];
        if (!lba_in_range(bd, lba)) continue;
        if (!fat32_probe_bpb(bd, lba)) continue;
        struct fat32_fs probe_fs;
        if (fat32_mount(&probe_fs, bd, lba) != 0) continue;
        uint32_t score = fat32_userspace_score(&probe_fs);
        if (!best_lba || score > best_score) {
            best_score = score;
            best_lba = lba;
        }
    }
    if (best_lba) {
        *out_lba = best_lba;
        if (out_score) *out_score = best_score;
        return 0;
    }
    return -1;
}
int vfs_mount(const char *mountpoint, struct fs_ops *ops, void *fs_data) {
retry:
    if (!mountpoint || !ops) return -1;
    /* Keep mountpoints unique so lookup cannot resolve to stale duplicates. */
    while (vfs_umount(mountpoint) == 0) {
    }
    struct vfs_mount *m = (struct vfs_mount *)kmalloc(sizeof(struct vfs_mount));
    if (!m) return -1;
    for (uint32_t i = 0; i < sizeof(m->mountpoint); i++) m->mountpoint[i] = 0;
    uint32_t i = 0;
    while (mountpoint[i] && i + 1 < sizeof(m->mountpoint)) {
        m->mountpoint[i] = mountpoint[i];
        i++;
    }
    m->mountpoint[i] = 0;
    m->ops = ops;
    m->fs_data = fs_data;
    m->open_refs = 0;
    m->active = 1;
    m->_pad[0] = 0;
    m->_pad[1] = 0;
    m->_pad[2] = 0;
    uint64_t irqf = vfs_lock_irqsave();
    if (mountpoint_exists_locked(mountpoint)) {
        vfs_unlock_irqrestore(irqf);
        kfree(m);
        goto retry;
    }
    m->next = g_mounts;
    g_mounts = m;
    vfs_unlock_irqrestore(irqf);
    return 0;
}

static void vfs_free_fs_data(struct vfs_mount *m) {
    if (!m || !m->fs_data) return;
    if (m->ops == &sec_ntfs_ops) {
        struct ntfs_fs *fs = (struct ntfs_fs *)m->fs_data;
        if (fs) {
            for (uint32_t i = 0; i < NTFS_MFT_CACHE_SIZE; i++) {
                if (fs->mft_cache[i].buf) {
                    kfree(fs->mft_cache[i].buf);
                    fs->mft_cache[i].buf = 0;
                }
            }
        }
    }
#if ENABLE_TOTFS
    if (m->ops == &sec_totfs_ops) {
        kfree(m->fs_data);
        m->fs_data = 0;
        return;
    }
#endif
    if (m->ops == &sec_fat32_ops || m->ops == &sec_ntfs_ops) {
        kfree(m->fs_data);
        m->fs_data = 0;
    }
}

static void vfs_mount_try_dispose(struct vfs_mount *m) {
    if (!m) return;
    if (m->active) return;
    if (m->open_refs != 0) return;
    vfs_free_fs_data(m);
    kfree(m);
}

static int vfs_mount_get_locked(struct vfs_mount *m) {
    if (!m) return -1;
    if (m->open_refs == 0xFFFFFFFFU) return -1;
    m->open_refs++;
    return 0;
}

static void vfs_mount_put(struct vfs_mount *m) {
    if (!m) return;
    int dispose = 0;
    uint64_t irqf = vfs_lock_irqsave();
    if (m->open_refs > 0) m->open_refs--;
    if (!m->active && m->open_refs == 0) {
        dispose = 1;
    }
    vfs_unlock_irqrestore(irqf);
    if (dispose) vfs_mount_try_dispose(m);
}

int vfs_umount(const char *mountpoint) {
    if (!mountpoint) return -1;
    uint64_t irqf = vfs_lock_irqsave();
    struct vfs_mount *prev = 0;
    struct vfs_mount *cur = g_mounts;

    while (cur) {
        if (streq(cur->mountpoint, mountpoint)) {
            if (prev) prev->next = cur->next; else g_mounts = cur->next;
            cur->next = 0;
            cur->active = 0;
            int dispose = (cur->open_refs == 0);
            vfs_unlock_irqrestore(irqf);
            if (dispose) vfs_mount_try_dispose(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    vfs_unlock_irqrestore(irqf);
    return -1;
}

static struct vfs_mount *find_mount_locked(const char *path, uint32_t *out_best_len) {
    struct vfs_mount *best = 0;
    uint32_t best_len = 0;
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        const char *mp = m->mountpoint;
        uint32_t i = 0;
        while (mp[i] && path[i] && mp[i] == path[i]) i++;
        if (mp[i] == 0 && (path[i] == 0 || path[i] == '/' || (i > 0 && mp[i-1] == '/'))) {
            /*
             * Prefer the newest mount for equal-length matches.
             * Mounts are inserted at list head, so strictly-greater keeps the
             * first match seen at that prefix length.
             */
            if (i > best_len) {
                best = m;
                best_len = i;
            }
        }
    }
    if (out_best_len) *out_best_len = best_len;
    return best;
}

static struct vfs_mount *find_mount_ref(const char *path, const char **out_rel) {
    int retried_recover = 0;
    if (!path) return 0;
    for (;;) {
        uint32_t best_len = 0;
        int best_is_root = 0;
        int best_is_nvme = 0;
        uint64_t irqf = vfs_lock_irqsave();
        struct vfs_mount *best = find_mount_locked(path, &best_len);
        if (best) {
            best_is_root = streq(best->mountpoint, "/");
            best_is_nvme = streq(best->mountpoint, "/nvme");
            if (vfs_mount_get_locked(best) == 0) {
                if (out_rel) {
                    const char *p = path + best_len;
                    if (*p == '/') p++;
                    *out_rel = p;
                }
                vfs_unlock_irqrestore(irqf);
                return best;
            }
            best = 0;
        }
        vfs_unlock_irqrestore(irqf);
        if (!retried_recover &&
            path_is_exact_nvme_mountpoint(path) &&
            ((!best) || best_is_root || best_is_nvme) &&
            vfs_try_recover_mount(path)) {
            retried_recover = 1;
            continue;
        }
        return 0;
    }
}
static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}
/* Scan GPT for a ToTFS partition. Returns 0 on success with *out_lba set. */
#if ENABLE_TOTFS
static int find_totfs_gpt_part(struct block_device *bd, uint64_t *out_lba) {
    struct gpt_info info;
    if (gpt_read_header(bd, &info) != 0) return -1;
    for (uint32_t i = 0; i < info.part_count; i++) {
        struct gpt_part p;
        if (gpt_read_part(bd, &info, i, &p) != 0) continue;
        if (p.first_lba == 0 || p.last_lba == 0) continue;
        if (guid_eq(p.type_guid, TOTFS_PART_GUID)) {
            /* Verify the magic */
            if (totfs_probe(bd, p.first_lba)) {
                *out_lba = p.first_lba;
                return 0;
            }
        }
    }
    return -1;
}
#endif
int vfs_init(struct block_device *bd) {
    uint64_t irqf = vfs_lock_irqsave();
    g_mounts = 0;
    g_storage_bd = bd;
    g_root_from_ramdisk = 0;
    vfs_unlock_irqrestore(irqf);
    if (!bd) {
        kprint("VFS: no block device\n");
        return -1;
    }
#if ENABLE_TOTFS
    /* ── Try ToTFS first ──────────────────────────────────────────── */
    uint64_t totfs_lba = 0;
    if (find_totfs_gpt_part(bd, &totfs_lba) == 0) {
        kprint("VFS: ToTFS partition found at LBA %llu\n", (unsigned long long)totfs_lba);
        if (totfs_mount(&g_totfs, bd, totfs_lba) == 0) {
            totfs_ops.open    = totfs_open_vfs;
            totfs_ops.read    = totfs_read_vfs;
            totfs_ops.write   = totfs_write_vfs;
            totfs_ops.close   = totfs_close_vfs;
            totfs_ops.readdir = totfs_readdir_vfs;
            totfs_ops.stat    = totfs_stat_vfs;
            totfs_ops.create  = totfs_create_vfs;
            totfs_ops.mkdir   = totfs_mkdir_vfs;
            totfs_ops.unlink  = totfs_unlink_vfs;
            vfs_mount("/", &totfs_ops, &g_totfs);
            kprint("VFS: ToTFS mounted at LBA %llu\n", (unsigned long long)totfs_lba);
            return 0;
        }
    }
    /* ── Fall back to FAT32 scan ──────────────────────────────────── */
    /* Also try direct ToTFS probe at common raw offsets if no GPT type match */
    {
        static const uint64_t raw_offsets[] = { 2048, 4096, 8192 };
        for (uint32_t i = 0; i < sizeof(raw_offsets)/sizeof(raw_offsets[0]); i++) {
            if (totfs_probe(bd, raw_offsets[i])) {
                if (totfs_mount(&g_totfs, bd, raw_offsets[i]) == 0) {
                    totfs_ops.open    = totfs_open_vfs;
                    totfs_ops.read    = totfs_read_vfs;
                    totfs_ops.write   = totfs_write_vfs;
                    totfs_ops.close   = totfs_close_vfs;
                    totfs_ops.readdir = totfs_readdir_vfs;
                    totfs_ops.stat    = totfs_stat_vfs;
                    totfs_ops.create  = totfs_create_vfs;
                    totfs_ops.mkdir   = totfs_mkdir_vfs;
                    totfs_ops.unlink  = totfs_unlink_vfs;
                    vfs_mount("/", &totfs_ops, &g_totfs);
                    kprint("VFS: ToTFS mounted at raw LBA %llu\n",
                           (unsigned long long)raw_offsets[i]);
                    return 0;
                }
            }
        }
    }
#endif
    uint64_t gpt_lba = 0;
    uint32_t gpt_score = 0;
    int have_gpt = (find_best_gpt_fat32_part(bd, &gpt_lba, &gpt_score) == 0);
    uint64_t mbr_lba = 0;
    uint32_t mbr_score = 0;
    int have_mbr = (find_best_mbr_fat32_part(bd, &mbr_lba, &mbr_score) == 0);
    uint64_t raw_lba = 0;
    uint32_t raw_score = 0;
    int have_raw = (find_best_raw_fat32_lba(bd, &raw_lba, &raw_score) == 0);
    kprint("ERROR: DBG_VFS cand gpt=%d lba=%llu score=%u mbr=%d lba=%llu score=%u raw=%d lba=%llu score=%u\n",
           have_gpt ? 1 : 0, gpt_lba, gpt_score,
           have_mbr ? 1 : 0, mbr_lba, mbr_score,
           have_raw ? 1 : 0, raw_lba, raw_score);
    uint64_t part_lba = 0;
    uint32_t score = 0;
    const char *source = 0;
    if (have_gpt) {
        part_lba = gpt_lba;
        score = gpt_score;
        source = "gpt";
    }
    if (have_mbr && (!source || mbr_score > score)) {
        part_lba = mbr_lba;
        score = mbr_score;
        source = "mbr";
    }
    if (have_raw && (!source || raw_score > score)) {
        part_lba = raw_lba;
        score = raw_score;
        source = "raw";
    }
    if (!source) {
        kprint("ERROR: DBG_VFS no userspace FAT32 candidate\n");
        return -1;
    }
    if (score == 0) {
        kprint("ERROR: DBG_VFS score=0 fallback mount source=%s lba=%llu\n", source, part_lba);
    }
    if (fat32_mount(&g_fat32, bd, part_lba) != 0) {
        return -1;
    }
    fat32_ops.open = fat32_open_vfs;
    fat32_ops.read = fat32_read_vfs;
    fat32_ops.write = fat32_write_vfs;
    fat32_ops.close = fat32_close_vfs;
    fat32_ops.readdir = fat32_readdir_vfs;
    fat32_ops.stat = fat32_stat_vfs;
    fat32_ops.create = fat32_create_vfs;
    fat32_ops.mkdir = fat32_mkdir_vfs;
    fat32_ops.unlink = fat32_unlink_vfs;
    fat32_ops.seek = fat32_seek_vfs;
    fat32_ops.truncate = fat32_truncate_vfs;
    fat32_ops.rename = fat32_rename_vfs;
    vfs_mount("/", &fat32_ops, &g_fat32);
    kprint("ERROR: DBG_VFS picked source=%s lba=%llu userspace_score=%u\n", source, part_lba, score);
    kprint("VFS: FAT32 mounted at LBA %llu\n", part_lba);
    return 0;
}
struct vfs_file *vfs_open(const char *path) {
    if (!path || !*path) return 0;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->open) return 0;
    struct vfs_file *f = vfs_file_alloc();
    if (!f) {
        kprint("VFS: open file pool exhausted path=%s\n", path);
        vfs_mount_put(m);
        return 0;
    }
    f->mount = m;
    if (m->ops->open(m->fs_data, rel, f) != 0) {
        vfs_mount_put(m);
        vfs_file_free(f);
        return 0;
    }
    return f;
}
int vfs_read(struct vfs_file *f, void *buf, uint32_t len) {
    if (!f || !f->mount || !f->mount->ops || !f->mount->ops->read) return -1;
    return f->mount->ops->read(f, buf, len);
}
int vfs_write(struct vfs_file *f, const void *buf, uint32_t len) {
    if (!f || !f->mount || !f->mount->ops || !f->mount->ops->write) return -1;
    return f->mount->ops->write(f, buf, len);
}
int vfs_close(struct vfs_file *f) {
    if (!f) return -1;
    int rc = 0;
    struct vfs_mount *m = f->mount;
    if (m && m->ops && m->ops->close) {
        rc = m->ops->close(f);
    }
    vfs_mount_put(m);
    vfs_file_free(f);
    return rc;
}
uint32_t vfs_size(struct vfs_file *f) {
    if (!f) return 0;
    return (uint32_t)f->size;
}
struct vfs_readdir_wrap {
    int (*cb)(const char *name, void *ctx);
    void *ctx;
};
static int vfs_readdir_name_cb(const char *name, uint64_t size, uint32_t attr, void *ctx) {
    (void)size;
    (void)attr;
    struct vfs_readdir_wrap *w = (struct vfs_readdir_wrap *)ctx;
    if (!w || !w->cb) return 1;
    return w->cb(name, w->ctx);
}
int vfs_readdir(const char *path, int (*cb)(const char *name, void *ctx), void *ctx) {
    if (!path || !cb) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    struct vfs_readdir_wrap w = { cb, ctx };
    int rc = m->ops->readdir(m->fs_data, rel, vfs_readdir_name_cb, &w);
    vfs_mount_put(m);
    return rc;
}
int vfs_readdir_ex(const char *path,
                   int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                   void *ctx) {
    if (!path || !cb) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    int rc = m->ops->readdir(m->fs_data, rel, cb, ctx);
    vfs_mount_put(m);
    return rc;
}
int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->stat) return -1;
    int rc = m->ops->stat(m->fs_data, rel, out);
    vfs_mount_put(m);
    return rc;
}
int vfs_create(const char *path, uint16_t type) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->create) return -1;
    int rc = m->ops->create(m->fs_data, rel, type);
    vfs_mount_put(m);
    return rc;
}
int vfs_mkdir(const char *path) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->mkdir) return -1;
    int rc = m->ops->mkdir(m->fs_data, rel);
    vfs_mount_put(m);
    return rc;
}
int vfs_unlink(const char *path) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount_ref(path, &rel);
    if (!m || !m->ops || !m->ops->unlink) return -1;
    int rc = m->ops->unlink(m->fs_data, rel);
    vfs_mount_put(m);
    return rc;
}
int64_t vfs_seek(struct vfs_file *f, int64_t offset, int whence) {
    if (!f || !f->mount || !f->mount->ops || !f->mount->ops->seek) return -1;
    return f->mount->ops->seek(f, offset, whence);
}
int vfs_truncate(struct vfs_file *f, uint64_t length) {
    if (!f || !f->mount || !f->mount->ops || !f->mount->ops->truncate) return -1;
    return f->mount->ops->truncate(f, length);
}
int vfs_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return -1;
    /* Both paths must be on the same mount */
    const char *old_rel = 0;
    struct vfs_mount *m = find_mount_ref(old_path, &old_rel);
    if (!m || !m->ops || !m->ops->rename) return -1;
    const char *new_rel = 0;
    uint32_t dummy_len = 0;
    struct vfs_mount *m2 = 0;
    spin_lock(&g_vfs_lock);
    m2 = find_mount_locked(new_path, &dummy_len);
    spin_unlock(&g_vfs_lock);
    if (m2 != m) {
        /* Cross-mount rename not supported */
        vfs_mount_put(m);
        return -1;
    }
    /* compute new_rel: skip mountpoint prefix */
    {
        uint32_t mp_len = 0;
        while (m->mountpoint[mp_len]) mp_len++;
        new_rel = new_path + mp_len;
        while (*new_rel == '/') new_rel++;
    }
    int rc = m->ops->rename(m->fs_data, old_rel, new_rel);
    vfs_mount_put(m);
    return rc;
}
static void vfs_storage_zero(struct vfs_storage_info *out) {
    uint8_t *p = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) p[i] = 0;
}
static void vfs_path_fs_zero(struct vfs_path_fs_info *out) {
    uint8_t *p = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) p[i] = 0;
}
static void vfs_mounts_zero(struct vfs_mounts_info *out) {
    uint8_t *p = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) p[i] = 0;
}
static void vfs_mounts_dbg_zero(struct vfs_mounts_dbg *out) {
    uint8_t *p = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) p[i] = 0;
}
static void vfs_storage_copy_mount(char dst[16], const char *src) {
    uint32_t i = 0;
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (src[i] && i + 1 < 16) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}
static void vfs_mount_info_copy_mount(char dst[64], const char *src) {
    uint32_t i = 0;
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (src[i] && i + 1 < 64) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}
static uint8_t vfs_storage_fs_type(const struct vfs_mount *m) {
    if (!m || !m->ops || !m->ops->open) return 0;
    if (m->ops->open == rd_open) return 4;
    if (m->ops->open == fat32_open_vfs) return 1;
    if (m->ops->open == totfs_open_vfs) return 2;
    if (m->ops->open == ntfs_open_vfs) return 3;
    return 0;
}
static int mountpoint_is_nvme_primary(const char *mp) {
    if (!mp) return 0;
    return mp[0] == '/' &&
           mp[1] == 'n' &&
           mp[2] == 'v' &&
           mp[3] == 'm' &&
           mp[4] == 'e' &&
           mp[5] == 0;
}
int vfs_get_storage_info(struct vfs_storage_info *out) {
    struct vfs_mount *first_secondary = 0;
    struct vfs_mount *nvme_secondary = 0;
    if (!out) return -1;
    vfs_storage_zero(out);
    uint64_t irqf = vfs_lock_irqsave();
    struct block_device *bd = g_storage_bd;
    if (bd) {
        out->nvme_detected = 1;
        out->sector_size = (uint32_t)bd->sector_size;
        out->total_sectors = bd->total_sectors;
    }
    if (g_root_from_ramdisk) {
        out->flags |= VFS_STORAGE_FLAG_ROOT_RAMDISK_SOURCE;
    }
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        uint8_t fs_type = vfs_storage_fs_type(m);
        if (m->mountpoint[0] == '/' && m->mountpoint[1] == 0) {
            out->root_fs_type = fs_type;
            vfs_storage_copy_mount(out->root_mount, m->mountpoint);
        } else {
            if (!first_secondary) first_secondary = m;
            if (!nvme_secondary && mountpoint_is_nvme_primary(m->mountpoint)) {
                nvme_secondary = m;
            }
        }
    }
    struct vfs_mount *secondary = nvme_secondary ? nvme_secondary : first_secondary;
    if (secondary) {
        out->secondary_fs_type = vfs_storage_fs_type(secondary);
        vfs_storage_copy_mount(out->secondary_mount, secondary->mountpoint);
    }
    vfs_unlock_irqrestore(irqf);
    return 0;
}
int vfs_get_path_fs_info(const char *path, struct vfs_path_fs_info *out) {
    const char *rel = 0;
    struct vfs_mount *m;
    if (!path || !*path || !out) return -1;
    vfs_path_fs_zero(out);
    m = find_mount_ref(path, &rel);
    if (!m) return -1;
    out->fs_type = vfs_storage_fs_type(m);
    vfs_storage_copy_mount(out->mount, m->mountpoint);
    vfs_mount_put(m);
    return 0;
}
int vfs_get_mounts_info(struct vfs_mounts_info *out) {
    uint32_t count = 0;
    if (!out) return -1;
    vfs_mounts_zero(out);
    uint64_t irqf = vfs_lock_irqsave();
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (struct vfs_mount *m = g_mounts; m; m = m->next) {
            int is_root = (m->mountpoint[0] == '/' && m->mountpoint[1] == 0);
            if ((pass == 0 && !is_root) || (pass == 1 && is_root)) continue;
            if (count >= VFS_MAX_MOUNT_INFO) {
                out->count = count;
                vfs_unlock_irqrestore(irqf);
                return 0;
            }
            out->entries[count].fs_type = vfs_storage_fs_type(m);
            vfs_mount_info_copy_mount(out->entries[count].mount, m->mountpoint);
            count++;
        }
    }
    out->count = count;
    vfs_unlock_irqrestore(irqf);
    return 0;
}

int vfs_get_mounts_dbg(struct vfs_mounts_dbg *out) {
    uint32_t count = 0;
    if (!out) return -1;
    vfs_mounts_dbg_zero(out);
    uint64_t irqf = vfs_lock_irqsave();
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (count >= VFS_MAX_MOUNT_INFO) {
            out->count = count;
            vfs_unlock_irqrestore(irqf);
            return 0;
        }
        struct vfs_mount_dbg *d = &out->entries[count];
        d->fs_type = vfs_storage_fs_type(m);
        vfs_mount_info_copy_mount(d->mount, m->mountpoint);
        d->sector_size = 0;
        d->block_size = 0;
        d->part_lba = 0;
        /* Populate per-FS details if available */
        if (d->fs_type == 1 && m->fs_data) { /* FAT32 */
            struct fat32_fs *fs = (struct fat32_fs *)m->fs_data;
            d->sector_size = (uint32_t)fs->bytes_per_sector;
            d->block_size = fs->bytes_per_sector * fs->sectors_per_cluster;
            d->part_lba = fs->part_lba;
        }
#if ENABLE_TOTFS
        else if (d->fs_type == 2 && m->fs_data) { /* ToTFS */
            struct totfs_fs *fs = (struct totfs_fs *)m->fs_data;
            if (fs->bd) d->sector_size = (uint32_t)fs->bd->sector_size;
            d->block_size = TOTFS_BLOCK_SIZE;
            d->part_lba = fs->part_lba;
        }
#endif
        else if (d->fs_type == 3 && m->fs_data) { /* NTFS */
            struct ntfs_fs *fs = (struct ntfs_fs *)m->fs_data;
            d->sector_size = fs->bytes_per_sector;
            d->block_size = fs->cluster_size;
            d->part_lba = fs->part_lba;
        } else if (d->fs_type == 4) { /* ramdisk */
            d->block_size = 4096;
        }
        count++;
    }
    out->count = count;
    vfs_unlock_irqrestore(irqf);
    return 0;
}
/* ── Secondary Mount (NVMe alongside ramdisk) ──────────────────────────── */
#define SEC_FS_NONE  0u
#define SEC_FS_FAT32 1u
#define SEC_FS_TOTFS 2u
#define SEC_FS_NTFS  3u
#define SEC_MAX_CANDS 64u
struct sec_candidate {
    uint8_t type;
    uint8_t _pad[3];
    uint32_t gpt_index;
    uint32_t score;
    uint64_t lba;
};
static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int mountpoint_has_prefix(const char *mp, const char *prefix) {
    if (!mp || !prefix) return 0;
    while (*prefix) {
        if (*mp != *prefix) return 0;
        mp++;
        prefix++;
    }
    return 1;
}

static uint32_t mount_count_with_prefix(const char *prefix) {
    uint32_t count = 0;
    uint64_t irqf = vfs_lock_irqsave();
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (mountpoint_has_prefix(m->mountpoint, prefix)) count++;
    }
    vfs_unlock_irqrestore(irqf);
    return count;
}
static int mount_exists_lba(uint64_t lba) {
    uint64_t irqf = vfs_lock_irqsave();
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (!m->fs_data) continue;
        if (m->ops == &sec_fat32_ops || m->ops == &fat32_ops) {
            struct fat32_fs *fs = (struct fat32_fs *)m->fs_data;
            if (fs->part_lba == lba) {
                vfs_unlock_irqrestore(irqf);
                return 1;
            }
#if ENABLE_TOTFS
        } else if (m->ops == &sec_totfs_ops || m->ops == &totfs_ops) {
            struct totfs_fs *fs = (struct totfs_fs *)m->fs_data;
            if (fs->part_lba == lba) {
                vfs_unlock_irqrestore(irqf);
                return 1;
            }
#endif
        } else if (m->ops == &sec_ntfs_ops) {
            struct ntfs_fs *fs = (struct ntfs_fs *)m->fs_data;
            if (fs->part_lba == lba) {
                vfs_unlock_irqrestore(irqf);
                return 1;
            }
        }
    }
    vfs_unlock_irqrestore(irqf);
    return 0;
}
static int mountpoint_exists(const char *mountpoint) {
    uint64_t irqf = vfs_lock_irqsave();
    int found = mountpoint_exists_locked(mountpoint);
    vfs_unlock_irqrestore(irqf);
    return found;
}

static int mountpoint_exists_locked(const char *mountpoint) {
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (streq(m->mountpoint, mountpoint)) return 1;
    }
    return 0;
}

static struct vfs_mount *find_mount_exact_ref(const char *mountpoint) {
    if (!mountpoint) return 0;
    uint64_t irqf = vfs_lock_irqsave();
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (streq(m->mountpoint, mountpoint) && vfs_mount_get_locked(m) == 0) {
            vfs_unlock_irqrestore(irqf);
            return m;
        }
    }
    vfs_unlock_irqrestore(irqf);
    return 0;
}

static int mountpoint_healthy(const char *mountpoint) {
    struct vfs_mount *m = find_mount_exact_ref(mountpoint);
    if (!m || !m->ops) return 0;
    int ok = (sec_probe_root(m->ops, m->fs_data) == 0);
    vfs_mount_put(m);
    return ok;
}

static int path_is_nvme_prefix(const char *path) {
    if (!path) return 0;
    return path[0] == '/' &&
           path[1] == 'n' &&
           path[2] == 'v' &&
           path[3] == 'm' &&
           path[4] == 'e';
}

static int path_is_exact_nvme_mountpoint(const char *path) {
    uint32_t i = 5;
    if (!path_is_nvme_prefix(path)) return 0;
    if (path[i] == 0) return 1; /* "/nvme" */
    while (path[i] >= '0' && path[i] <= '9') i++;
    if (path[i] == 0) return 1; /* "/nvme1", "/nvme2", ... */
    if (path[i] == '/' && path[i + 1] == 0) return 1; /* optional trailing slash */
    return 0;
}

static int vfs_try_recover_mount(const char *path) {
    /*
     * Recovery must be explicit and path-local. Avoid triggering secondary
     * scans during ordinary file opens like "/nvme/APP.FRY", which can couple
     * app launch paths to storage probing latency.
     */
    if (!path_is_exact_nvme_mountpoint(path)) return 0;
    int have_nvme = mountpoint_exists("/nvme");
    if (have_nvme && mountpoint_healthy("/nvme")) return 0;
    uint64_t irqf = vfs_lock_irqsave();
    if (g_nvme_recovering) {
        vfs_unlock_irqrestore(irqf);
        return 0;
    }
    g_nvme_recovering = 1;
    struct block_device *bd = g_storage_bd;
    vfs_unlock_irqrestore(irqf);
    if (have_nvme) vfs_umount("/nvme");
    if (bd) {
        vfs_mount_secondary(bd, "/nvme");
    }
    irqf = vfs_lock_irqsave();
    g_nvme_recovering = 0;
    vfs_unlock_irqrestore(irqf);
    return 1;
}

static int ntfs_write_stub(struct vfs_file *f, const void *buf, uint32_t len);
static int ntfs_create_stub(void *fs_data, const char *path, uint16_t attr);
static int ntfs_mkdir_stub(void *fs_data, const char *path);
static int ntfs_unlink_stub(void *fs_data, const char *path);

static int sec_probe_readdir_cb(const char *name, uint64_t size, uint32_t attr, void *ctx) {
    (void)name;
    (void)size;
    (void)attr;
    (void)ctx;
    /* One entry is enough to validate traversal. */
    return 1;
}

static int sec_probe_root(struct fs_ops *ops, void *fs_data) {
    struct vfs_stat st;
    if (!ops || !ops->stat || !ops->readdir) return -1;
    if (ops->stat(fs_data, "", &st) != 0) return -1;
    if ((st.attr & 0x10u) == 0) return -1;
    if (ops->readdir(fs_data, "", sec_probe_readdir_cb, 0) != 0) return -1;
    return 0;
}

static int sec_verify_mounted_path(const char *mountpoint) {
    struct vfs_stat st;
    if (!mountpoint || !*mountpoint) return -1;
    if (vfs_stat(mountpoint, &st) != 0) return -1;
    if ((st.attr & 0x10u) == 0) return -1;
    if (vfs_readdir_ex(mountpoint, sec_probe_readdir_cb, 0) != 0) return -1;
    return 0;
}

static void sec_ops_init(void) {
    uint64_t irqf = vfs_lock_irqsave();
    if (sec_ops_inited) {
        vfs_unlock_irqrestore(irqf);
        return;
    }
#if ENABLE_TOTFS
    sec_totfs_ops.open    = totfs_open_vfs;
    sec_totfs_ops.read    = totfs_read_vfs;
    sec_totfs_ops.write   = totfs_write_vfs;
    sec_totfs_ops.close   = totfs_close_vfs;
    sec_totfs_ops.readdir = totfs_readdir_vfs;
    sec_totfs_ops.stat    = totfs_stat_vfs;
    sec_totfs_ops.create  = totfs_create_vfs;
    sec_totfs_ops.mkdir   = totfs_mkdir_vfs;
    sec_totfs_ops.unlink  = totfs_unlink_vfs;
#endif
    sec_fat32_ops.open     = fat32_open_vfs;
    sec_fat32_ops.read     = fat32_read_vfs;
    sec_fat32_ops.write    = fat32_write_vfs;
    sec_fat32_ops.close    = fat32_close_vfs;
    sec_fat32_ops.readdir  = fat32_readdir_vfs;
    sec_fat32_ops.stat     = fat32_stat_vfs;
    sec_fat32_ops.create   = fat32_create_vfs;
    sec_fat32_ops.mkdir    = fat32_mkdir_vfs;
    sec_fat32_ops.unlink   = fat32_unlink_vfs;
    sec_fat32_ops.seek     = fat32_seek_vfs;
    sec_fat32_ops.truncate = fat32_truncate_vfs;
    sec_fat32_ops.rename   = fat32_rename_vfs;
    sec_ntfs_ops.open    = ntfs_open_vfs;
    sec_ntfs_ops.read    = ntfs_read_vfs;
    sec_ntfs_ops.write   = ntfs_write_stub;
    sec_ntfs_ops.close   = ntfs_close_vfs;
    sec_ntfs_ops.readdir = ntfs_readdir_vfs;
    sec_ntfs_ops.stat    = ntfs_stat_vfs;
    sec_ntfs_ops.create  = ntfs_create_stub;
    sec_ntfs_ops.mkdir   = ntfs_mkdir_stub;
    sec_ntfs_ops.unlink  = ntfs_unlink_stub;
    sec_ops_inited = 1;
    vfs_unlock_irqrestore(irqf);
}
static int sec_make_mountpoint(const char *base, uint32_t ordinal, char out[64]) {
    uint32_t i = 0;
    uint32_t n = ordinal;
    char digits[10];
    uint32_t d = 0;
    if (!base || !out) return -1;
    while (base[i] && i + 1 < 64) {
        out[i] = base[i];
        i++;
    }
    if (base[i] != 0) return -1;
    if (ordinal > 0) {
        do {
            if (d >= sizeof(digits)) return -1;
            digits[d++] = (char)('0' + (n % 10u));
            n /= 10u;
        } while (n > 0);
        while (d > 0) {
            if (i + 1 >= 64) return -1;
            out[i++] = digits[--d];
        }
    }
    out[i] = 0;
    return 0;
}
#if ENABLE_TOTFS
static int mount_secondary_totfs(struct block_device *bd, uint64_t lba, const char *mountpoint) {
    struct totfs_fs *fs = (struct totfs_fs *)kmalloc(sizeof(struct totfs_fs));
    if (!fs) return -1;
    if (totfs_mount(fs, bd, lba) != 0) {
        kfree(fs);
        return -1;
    }
    if (vfs_mount(mountpoint, &sec_totfs_ops, fs) != 0) {
        kfree(fs);
        return -1;
    }
    kprint("VFS: ToTFS mounted at %s (LBA %llu)\n", mountpoint, (unsigned long long)lba);
    return 0;
}
#endif
static int mount_secondary_fat32(struct block_device *bd, uint64_t lba, const char *mountpoint) {
    struct fat32_fs *fs = (struct fat32_fs *)kmalloc(sizeof(struct fat32_fs));
    if (!fs) return -1;
    if (fat32_mount(fs, bd, lba) != 0) {
        kfree(fs);
        return -1;
    }
    if (sec_probe_root(&sec_fat32_ops, fs) != 0) {
        kprint("VFS: FAT32 probe failed at %s (LBA %llu)\n",
               mountpoint, (unsigned long long)lba);
        kfree(fs);
        return -1;
    }
    if (vfs_mount(mountpoint, &sec_fat32_ops, fs) != 0) {
        kfree(fs);
        return -1;
    }
    if (sec_verify_mounted_path(mountpoint) != 0) {
        kprint("VFS: FAT32 path verify failed at %s (LBA %llu)\n",
               mountpoint, (unsigned long long)lba);
        vfs_umount(mountpoint);
        return -1;
    }
    kprint("VFS: FAT32 mounted at %s (LBA %llu)\n", mountpoint, (unsigned long long)lba);
    return 0;
}
static int ntfs_write_stub(struct vfs_file *f, const void *buf, uint32_t len) {
    (void)f; (void)buf; (void)len;
    return -1;
}
static int ntfs_create_stub(void *fs_data, const char *path, uint16_t attr) {
    (void)fs_data; (void)path; (void)attr;
    return -1;
}
static int ntfs_mkdir_stub(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    return -1;
}
static int ntfs_unlink_stub(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    return -1;
}
static int mount_secondary_ntfs(struct block_device *bd, uint64_t lba, const char *mountpoint) {
    struct ntfs_fs *fs = (struct ntfs_fs *)kmalloc(sizeof(struct ntfs_fs));
    if (!fs) return -1;
    if (ntfs_mount(fs, bd, lba) != 0) {
        kfree(fs);
        return -1;
    }
    if (sec_probe_root(&sec_ntfs_ops, fs) != 0) {
        kprint("VFS: NTFS probe failed at %s (LBA %llu)\n",
               mountpoint, (unsigned long long)lba);
        kfree(fs);
        return -1;
    }
    if (vfs_mount(mountpoint, &sec_ntfs_ops, fs) != 0) {
        kfree(fs);
        return -1;
    }
    if (sec_verify_mounted_path(mountpoint) != 0) {
        kprint("VFS: NTFS path verify failed at %s (LBA %llu)\n",
               mountpoint, (unsigned long long)lba);
        vfs_umount(mountpoint);
        return -1;
    }
    kprint("VFS: NTFS mounted at %s (LBA %llu)\n", mountpoint, (unsigned long long)lba);
    return 0;
}
static int mount_secondary_candidate(struct block_device *bd, const struct sec_candidate *c, const char *mountpoint) {
    if (!bd || !c || !mountpoint) return -1;
#if ENABLE_TOTFS
    if (c->type == SEC_FS_TOTFS) return mount_secondary_totfs(bd, c->lba, mountpoint);
#endif
    if (c->type == SEC_FS_FAT32) return mount_secondary_fat32(bd, c->lba, mountpoint);
    if (c->type == SEC_FS_NTFS) return mount_secondary_ntfs(bd, c->lba, mountpoint);
    return -1;
}
int vfs_mount_secondary(struct block_device *bd, const char *mountpoint) {
    struct gpt_info info;
    struct sec_candidate cands[SEC_MAX_CANDS];
    uint32_t cand_count = 0;
    int primary_idx = -1;
    int ntfs_idx = -1;
    int boot_media_seen = 0;
    int non_fat32_seen = 0;
    uint32_t fat32_seen = 0;
    uint32_t best_fat32_score = 0;
    uint32_t mounted = 0;
    uint32_t next_ordinal = 0;
    char mp[64];
    if (!bd || !mountpoint) return -1;
    uint64_t irqf = vfs_lock_irqsave();
    g_storage_bd = bd;
    vfs_unlock_irqrestore(irqf);
    /* Secondary mounts must stay off root to keep ramdisk/block root separate. */
    if (!(mountpoint[0] == '/' && mountpoint[1] != 0)) return -1;
    sec_ops_init();

    uint32_t existing = mount_count_with_prefix(mountpoint);
    if (existing >= SEC_MAX_MOUNTED) {
        kprint("VFS: mount limit reached for %s (%u)\n",
               mountpoint, (unsigned)SEC_MAX_MOUNTED);
        return -1;
    }

    if (gpt_read_header(bd, &info) != 0) {
        kprint("VFS: no GPT detected for secondary mount at %s\n", mountpoint);
        return -1;
    }
    for (uint32_t i = 0; i < info.part_count && cand_count < SEC_MAX_CANDS; i++) {
        struct gpt_part p;
        struct sec_candidate c;
        if (gpt_read_part(bd, &info, i, &p) != 0) continue;
        if (p.first_lba == 0 || p.last_lba == 0) continue;
        if (mount_exists_lba(p.first_lba)) continue; /* already mounted; avoid aliasing */
        c.type = SEC_FS_NONE;
        c.gpt_index = i;
        c.score = 0;
        c.lba = p.first_lba;
        if (fat32_probe_bpb(bd, p.first_lba)) {
            struct fat32_fs probe_fs;
            c.type = SEC_FS_FAT32;
            fat32_seen++;
            if (fat32_mount(&probe_fs, bd, p.first_lba) == 0) {
                c.score = fat32_userspace_score(&probe_fs);
            }
            if (c.score > 0) boot_media_seen = 1;
        }
#if ENABLE_TOTFS
        else if (guid_eq(p.type_guid, TOTFS_PART_GUID) && totfs_probe(bd, p.first_lba)) {
            c.type = SEC_FS_TOTFS;
        }
#endif
        else if (guid_eq(p.type_guid, NTFS_BASIC_DATA_GUID) && ntfs_probe(bd, p.first_lba)) {
            c.type = SEC_FS_NTFS;
            non_fat32_seen = 1;
            if (ntfs_idx < 0) ntfs_idx = (int)cand_count;
        } else {
            continue;
        }
        cands[cand_count++] = c;
    }
    if (boot_media_seen && non_fat32_seen) {
        /*
         * Keep FAT32 candidates even when NTFS/ToTFS are present.
         * Suppressing all FAT32 here can hide app-bearing volumes on real
         * hardware and make GUI launches look broken while QEMU still works
         * from ramdisk payloads.
         */
        kprint("VFS: boot-media-like FAT32 seen at %s with non-FAT32 alternatives; keeping FAT32 candidates\n",
               mountpoint);
    } else if (boot_media_seen && fat32_seen > 0) {
        /*
         * If FAT32 is all we found, keep it mountable. Dropping every FAT32
         * candidate leaves /nvme unavailable and breaks app launches on
         * systems that store userspace payloads on FAT32.
         */
        kprint("VFS: boot-media-like FAT32 seen at %s, keeping FAT32 (no alternate FS)\n",
               mountpoint);
    }
    if (cand_count == 0) {
        kprint("VFS: no supported secondary partitions found at %s\n", mountpoint);
        return -1;
    }
#if ENABLE_TOTFS
    for (uint32_t i = 0; i < cand_count; i++) {
        if (cands[i].type == SEC_FS_TOTFS) {
            primary_idx = (int)i;
            break;
        }
    }
#endif
    if (primary_idx < 0) {
        for (uint32_t i = 0; i < cand_count; i++) {
            if (cands[i].type != SEC_FS_FAT32) continue;
            if (primary_idx < 0 || cands[i].score > best_fat32_score) {
                primary_idx = (int)i;
                best_fat32_score = cands[i].score;
            }
        }
    }
    if (ntfs_idx >= 0) {
        if (primary_idx < 0 ||
            (primary_idx >= 0 && cands[primary_idx].type == SEC_FS_FAT32 && best_fat32_score == 0)) {
            primary_idx = ntfs_idx;
        }
    }
    if (primary_idx >= 0) {
        if (sec_make_mountpoint(mountpoint, next_ordinal, mp) == 0 &&
            !mountpoint_exists(mp) &&
            mount_secondary_candidate(bd, &cands[primary_idx], mp) == 0) {
            mounted++;
            next_ordinal++;
            existing++;
        }
    }
    for (uint32_t i = 0; i < cand_count; i++) {
        if ((int)i == primary_idx) continue;
        if (existing >= SEC_MAX_MOUNTED) break;
        while (sec_make_mountpoint(mountpoint, next_ordinal, mp) == 0 && mountpoint_exists(mp)) {
            next_ordinal++;
        }
        if (sec_make_mountpoint(mountpoint, next_ordinal, mp) != 0) break;
        if (mount_secondary_candidate(bd, &cands[i], mp) == 0) {
            mounted++;
            next_ordinal++;
            existing++;
        }
    }
    if (mounted == 0) {
        kprint("VFS: secondary candidates found but mount failed at %s\n", mountpoint);
        return -1;
    }
    if (mounted > 1) {
        kprint("VFS: mounted %u secondary partitions (%s, %s1...)\n",
               mounted, mountpoint, mountpoint);
    }
    return 0;
}
