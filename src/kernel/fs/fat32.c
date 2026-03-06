// FAT32 filesystem with LFN support

#include "vfs.h"
#include "fat32.h"

void kprint(const char *fmt, ...);

#define FAT32_MAX_SECTOR 4096

struct fat32_bpb {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t reserved[12];
    uint8_t drive_num;
    uint8_t reserved1;
    uint8_t boot_sig;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} __attribute__((packed));

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIR       0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

static uint32_t fat32_chain_step_limit(const struct fat32_fs *fs) {
    if (!fs || fs->total_clusters == 0) return 1;
    if (fs->total_clusters == 0xFFFFFFFFu) return fs->total_clusters;
    return fs->total_clusters + 1u;
}

static uint64_t fat32_cluster_to_lba(struct fat32_fs *fs, uint32_t cluster) {
    uint64_t cluster_index = (cluster >= 2) ? ((uint64_t)cluster - 2ULL) : 0ULL;
    return fs->data_lba + cluster_index * (uint64_t)fs->sectors_per_cluster;
}

static int read_sector(struct fat32_fs *fs, uint64_t lba, void *buf) {
    if (!fs || !fs->bd || !buf) return -1;
    if (fs->bd->read_sector) return fs->bd->read_sector(fs->bd, lba, buf);
    return block_device_read(fs->bd, lba, buf, 1);
}

static int write_sector(struct fat32_fs *fs, uint64_t lba, const void *buf) {
    if (!fs || !fs->bd || !buf) return -1;
    if (fs->bd->write_sector) return fs->bd->write_sector(fs->bd, lba, buf);
    return block_device_write(fs->bd, lba, buf, 1);
}

static uint32_t read_fat_entry(struct fat32_fs *fs, uint32_t cluster) {
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_lba = fs->fat_lba + (fat_offset / fs->bytes_per_sector);
    uint32_t off = (uint32_t)(fat_offset % fs->bytes_per_sector);

    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return 0x0FFFFFFF;
    if (read_sector(fs, fat_lba, sector) != 0) return 0x0FFFFFFF;
    uint32_t val = *(uint32_t *)(sector + off) & 0x0FFFFFFF;
    return val;
}

static int write_fat_entry(struct fat32_fs *fs, uint32_t cluster, uint32_t value) {
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_lba = fs->fat_lba + (fat_offset / fs->bytes_per_sector);
    uint32_t off = (uint32_t)(fat_offset % fs->bytes_per_sector);

    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    if (read_sector(fs, fat_lba, sector) != 0) return -1;
    uint32_t *p = (uint32_t *)(sector + off);
    *p = (*p & 0xF0000000u) | (value & 0x0FFFFFFF);
    if (write_sector(fs, fat_lba, sector) != 0) return -1;

    // mirror to other FAT copies
    for (uint32_t f = 1; f < fs->fat_count; f++) {
        uint64_t lba2 = fat_lba + (uint64_t)f * fs->sectors_per_fat;
        if (write_sector(fs, lba2, sector) != 0) return -1;
    }
    return 0;
}

static uint32_t alloc_cluster(struct fat32_fs *fs, uint32_t prev) {
    if (fs->total_clusters == 0) return 0;
    uint64_t cluster_limit = (uint64_t)fs->total_clusters + 2ULL;
    for (uint64_t c64 = 2; c64 < cluster_limit; c64++) {
        uint32_t c = (uint32_t)c64;
        if (read_fat_entry(fs, c) == 0) {
            if (write_fat_entry(fs, c, 0x0FFFFFFF) != 0) return 0;
            if (prev) {
                if (write_fat_entry(fs, prev, c) != 0) return 0;
            }
            // zero cluster
            uint8_t zero[FAT32_MAX_SECTOR];
            if (fs->bytes_per_sector > sizeof(zero)) return 0;
            for (uint32_t i = 0; i < fs->bytes_per_sector; i++) zero[i] = 0;
            uint64_t lba = fat32_cluster_to_lba(fs, c);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
                if (write_sector(fs, lba + s, zero) != 0) return 0;
            }
            return c;
        }
    }
    return 0;
}

