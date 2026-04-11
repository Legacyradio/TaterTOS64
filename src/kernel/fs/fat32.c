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

/* ===== Phase 6: Filesystem Expansion ===== */

/*
 * fat32_seek — Reposition read/write offset within an open file.
 * whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
 * Returns new absolute position on success, -1 on error.
 */
int64_t fat32_seek(struct fat32_file *f, int64_t offset, int whence) {
    if (!f) return -1;
    int64_t new_pos;
    switch (whence) {
    case 0: /* SEEK_SET */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR */
        new_pos = (int64_t)f->pos + offset;
        break;
    case 2: /* SEEK_END */
        new_pos = (int64_t)f->size + offset;
        break;
    default:
        return -1;
    }
    if (new_pos < 0) return -1;
    if (new_pos > (int64_t)0xFFFFFFFFLL) return -1; /* FAT32 max file size */
    f->pos = (uint32_t)new_pos;
    return new_pos;
}

/*
 * Free all clusters in a chain starting at 'cluster'.
 */
static void fat32_free_chain(struct fat32_fs *fs, uint32_t cluster) {
    uint32_t chain_steps = 0;
    uint32_t chain_limit = fat32_chain_step_limit(fs);
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (chain_steps++ >= chain_limit) break;
        uint32_t next = read_fat_entry(fs, cluster);
        write_fat_entry(fs, cluster, 0);
        cluster = next;
    }
}

/*
 * Walk a cluster chain for 'count' steps and return the cluster at step N.
 * Returns 0 on error (chain too short).
 */
static uint32_t fat32_walk_chain(struct fat32_fs *fs, uint32_t start, uint32_t count) {
    uint32_t cl = start;
    uint32_t chain_limit = fat32_chain_step_limit(fs);
    for (uint32_t i = 0; i < count; i++) {
        if (cl < 2 || cl >= 0x0FFFFFF8) return 0;
        if (i >= chain_limit) return 0;
        cl = read_fat_entry(fs, cl);
    }
    return cl;
}

/*
 * fat32_truncate — Set file to new_size bytes.
 * Truncating down frees excess clusters. Truncating up to the same size is a no-op.
 * Extending beyond current size is not supported (write to extend instead).
 * Returns 0 on success, -1 on error.
 */
int fat32_truncate(struct fat32_file *f, uint32_t new_size) {
    if (!f || !f->fs) return -1;
    if (f->attr & ATTR_DIR) return -1;
    if (new_size == f->size) return 0;

    /* Extending: only support truncate-down and truncate-to-zero */
    if (new_size > f->size) return -1;

    struct fat32_fs *fs = f->fs;
    uint64_t cluster_size = (uint64_t)fs->sectors_per_cluster * (uint64_t)fs->bytes_per_sector;
    if (cluster_size == 0) return -1;

    if (new_size == 0) {
        /* Free entire chain */
        if (f->first_cluster >= 2) {
            fat32_free_chain(fs, f->first_cluster);
        }
        f->first_cluster = 0;
        f->size = 0;
        f->pos = 0;
        update_dir_entry_size(fs, f->dir_cluster, f->dir_offset, 0);
        update_dir_entry_cluster(fs, f->dir_cluster, f->dir_offset, 0);
        return 0;
    }

    /* Calculate how many clusters the new size needs */
    uint32_t clusters_needed = (uint32_t)((new_size + cluster_size - 1) / cluster_size);
    if (clusters_needed == 0) clusters_needed = 1;

    /* Walk chain to the last cluster we want to keep */
    uint32_t keep_last = fat32_walk_chain(fs, f->first_cluster, clusters_needed - 1);
    if (keep_last < 2) return -1;

    /* Free the chain after the last kept cluster */
    uint32_t next_after = read_fat_entry(fs, keep_last);
    if (next_after >= 2 && next_after < 0x0FFFFFF8) {
        fat32_free_chain(fs, next_after);
    }
    /* Mark the kept last cluster as end-of-chain */
    write_fat_entry(fs, keep_last, 0x0FFFFFFF);

    f->size = new_size;
    if (f->pos > new_size) f->pos = new_size;
    update_dir_entry_size(fs, f->dir_cluster, f->dir_offset, new_size);
    return 0;
}

/*
 * Build an 8.3 short name from a filename component.
 * Returns 0 on success, -1 if the name cannot be represented as 8.3.
 * out must be exactly 11 bytes.
 */
