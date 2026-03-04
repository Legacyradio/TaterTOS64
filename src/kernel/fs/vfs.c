// VFS core
#include "vfs.h"
#include "fat32.h"
#include "totfs.h"
#include "ntfs.h"
#include "part.h"
#include "../../boot/efi_handoff.h"
void kprint(const char *fmt, ...);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
struct block_device *nvme_get_block_device(void);
#define TATER_PHYSMAP 0xFFFF800000000000ULL
static struct vfs_mount *g_mounts;
static struct fat32_fs   g_fat32;
static struct fs_ops     fat32_ops;
static struct totfs_fs   g_totfs;
static struct fs_ops     totfs_ops;
static struct fs_ops     sec_totfs_ops;
static struct fs_ops     sec_fat32_ops;
static struct fs_ops     sec_ntfs_ops;
static int               sec_ops_inited = 0;
static int               g_nvme_recovering = 0;
static int mountpoint_exists(const char *mountpoint);
static int vfs_try_recover_mount(const char *path);
static int streq(const char *a, const char *b);
#define SEC_MAX_MOUNTED 8u
// ── Ramdisk backend ──────────────────────────────────────────────────────────
static struct ramdisk_header *g_rd_hdr  = 0;
static uint64_t               g_rd_base = 0;  // virtual address of ramdisk
struct rd_file_state {
    uint32_t entry_idx;
    uint64_t pos;
};
static int rd_nameeq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
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
    if (path) {
        while (*path == '/') path++;
        if (*path) {
            /*
             * Flat ramdisk: only root directory exists.
             * Non-root readdir should fail instead of mirroring root entries.
             */
            return -1;
        }
    }
    for (uint32_t i = 0; i < g_rd_hdr->count; i++) {
        if (cb(g_rd_hdr->entries[i].name, g_rd_hdr->entries[i].size, 0x20, ctx)) return 0;
    }
    return 0;
}
static int rd_stat(void *fs_data, const char *path, struct vfs_stat *out) {
    (void)fs_data;
    if (!g_rd_hdr) return -1;
    while (*path == '/') path++;
    for (uint32_t i = 0; i < g_rd_hdr->count; i++) {
        if (rd_nameeq(g_rd_hdr->entries[i].name, path)) {
            if (out) { out->size = g_rd_hdr->entries[i].size; out->attr = 0x20; }
            return 0;
        }
    }
    return -1;
}
static struct fs_ops rd_ops;
int vfs_init_ramdisk(uint64_t phys_base, uint64_t size) {
    if (!phys_base || size < sizeof(struct ramdisk_header)) return -1;
    g_rd_base = TATER_PHYSMAP + phys_base;
    g_rd_hdr  = (struct ramdisk_header *)(uintptr_t)g_rd_base;
    if (g_rd_hdr->magic != RAMDISK_MAGIC) {
        kprint("VFS: ramdisk bad magic %08x\n", (unsigned)g_rd_hdr->magic);
        g_rd_hdr = 0;
        return -1;
    }
    g_mounts       = 0;
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
    if (!mountpoint || !ops) return -1;
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
    m->next = g_mounts;
    g_mounts = m;
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
    if (m->ops == &sec_totfs_ops || m->ops == &sec_fat32_ops || m->ops == &sec_ntfs_ops) {
        kfree(m->fs_data);
        m->fs_data = 0;
    }
}