static int is_fat32_bpb(const struct fat32_bpb *bpb) {
    if (!bpb) return 0;
    if (bpb->bytes_per_sector != 512 &&
        bpb->bytes_per_sector != 1024 &&
        bpb->bytes_per_sector != 2048 &&
        bpb->bytes_per_sector != 4096) {
        return 0;
    }
    if (bpb->sectors_per_cluster == 0 || (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) != 0) {
        return 0;
    }
    if (bpb->reserved_sectors == 0) return 0;
    if (bpb->fat_count == 0) return 0;
    if (bpb->sectors_per_fat_32 == 0) return 0;
    if (bpb->root_cluster < 2) return 0;
    if (bpb->total_sectors_16 == 0 && bpb->total_sectors_32 == 0) return 0;
    return 1;
}

static int fat32_boot_sig_ok(const uint8_t *sector, uint32_t bytes_per_sector) {
    if (!sector) return 0;
    if (bytes_per_sector < 512 || bytes_per_sector > FAT32_MAX_SECTOR) return 0;
    return sector[bytes_per_sector - 2] == 0x55 && sector[bytes_per_sector - 1] == 0xAA;
}

int fat32_probe_bpb(struct block_device *bd, uint64_t part_lba) {
    if (!bd) return 0;
    if (bd->sector_size < 512 || bd->sector_size > FAT32_MAX_SECTOR) return 0;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (block_device_read(bd, part_lba, sector, 1) != 0) return 0;
    const struct fat32_bpb *bpb = (const struct fat32_bpb *)sector;
    if (!is_fat32_bpb(bpb)) return 0;
    if (bpb->bytes_per_sector != bd->sector_size) return 0;
    if (!fat32_boot_sig_ok(sector, bpb->bytes_per_sector)) return 0;
    return 1;
}

int fat32_mount(struct fat32_fs *fs, struct block_device *bd, uint64_t part_lba) {
    if (!fs || !bd) return -1;
    if (bd->sector_size < 512 || bd->sector_size > FAT32_MAX_SECTOR) return -1;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (block_device_read(bd, part_lba, sector, 1) != 0) return -1;

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector;
    if (!is_fat32_bpb(bpb)) return -1;
    if (bpb->bytes_per_sector != bd->sector_size) return -1;
    if (!fat32_boot_sig_ok(sector, bpb->bytes_per_sector)) return -1;
    fs->bd = bd;
    fs->part_lba = part_lba;
    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->reserved_sectors = bpb->reserved_sectors;
    fs->fat_count = bpb->fat_count;
    fs->sectors_per_fat = bpb->sectors_per_fat_32;
    fs->root_cluster = bpb->root_cluster;
    uint64_t total_sectors = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
    if (total_sectors == 0) return -1;
    fs->total_sectors = (uint32_t)total_sectors;

    fs->fat_lba = part_lba + fs->reserved_sectors;
    fs->data_lba = fs->fat_lba + (uint64_t)fs->fat_count * fs->sectors_per_fat;

    uint64_t meta = (uint64_t)fs->reserved_sectors +
                    (uint64_t)fs->fat_count * fs->sectors_per_fat;
    if (total_sectors <= meta) return -1;
    uint64_t data_sectors = total_sectors - meta;
    fs->total_clusters = (uint32_t)(data_sectors / fs->sectors_per_cluster);
    if (fs->total_clusters == 0) return -1;
    return 0;
}