static int build_short_name(const char *name, uint8_t *out) {
    if (!name || !out) return -1;
    for (int i = 0; i < 11; i++) out[i] = ' ';

    /* Find dot position */
    int dot_pos = -1;
    int len = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.' && dot_pos < 0) dot_pos = i;
        len++;
    }
    if (len == 0 || len > 12) return -1;

    /* Base name: up to 8 chars before dot */
    int base_len = (dot_pos >= 0) ? dot_pos : len;
    if (base_len > 8 || base_len == 0) return -1;
    for (int i = 0; i < base_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ' || c == '"' || c == '/' || c == '\\' ||
            c == '[' || c == ']' || c == ';' || c == '=' || c == ',') return -1;
        out[i] = (uint8_t)c;
    }

    /* Extension: up to 3 chars after dot */
    if (dot_pos >= 0) {
        const char *ext = name + dot_pos + 1;
        int ext_len = 0;
        while (ext[ext_len]) ext_len++;
        if (ext_len > 3) return -1;
        for (int i = 0; i < ext_len; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[8 + i] = (uint8_t)c;
        }
    }
    return 0;
}

/*
 * Find a free 32-byte directory entry slot in a directory cluster chain.
 * Returns 0 on success, filling out_cluster and out_offset.
 * If no free slot exists, extends the directory by allocating a new cluster.
 */
static int find_free_dir_slot(struct fat32_fs *fs, uint32_t dir_cluster,
                              uint32_t *out_cluster, uint32_t *out_offset) {
    if (!fs) return -1;
    uint32_t cl = dir_cluster;
    uint32_t prev = 0;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    uint32_t chain_steps = 0;
    uint32_t chain_limit = fat32_chain_step_limit(fs);

    while (cl >= 2 && cl < 0x0FFFFFF8) {
        if (chain_steps++ >= chain_limit) return -1;
        uint64_t lba = fat32_cluster_to_lba(fs, cl);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (read_sector(fs, lba + s, sector) != 0) return -1;
            for (uint32_t off = 0; off < fs->bytes_per_sector; off += 32) {
                uint8_t first = sector[off];
                if (first == 0x00 || first == 0xE5) {
                    *out_cluster = cl;
                    *out_offset = s * fs->bytes_per_sector + off;
                    return 0;
                }
            }
        }
        prev = cl;
        cl = read_fat_entry(fs, cl);
    }

    /* No free slot found — extend the directory */
    uint32_t newc = alloc_cluster(fs, prev);
    if (!newc) return -1;
    *out_cluster = newc;
    *out_offset = 0;
    return 0;
}

/*
 * Write a 32-byte directory entry at the given cluster + offset.
 */
static int write_dir_entry(struct fat32_fs *fs, uint32_t dir_cluster,
                           uint32_t dir_offset, const uint8_t *entry) {
    uint64_t lba = fat32_cluster_to_lba(fs, dir_cluster)
                 + (dir_offset / fs->bytes_per_sector);
    uint32_t off = dir_offset % fs->bytes_per_sector;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    if (read_sector(fs, lba, sector) != 0) return -1;
    for (int i = 0; i < 32; i++) sector[off + i] = entry[i];
    return write_sector(fs, lba, sector);
}

/*
 * Split path into parent dir path and filename.
 * E.g. "/foo/bar/baz.txt" -> parent="/foo/bar", name="baz.txt".
 * root-level: "/baz.txt" -> parent="", name="baz.txt".
 * Returns 0 on success.
 */
static int split_path(const char *path, char *parent, uint32_t parent_sz,
                      char *name, uint32_t name_sz) {
    if (!path || !parent || !name) return -1;
    const char *p = path;
    if (*p == '/') p++;
    if (*p == 0) return -1; /* can't create root */

    /* Find last slash */
    const char *last_slash = 0;
    for (const char *s = p; *s; s++) {
        if (*s == '/') last_slash = s;
    }

    if (!last_slash) {
        /* File in root directory */
        parent[0] = 0;
        uint32_t i = 0;
        while (p[i] && i + 1 < name_sz) { name[i] = p[i]; i++; }
        name[i] = 0;
    } else {
        /* File in subdirectory */
        uint32_t plen = (uint32_t)(last_slash - p);
        if (plen + 1 >= parent_sz) return -1;
        for (uint32_t i = 0; i < plen; i++) parent[i] = p[i];
        parent[plen] = 0;
        const char *n = last_slash + 1;
        uint32_t i = 0;
        while (n[i] && i + 1 < name_sz) { name[i] = n[i]; i++; }
        name[i] = 0;
    }
    return 0;
}

/*
 * Resolve a directory path to its cluster number.
 * Empty path means root directory.
 */
static int resolve_dir_cluster(struct fat32_fs *fs, const char *dir_path,
                               uint32_t *out_cluster) {
    if (!fs || !out_cluster) return -1;
    if (!dir_path || dir_path[0] == 0) {
        *out_cluster = fs->root_cluster;
        return 0;
    }

    const char *p = dir_path;
    uint32_t cur = fs->root_cluster;
    while (*p) {
        char name[260];
        uint32_t n = 0;
        while (*p && *p != '/' && n < sizeof(name) - 1) {
            name[n++] = *p++;
        }
        name[n] = 0;
        if (*p == '/') p++;

        uint32_t cl = 0;
        uint8_t attr = 0;
        if (find_in_dir(fs, cur, name, &cl, 0, &attr, 0, 0) != 0) return -1;
        if (!(attr & ATTR_DIR)) return -1;
        cur = cl;
    }
    *out_cluster = cur;
    return 0;
}