int vfs_umount(const char *mountpoint) {
    struct vfs_mount *prev = 0;
    struct vfs_mount *cur = g_mounts;
    if (!mountpoint) return -1;

    while (cur) {
        if (streq(cur->mountpoint, mountpoint)) {
            if (prev) prev->next = cur->next; else g_mounts = cur->next;
            vfs_free_fs_data(cur);
            kfree(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return -1;
}
static struct vfs_mount *find_mount(const char *path, const char **out_rel) {
    struct vfs_mount *best = 0;
    uint32_t best_len = 0;
search:
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        const char *mp = m->mountpoint;
        uint32_t i = 0;
        while (mp[i] && path[i] && mp[i] == path[i]) i++;
        if (mp[i] == 0 && (path[i] == 0 || path[i] == '/' || (i > 0 && mp[i-1] == '/'))) {
            if (i >= best_len) {
                best = m;
                best_len = i;
            }
        }
    }
    if (!best && vfs_try_recover_mount(path)) {
        best = 0;
        best_len = 0;
        goto search;
    }
    if (!best) return 0;
    if (out_rel) {
        const char *p = path + best_len;
        if (*p == '/') p++;
        *out_rel = p;
    }
    return best;
}
static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}
/* Scan GPT for a ToTFS partition. Returns 0 on success with *out_lba set. */
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
int vfs_init(struct block_device *bd) {
    g_mounts = 0;
    if (!bd) {
        kprint("VFS: no block device\n");
        return -1;
    }
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
    vfs_mount("/", &fat32_ops, &g_fat32);
    kprint("ERROR: DBG_VFS picked source=%s lba=%llu userspace_score=%u\n", source, part_lba, score);
    kprint("VFS: FAT32 mounted at LBA %llu\n", part_lba);
    return 0;
}
struct vfs_file *vfs_open(const char *path) {
    if (!path || !*path) return 0;
    const char *rel = 0;
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->open) return 0;
    struct vfs_file *f = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
    if (!f) return 0;
    for (uint32_t i = 0; i < sizeof(f->private); i++) f->private[i] = 0;
    f->mount = m;
    if (m->ops->open(m->fs_data, rel, f) != 0) {
        kfree(f);
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
    if (f->mount && f->mount->ops && f->mount->ops->close) {
        rc = f->mount->ops->close(f);
    }
    kfree(f);
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
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    struct vfs_readdir_wrap w = { cb, ctx };
    return m->ops->readdir(m->fs_data, rel, vfs_readdir_name_cb, &w);
}
int vfs_readdir_ex(const char *path,
                   int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                   void *ctx) {
    if (!path || !cb) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    return m->ops->readdir(m->fs_data, rel, cb, ctx);
}
int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->stat) return -1;
    return m->ops->stat(m->fs_data, rel, out);
}
int vfs_create(const char *path, uint16_t type) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->create) return -1;
    return m->ops->create(m->fs_data, rel, type);
}
int vfs_mkdir(const char *path) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->mkdir) return -1;
    return m->ops->mkdir(m->fs_data, rel);
}
int vfs_unlink(const char *path) {
    if (!path) return -1;
    const char *rel = 0;
    struct vfs_mount *m = find_mount(path, &rel);
    if (!m || !m->ops || !m->ops->unlink) return -1;
    return m->ops->unlink(m->fs_data, rel);
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
    struct block_device *bd = nvme_get_block_device();
    if (bd) {
        out->nvme_detected = 1;
        out->sector_size = (uint32_t)bd->sector_size;
        out->total_sectors = bd->total_sectors;
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
    return 0;
}
int vfs_get_path_fs_info(const char *path, struct vfs_path_fs_info *out) {
    const char *rel = 0;
    struct vfs_mount *m;
    if (!path || !*path || !out) return -1;
    vfs_path_fs_zero(out);
    m = find_mount(path, &rel);
    if (!m) return -1;
    out->fs_type = vfs_storage_fs_type(m);
    vfs_storage_copy_mount(out->mount, m->mountpoint);
    return 0;
}
int vfs_get_mounts_info(struct vfs_mounts_info *out) {
    uint32_t count = 0;
    if (!out) return -1;
    vfs_mounts_zero(out);
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (struct vfs_mount *m = g_mounts; m; m = m->next) {
            int is_root = (m->mountpoint[0] == '/' && m->mountpoint[1] == 0);
            if ((pass == 0 && !is_root) || (pass == 1 && is_root)) continue;
            if (count >= VFS_MAX_MOUNT_INFO) {
                out->count = count;
                return 0;
            }
            out->entries[count].fs_type = vfs_storage_fs_type(m);
            vfs_mount_info_copy_mount(out->entries[count].mount, m->mountpoint);
            count++;
        }
    }
    out->count = count;
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
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (mountpoint_has_prefix(m->mountpoint, prefix)) count++;
    }
    return count;
}
static int mountpoint_exists(const char *mountpoint) {
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        if (streq(m->mountpoint, mountpoint)) return 1;
    }
    return 0;
}

static int path_is_nvme_prefix(const char *path) {
    if (!path) return 0;
    return path[0] == '/' &&
           path[1] == 'n' &&
           path[2] == 'v' &&
           path[3] == 'm' &&
           path[4] == 'e';
}

static int vfs_try_recover_mount(const char *path) {
    if (!path_is_nvme_prefix(path)) return 0;
    if (mountpoint_exists("/nvme")) return 0;
    if (g_nvme_recovering) return 0;
    g_nvme_recovering = 1;
    struct block_device *bd = nvme_get_block_device();
    if (bd) {
        vfs_mount_secondary(bd, "/nvme");
    }
    g_nvme_recovering = 0;
    return 1;
}