int fat32_init(struct block_device *bd) {
    if (!bd) {
        kprint("FAT32: no block device\n");
        return -1;
    }

    const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };

    struct gpt_loc loc;
    uint64_t part_lba = 0;
    int part_found = 0;
    static const uint64_t raw_candidates[] = {
        2048, 4096, 8192, 16384, 32768, 65536, 0
    };
    if (gpt_find_by_type(bd, esp_guid, &loc) == 0 && loc.start_lba != 0) {
        part_lba = loc.start_lba;
        part_found = 1;
    } else {
        struct gpt_info info;
        if (gpt_read_header(bd, &info) != 0) {
            // GPT missing or unreadable: try raw FAT32 at common offsets.
            for (uint32_t i = 0; i < sizeof(raw_candidates) / sizeof(raw_candidates[0]); i++) {
                if (fat32_probe_bpb(bd, raw_candidates[i])) {
                    part_lba = raw_candidates[i];
                    part_found = 1;
                    break;
                }
            }
            if (!part_found) {
                kprint("FAT32: GPT header read failed\n");
                return -1;
            }
        } else {
            for (uint32_t i = 0; i < info.part_count; i++) {
                struct gpt_part p;
                if (gpt_read_part(bd, &info, i, &p) != 0) continue;
                if (p.first_lba == 0 || p.last_lba == 0) continue;
                if (fat32_probe_bpb(bd, p.first_lba)) {
                    part_lba = p.first_lba;
                    part_found = 1;
                    break;
                }
            }
        }
    }

    if (!part_found) {
        kprint("FAT32: no FAT32 partition found\n");
        return -1;
    }

    struct fat32_fs fs;
    if (fat32_mount(&fs, bd, part_lba) != 0) {
        kprint("FAT32: mount failed at LBA %llu\n", part_lba);
        return -1;
    }

    kprint("FAT32: LBA=%llu bytes=%u spc=%u fats=%u spf=%u root=%u\n",
           part_lba, fs.bytes_per_sector, fs.sectors_per_cluster,
           fs.fat_count, fs.sectors_per_fat, fs.root_cluster);
    return 0;
}

static void upper_str_n(char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 32);
    }
}

static void build_83_name(const uint8_t *entry, char *out) {
    int pos = 0;
    for (int i = 0; i < 8; i++) {
        char c = (char)entry[i];
        if (c == ' ') break;
        out[pos++] = c;
    }
    if (entry[8] != ' ') {
        out[pos++] = '.';
        for (int i = 8; i < 11; i++) {
            char c = (char)entry[i];
            if (c == ' ') break;
            out[pos++] = c;
        }
    }
    out[pos] = 0;
}

static uint8_t lfn_checksum_83(const uint8_t *shortname) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + shortname[i]);
    }
    return sum;
}

struct lfn_state {
    char name[260];
    uint8_t checksum;
    uint8_t expected_ord;
    uint8_t valid;
};

static void lfn_reset(struct lfn_state *st) {
    if (!st) return;
    for (uint32_t i = 0; i < sizeof(st->name); i++) st->name[i] = 0;
    st->checksum = 0;
    st->expected_ord = 0;
    st->valid = 0;
}

static int lfn_consume(struct lfn_state *st, const uint8_t *e) {
    if (!st || !e) return 0;
    if (e[11] != ATTR_LFN) return 0;
    uint8_t ord = e[0] & 0x1F;
    uint8_t is_last = e[0] & 0x40;
    if (ord == 0) { lfn_reset(st); return 1; }
    if (is_last) {
        lfn_reset(st);
        st->checksum = e[13];
        st->expected_ord = ord;
        st->valid = 1;
    }
    if (!st->valid) return 1;
    if (e[13] != st->checksum) { lfn_reset(st); return 1; }
    if (ord != st->expected_ord) { lfn_reset(st); return 1; }

    int idx = (ord - 1) * 13;
    if (idx < 0 || idx >= (int)sizeof(st->name) - 1) { lfn_reset(st); return 1; }
    static const uint8_t offs[13] = { 1,3,5,7,9,14,16,18,20,22,24,28,30 };
    for (int i = 0; i < 13; i++) {
        int pos = idx + i;
        if (pos >= (int)sizeof(st->name) - 1) break;
        uint16_t ch = (uint16_t)e[offs[i]] | ((uint16_t)e[offs[i] + 1] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) {
            st->name[pos] = 0;
            break;
        }
        st->name[pos] = (ch < 0x80) ? (char)ch : '?';
    }
    st->expected_ord--;
    if (st->expected_ord == 0) st->valid = 2;
    return 1;
}

static int name_match_83(const uint8_t *entry, const char *name) {
    char tmp[12];
    for (int i = 0; i < 11; i++) tmp[i] = ' ';
    tmp[11] = 0;

    int i = 0;
    const char *p = name;
    while (*p && *p != '.' && i < 8) {
        tmp[i++] = *p++;
    }
    if (*p == '.') {
        p++;
        int j = 0;
        while (*p && j < 3) {
            tmp[8 + j] = *p++;
            j++;
        }
    }
    upper_str_n(tmp, 11);

    for (int k = 0; k < 11; k++) {
        if (entry[k] != (uint8_t)tmp[k]) return 0;
    }
    return 1;
}