/*
 * fat32_create — Create a new empty file or directory entry.
 * attr: 0 for regular file, ATTR_DIR for directory.
 * Returns 0 on success, -1 on error.
 */
int fat32_create(struct fat32_fs *fs, const char *path, uint8_t attr) {
    if (!fs || !path) return -1;
    char parent[260], name[260];
    if (split_path(path, parent, sizeof(parent), name, sizeof(name)) != 0) return -1;

    uint32_t dir_cluster;
    if (resolve_dir_cluster(fs, parent, &dir_cluster) != 0) return -1;

    /* Check if name already exists */
    uint32_t dummy;
    if (find_in_dir(fs, dir_cluster, name, &dummy, 0, 0, 0, 0) == 0) return -1;

    /* Build 8.3 entry */
    uint8_t entry[32];
    for (int i = 0; i < 32; i++) entry[i] = 0;
    if (build_short_name(name, entry) != 0) return -1;
    entry[11] = attr | ATTR_ARCHIVE;

    uint32_t first_cluster = 0;
    if (attr & ATTR_DIR) {
        /* Allocate a cluster for the new directory and write "." and ".." */
        first_cluster = alloc_cluster(fs, 0);
        if (!first_cluster) return -1;

        /* Write "." entry */
        uint8_t dot[32];
        for (int i = 0; i < 32; i++) dot[i] = 0;
        dot[0] = '.';
        for (int i = 1; i < 11; i++) dot[i] = ' ';
        dot[11] = ATTR_DIR;
        *(uint16_t *)(dot + 20) = (uint16_t)((first_cluster >> 16) & 0xFFFF);
        *(uint16_t *)(dot + 26) = (uint16_t)(first_cluster & 0xFFFF);

        /* Write ".." entry */
        uint8_t dotdot[32];
        for (int i = 0; i < 32; i++) dotdot[i] = 0;
        dotdot[0] = '.';
        dotdot[1] = '.';
        for (int i = 2; i < 11; i++) dotdot[i] = ' ';
        dotdot[11] = ATTR_DIR;
        uint32_t parent_cl = (dir_cluster == fs->root_cluster) ? 0 : dir_cluster;
        *(uint16_t *)(dotdot + 20) = (uint16_t)((parent_cl >> 16) & 0xFFFF);
        *(uint16_t *)(dotdot + 26) = (uint16_t)(parent_cl & 0xFFFF);

        /* Write both entries to the first sector of the new cluster */
        uint8_t sector[FAT32_MAX_SECTOR];
        if (fs->bytes_per_sector > sizeof(sector)) { fat32_free_chain(fs, first_cluster); return -1; }
        uint64_t lba = fat32_cluster_to_lba(fs, first_cluster);
        if (read_sector(fs, lba, sector) != 0) { fat32_free_chain(fs, first_cluster); return -1; }
        for (int i = 0; i < 32; i++) sector[i] = dot[i];
        for (int i = 0; i < 32; i++) sector[32 + i] = dotdot[i];
        if (write_sector(fs, lba, sector) != 0) { fat32_free_chain(fs, first_cluster); return -1; }
    }

    /* Set cluster in entry */
    *(uint16_t *)(entry + 20) = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    *(uint16_t *)(entry + 26) = (uint16_t)(first_cluster & 0xFFFF);
    /* Size is 0 for both files and directories */
    *(uint32_t *)(entry + 28) = 0;

    /* Find free slot in parent directory and write entry */
    uint32_t slot_cl, slot_off;
    if (find_free_dir_slot(fs, dir_cluster, &slot_cl, &slot_off) != 0) {
        if (first_cluster) fat32_free_chain(fs, first_cluster);
        return -1;
    }
    if (write_dir_entry(fs, slot_cl, slot_off, entry) != 0) {
        if (first_cluster) fat32_free_chain(fs, first_cluster);
        return -1;
    }
    return 0;
}

/*
 * fat32_mkdir — Create a new directory.
 */
int fat32_mkdir(struct fat32_fs *fs, const char *path) {
    return fat32_create(fs, path, ATTR_DIR);
}

/*
 * Mark a directory entry as deleted (0xE5) at the given cluster + offset.
 */