static int ntfs_write_stub(struct vfs_file *f, const void *buf, uint32_t len);
static int ntfs_create_stub(void *fs_data, const char *path, uint16_t attr);
static int ntfs_mkdir_stub(void *fs_data, const char *path);
static int ntfs_unlink_stub(void *fs_data, const char *path);

static void sec_ops_init(void) {
    if (sec_ops_inited) return;
    sec_totfs_ops.open    = totfs_open_vfs;
    sec_totfs_ops.read    = totfs_read_vfs;
    sec_totfs_ops.write   = totfs_write_vfs;
    sec_totfs_ops.close   = totfs_close_vfs;
    sec_totfs_ops.readdir = totfs_readdir_vfs;
    sec_totfs_ops.stat    = totfs_stat_vfs;
    sec_totfs_ops.create  = totfs_create_vfs;
    sec_totfs_ops.mkdir   = totfs_mkdir_vfs;
    sec_totfs_ops.unlink  = totfs_unlink_vfs;
    sec_fat32_ops.open    = fat32_open_vfs;
    sec_fat32_ops.read    = fat32_read_vfs;
    sec_fat32_ops.write   = fat32_write_vfs;
    sec_fat32_ops.close   = fat32_close_vfs;
    sec_fat32_ops.readdir = fat32_readdir_vfs;
    sec_fat32_ops.stat    = fat32_stat_vfs;
    sec_fat32_ops.create  = 0;
    sec_fat32_ops.mkdir   = 0;
    sec_fat32_ops.unlink  = 0;
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
static int mount_secondary_fat32(struct block_device *bd, uint64_t lba, const char *mountpoint) {
    struct fat32_fs *fs = (struct fat32_fs *)kmalloc(sizeof(struct fat32_fs));
    if (!fs) return -1;
    if (fat32_mount(fs, bd, lba) != 0) {
        kfree(fs);
        return -1;
    }
    if (vfs_mount(mountpoint, &sec_fat32_ops, fs) != 0) {
        kfree(fs);
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
    if (vfs_mount(mountpoint, &sec_ntfs_ops, fs) != 0) {
        kfree(fs);
        return -1;
    }
    kprint("VFS: NTFS mounted at %s (LBA %llu)\n", mountpoint, (unsigned long long)lba);
    return 0;
}
static int mount_secondary_candidate(struct block_device *bd, const struct sec_candidate *c, const char *mountpoint) {
    if (!bd || !c || !mountpoint) return -1;
    if (c->type == SEC_FS_TOTFS) return mount_secondary_totfs(bd, c->lba, mountpoint);
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
    uint32_t best_fat32_score = 0;
    uint32_t mounted = 0;
    uint32_t next_ordinal = 0;
    char mp[64];
    if (!bd || !mountpoint) return -1;
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
        c.type = SEC_FS_NONE;
        c.gpt_index = i;
        c.score = 0;
        c.lba = p.first_lba;
        if (guid_eq(p.type_guid, TOTFS_PART_GUID) && totfs_probe(bd, p.first_lba)) {
            c.type = SEC_FS_TOTFS;
        } else if (fat32_probe_bpb(bd, p.first_lba)) {
            struct fat32_fs probe_fs;
            c.type = SEC_FS_FAT32;
            if (fat32_mount(&probe_fs, bd, p.first_lba) == 0) {
                c.score = fat32_userspace_score(&probe_fs);
            }
            if (c.score > 0) boot_media_seen = 1;
        } else if (guid_eq(p.type_guid, NTFS_BASIC_DATA_GUID) && ntfs_probe(bd, p.first_lba)) {
            c.type = SEC_FS_NTFS;
            if (ntfs_idx < 0) ntfs_idx = (int)cand_count;
        } else {
            continue;
        }
        cands[cand_count++] = c;
    }
    if (boot_media_seen) {
        /* Don't abort NTFS/TOTFS discovery just because the boot FAT32 was seen. */
        kprint("VFS: boot media detected at %s, skipping FAT32 secondary mounts\n", mountpoint);
        for (uint32_t i = 0; i < cand_count; i++) {
            if (cands[i].type == SEC_FS_FAT32) {
                cands[i].type = SEC_FS_NONE;
            }
        }
    }
    if (cand_count == 0) {
        kprint("VFS: no supported secondary partitions found at %s\n", mountpoint);
        return -1;
    }
    for (uint32_t i = 0; i < cand_count; i++) {
        if (cands[i].type == SEC_FS_TOTFS) {
            primary_idx = (int)i;
            break;
        }
    }
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