static int name_match_lfn(const char *lfn, const char *name) {
    int i = 0;
    while (lfn[i] && name[i]) {
        char a = lfn[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        i++;
    }
    return lfn[i] == 0 && name[i] == 0;
}

static int find_in_dir(struct fat32_fs *fs, uint32_t dir_cluster, const char *name,
                       uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr,
                       uint32_t *out_dir_cluster, uint32_t *out_dir_offset) {
    if (!fs || !name) return -1;
    uint32_t cl = dir_cluster;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    struct lfn_state lfn;
    lfn_reset(&lfn);
    uint32_t chain_steps = 0;
    uint32_t chain_limit = fat32_chain_step_limit(fs);

    while (cl >= 2 && cl < 0x0FFFFFF8) {
        if (chain_steps++ >= chain_limit) return -1;
        uint64_t lba = fat32_cluster_to_lba(fs, cl);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, lba + s, sector) != 0) return -1;
            for (uint32_t off = 0; off < fs->bytes_per_sector; off += 32) {
                uint8_t *e = sector + off;
                if (e[0] == 0x00) return -1;
                if (e[0] == 0xE5) { lfn_reset(&lfn); continue; }

                if (lfn_consume(&lfn, e)) continue;
                if (e[11] & ATTR_VOLUME) { lfn_reset(&lfn); continue; }

                int match = 0;
                if (lfn.valid == 2) {
                    uint8_t sum = lfn_checksum_83(e);
                    if (sum == lfn.checksum && name_match_lfn(lfn.name, name)) match = 1;
                }
                lfn_reset(&lfn);
                if (!match && name_match_83(e, name)) match = 1;

                if (match) {
                    uint16_t cl_hi = *(uint16_t *)(e + 20);
                    uint16_t cl_lo = *(uint16_t *)(e + 26);
                    if (out_cluster) *out_cluster = ((uint32_t)cl_hi << 16) | cl_lo;
                    if (out_size) *out_size = *(uint32_t *)(e + 28);
                    if (out_attr) *out_attr = e[11];
                    if (out_dir_cluster) *out_dir_cluster = cl;
                    if (out_dir_offset) *out_dir_offset = s * fs->bytes_per_sector + off;
                    return 0;
                }
            }
        }
        cl = read_fat_entry(fs, cl);
    }
    return -1;
}

static int update_dir_entry_size(struct fat32_fs *fs, uint32_t dir_cluster, uint32_t dir_offset, uint32_t new_size) {
    uint64_t lba = fat32_cluster_to_lba(fs, dir_cluster) + (dir_offset / fs->bytes_per_sector);
    uint32_t off = dir_offset % fs->bytes_per_sector;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    if (read_sector(fs, lba, sector) != 0) return -1;
    *(uint32_t *)(sector + off + 28) = new_size;
    return write_sector(fs, lba, sector);
}

static int update_dir_entry_cluster(struct fat32_fs *fs, uint32_t dir_cluster, uint32_t dir_offset, uint32_t new_cluster) {
    uint64_t lba = fat32_cluster_to_lba(fs, dir_cluster) + (dir_offset / fs->bytes_per_sector);
    uint32_t off = dir_offset % fs->bytes_per_sector;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    if (read_sector(fs, lba, sector) != 0) return -1;
    *(uint16_t *)(sector + off + 20) = (uint16_t)((new_cluster >> 16) & 0xFFFF);
    *(uint16_t *)(sector + off + 26) = (uint16_t)(new_cluster & 0xFFFF);
    return write_sector(fs, lba, sector);
}