static int mark_dir_entry_deleted(struct fat32_fs *fs, uint32_t dir_cluster,
                                  uint32_t dir_offset) {
    uint64_t lba = fat32_cluster_to_lba(fs, dir_cluster)
                 + (dir_offset / fs->bytes_per_sector);
    uint32_t off = dir_offset % fs->bytes_per_sector;
    uint8_t sector[FAT32_MAX_SECTOR];
    if (fs->bytes_per_sector > sizeof(sector)) return -1;
    if (read_sector(fs, lba, sector) != 0) return -1;
    sector[off] = 0xE5;
    return write_sector(fs, lba, sector);
}

/*
 * fat32_unlink — Delete a file.
 * Does not delete directories (use rmdir for that).
 * Returns 0 on success, -1 on error.
 */
int fat32_unlink(struct fat32_fs *fs, const char *path) {
    if (!fs || !path) return -1;
    struct fat32_file f;
    if (fat32_open(fs, path, &f) != 0) return -1;
    if (f.attr & ATTR_DIR) return -1; /* can't unlink directories */

    /* Free the cluster chain */
    if (f.first_cluster >= 2) {
        fat32_free_chain(fs, f.first_cluster);
    }

    /* Mark directory entry as deleted */
    return mark_dir_entry_deleted(fs, f.dir_cluster, f.dir_offset);
}

/*
 * fat32_rename — Rename/move a file or directory.
 *
 * Strategy: create new directory entry with old file's clusters,
 * then delete old entry. Handles same-directory and cross-directory renames.
 * Returns 0 on success, -1 on error.
 */
int fat32_rename(struct fat32_fs *fs, const char *old_path, const char *new_path) {
    if (!fs || !old_path || !new_path) return -1;

    /* Open old file to get its metadata */
    struct fat32_file old_f;
    if (fat32_open(fs, old_path, &old_f) != 0) return -1;

    /* Check that new_path doesn't already exist */
    char new_parent[260], new_name[260];
    if (split_path(new_path, new_parent, sizeof(new_parent),
                   new_name, sizeof(new_name)) != 0) return -1;

    uint32_t new_dir_cluster;
    if (resolve_dir_cluster(fs, new_parent, &new_dir_cluster) != 0) return -1;

    uint32_t check;
    if (find_in_dir(fs, new_dir_cluster, new_name, &check, 0, 0, 0, 0) == 0) return -1;

    /* Build new 8.3 entry with same cluster/size/attr */
    uint8_t entry[32];
    for (int i = 0; i < 32; i++) entry[i] = 0;
    if (build_short_name(new_name, entry) != 0) return -1;
    entry[11] = old_f.attr | ATTR_ARCHIVE;
    *(uint16_t *)(entry + 20) = (uint16_t)((old_f.first_cluster >> 16) & 0xFFFF);
    *(uint16_t *)(entry + 26) = (uint16_t)(old_f.first_cluster & 0xFFFF);
    *(uint32_t *)(entry + 28) = old_f.size;

    /* Write new entry */
    uint32_t slot_cl, slot_off;
    if (find_free_dir_slot(fs, new_dir_cluster, &slot_cl, &slot_off) != 0) return -1;
    if (write_dir_entry(fs, slot_cl, slot_off, entry) != 0) return -1;

    /* Delete old entry */
    if (mark_dir_entry_deleted(fs, old_f.dir_cluster, old_f.dir_offset) != 0) {
        /* Try to undo the new entry — best effort */
        mark_dir_entry_deleted(fs, slot_cl, slot_off);
        return -1;
    }

    /* If this is a directory and we moved it to a different parent,
       update ".." entry to point to new parent */
    if ((old_f.attr & ATTR_DIR) && old_f.first_cluster >= 2) {
        char old_parent[260], old_name[260];
        if (split_path(old_path, old_parent, sizeof(old_parent),
                       old_name, sizeof(old_name)) == 0) {
            uint32_t old_dir_cluster;
            if (resolve_dir_cluster(fs, old_parent, &old_dir_cluster) == 0) {
                if (old_dir_cluster != new_dir_cluster) {
                    /* Update ".." in the directory's own cluster */
                    uint64_t lba = fat32_cluster_to_lba(fs, old_f.first_cluster);
                    uint8_t sector[FAT32_MAX_SECTOR];
                    if (fs->bytes_per_sector <= sizeof(sector)) {
                        if (read_sector(fs, lba, sector) == 0) {
                            /* ".." is at offset 32 */
                            uint32_t parent_cl = (new_dir_cluster == fs->root_cluster) ? 0 : new_dir_cluster;
                            *(uint16_t *)(sector + 32 + 20) = (uint16_t)((parent_cl >> 16) & 0xFFFF);
                            *(uint16_t *)(sector + 32 + 26) = (uint16_t)(parent_cl & 0xFFFF);
                            write_sector(fs, lba, sector);
                        }
                    }
                }
            }
        }
    }

    return 0;
}