int fat32_open(struct fat32_fs *fs, const char *path, struct fat32_file *out) {
    if (!fs || !path || !out) return -1;
    const char *p = path;
    if (*p == '/') p++;
    if (*p == 0) return -1;

    uint32_t cur = fs->root_cluster;
    while (*p) {
        char name[260];
        uint32_t n = 0;
        while (*p && *p != '/' && n < sizeof(name) - 1) {
            name[n++] = *p++;
        }
        name[n] = 0;
        if (*p == '/') p++;

        uint32_t cl = 0, size = 0, dir_cl = 0, dir_off = 0;
        uint8_t attr = 0;
        if (find_in_dir(fs, cur, name, &cl, &size, &attr, &dir_cl, &dir_off) != 0) return -1;

        if (*p == 0) {
            out->fs = fs;
            out->first_cluster = cl;
            out->size = size;
            out->pos = 0;
            out->dir_cluster = dir_cl;
            out->dir_offset = dir_off;
            out->attr = attr;
            return 0;
        }

        if (!(attr & ATTR_DIR)) return -1;
        cur = cl;
    }
    return -1;
}

int fat32_read(struct fat32_file *f, void *buf, uint32_t len) {
    if (!f || !buf || len == 0) return 0;
    if (f->pos >= f->size) return 0;
    if (len > f->size - f->pos) len = f->size - f->pos;
    if (len == 0) return 0;
    if (f->size == 0) return 0;
    if (f->first_cluster < 2) return -1;

    uint8_t *out = (uint8_t *)buf;
    uint32_t remaining = len;
    uint32_t cluster = f->first_cluster;
    uint64_t cluster_size = (uint64_t)f->fs->sectors_per_cluster *
                            (uint64_t)f->fs->bytes_per_sector;
    uint64_t skip = f->pos;
    uint32_t chain_steps = 0;
    uint32_t chain_limit = fat32_chain_step_limit(f->fs);
    if (cluster_size == 0) return -1;

    while (skip >= cluster_size) {
        if (chain_steps++ >= chain_limit) return (len - remaining);
        cluster = read_fat_entry(f->fs, cluster);
        skip -= cluster_size;
        if (cluster >= 0x0FFFFFF8) return 0;
    }

    while (remaining > 0 && cluster < 0x0FFFFFF8) {
        if (chain_steps++ >= chain_limit) break;
        uint64_t lba = fat32_cluster_to_lba(f->fs, cluster);
        uint32_t skip_sectors = (uint32_t)(skip / f->fs->bytes_per_sector);
        uint32_t skip_bytes = (uint32_t)(skip % f->fs->bytes_per_sector);
        for (uint32_t s = 0; s < f->fs->sectors_per_cluster; s++) {
            uint8_t sector[FAT32_MAX_SECTOR];
            if (s < skip_sectors) continue;
            if (f->fs->bytes_per_sector > sizeof(sector)) return (len - remaining);
            if (read_sector(f->fs, lba + s, sector) != 0) return (len - remaining);

            uint32_t off = (s == skip_sectors) ? skip_bytes : 0;
            uint32_t avail = f->fs->bytes_per_sector - off;
            uint32_t take = (remaining < avail) ? remaining : avail;

            for (uint32_t i = 0; i < take; i++) {
                out[len - remaining + i] = sector[off + i];
            }

            remaining -= take;
            if (remaining == 0) break;
        }
        skip = 0;
        if (remaining == 0) break;
        cluster = read_fat_entry(f->fs, cluster);
    }

    f->pos += (len - remaining);
    return (len - remaining);
}

int fat32_write(struct fat32_file *f, const void *buf, uint32_t len) {
    if (!f || !buf || len == 0) return 0;
    if (f->attr & ATTR_DIR) return -1;

    if (f->first_cluster < 2) {
        if (f->pos != 0 || f->size != 0) return -1;
        uint32_t newc = alloc_cluster(f->fs, 0);
        if (!newc) return -1;
        if (update_dir_entry_cluster(f->fs, f->dir_cluster, f->dir_offset, newc) != 0) {
            write_fat_entry(f->fs, newc, 0);
            return -1;
        }
        f->first_cluster = newc;
    }

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t remaining = len;
    uint32_t cluster = f->first_cluster;
    uint64_t cluster_size = (uint64_t)f->fs->sectors_per_cluster *
                            (uint64_t)f->fs->bytes_per_sector;
    uint64_t skip = f->pos;
    uint32_t chain_steps = 0;
    uint32_t chain_limit = fat32_chain_step_limit(f->fs);
    if (cluster_size == 0) return -1;

    uint32_t prev = 0;
    while (skip >= cluster_size) {
        if (chain_steps++ >= chain_limit) return (len - remaining);
        prev = cluster;
        uint32_t next = read_fat_entry(f->fs, cluster);
        if (next >= 0x0FFFFFF8) {
            next = alloc_cluster(f->fs, cluster);
            if (!next) return (len - remaining);
        }
        cluster = next;
        skip -= cluster_size;
    }

    while (remaining > 0) {
        if (chain_steps++ >= chain_limit) break;
        if (cluster >= 0x0FFFFFF8) {
            cluster = alloc_cluster(f->fs, prev);
            if (!cluster) break;
        }
        uint64_t lba = fat32_cluster_to_lba(f->fs, cluster);
        uint32_t skip_sectors = (uint32_t)(skip / f->fs->bytes_per_sector);
        uint32_t skip_bytes = (uint32_t)(skip % f->fs->bytes_per_sector);
        for (uint32_t s = 0; s < f->fs->sectors_per_cluster; s++) {
            uint8_t sector[FAT32_MAX_SECTOR];
            if (s < skip_sectors) continue;
            if (f->fs->bytes_per_sector > sizeof(sector)) return (len - remaining);
            if (read_sector(f->fs, lba + s, sector) != 0) return (len - remaining);
            uint32_t off = (s == skip_sectors) ? skip_bytes : 0;
            uint32_t avail = f->fs->bytes_per_sector - off;
            uint32_t take = (remaining < avail) ? remaining : avail;
            for (uint32_t i = 0; i < take; i++) {
                sector[off + i] = in[len - remaining + i];
            }
            if (write_sector(f->fs, lba + s, sector) != 0) return (len - remaining);
            remaining -= take;
            if (remaining == 0) break;
        }
        skip = 0;
        prev = cluster;
        if (remaining == 0) break;
        cluster = read_fat_entry(f->fs, cluster);
    }

    f->pos += (len - remaining);
    if (f->pos > f->size) {
        f->size = f->pos;
        update_dir_entry_size(f->fs, f->dir_cluster, f->dir_offset, f->size);
    }
    return (len - remaining);
}

int fat32_readdir(struct fat32_fs *fs, uint32_t dir_cluster,
                  int (*cb)(const char *name, uint8_t attr, uint32_t size, void *ctx),
                  void *ctx) {
    if (!fs || !cb) return -1;
    uint32_t cl = dir_cluster;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    struct lfn_state lfn;
    lfn_reset(&lfn);
    uint32_t chain_steps = 0;
    uint32_t chain_limit = fat32_chain_step_limit(fs);

    while (cl >= 2 && cl < 0x0FFFFFF8) {
        if (chain_steps++ >= chain_limit) return -1;
        uint64_t lba = fat32_cluster_to_lba(fs, cl);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, lba + s, sector) != 0) return -1;
            for (uint32_t off = 0; off < fs->bytes_per_sector; off += 32) {
                uint8_t *e = sector + off;
                if (e[0] == 0x00) return 0;
                if (e[0] == 0xE5) { lfn_reset(&lfn); continue; }

                if (lfn_consume(&lfn, e)) continue;

                if (e[11] & ATTR_VOLUME) {
                    lfn_reset(&lfn);
                    continue;
                }

                char name[260];
                if (lfn.valid == 2 && lfn_checksum_83(e) == lfn.checksum) {
                    uint32_t i = 0;
                    while (lfn.name[i] && i + 1 < sizeof(name)) { name[i] = lfn.name[i]; i++; }
                    name[i] = 0;
                } else {
                    build_83_name(e, name);
                }
                lfn_reset(&lfn);
                if (name[0]) {
                    uint32_t size = *(uint32_t *)(e + 28);
                    if (cb(name, e[11], size, ctx)) return 0;
                }
            }
        }
        cl = read_fat_entry(fs, cl);
    }
    return 0;
}

int fat32_stat(struct fat32_fs *fs, const char *path, uint32_t *size_out, uint8_t *attr_out) {
    if (!fs || !path) return -1;
    struct fat32_file f;
    if (fat32_open(fs, path, &f) != 0) return -1;
    if (size_out) *size_out = f.size;
    if (attr_out) *attr_out = f.attr;
    return 0;
}
