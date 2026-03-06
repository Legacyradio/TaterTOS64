// GPT partition parser

#include "vfs.h"
#include "part.h"

void kprint(const char *fmt, ...);
struct block_device *nvme_get_block_device(void);

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void guid_hex(const uint8_t *g, char *out) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        out[i * 2 + 0] = hex[(g[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[g[i] & 0xF];
    }
    out[32] = 0;
}

struct gpt_header {
    uint8_t signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t part_entry_count;
    uint32_t part_entry_size;
    uint32_t part_array_crc32;
} __attribute__((packed));

#define GPT_HEADER_MIN_SIZE 92u
#define GPT_SCAN_MAX_LBA    8192u

static int sig_ok(const uint8_t *s) {
    return s[0]=='E' && s[1]=='F' && s[2]=='I' && s[3]==' ' &&
           s[4]=='P' && s[5]=='A' && s[6]=='R' && s[7]=='T';
}

static int header_sane(const struct gpt_header *h, uint64_t sector_size) {
    if (!h) return 0;
    if (!sig_ok(h->signature)) return 0;
    if (h->header_size < GPT_HEADER_MIN_SIZE || h->header_size > sector_size) return 0;
    if (h->part_entry_count == 0) return 0;
    if (h->part_entry_size < 128 || h->part_entry_size > sector_size) return 0;
    if ((sector_size / h->part_entry_size) == 0) return 0;
    return 1;
}

static int gpt_read_header_at(struct block_device *bd, uint64_t lba, struct gpt_header *out) {
    if (!bd || !out) return -1;
    if (bd->sector_size < 512 || bd->sector_size > 4096) return -1;

    uint8_t buf[4096];
    if (block_device_read(bd, lba, buf, 1) != 0) {
        return -1;
    }
    *out = *(const struct gpt_header *)buf;
    return 0;
}

static int gpt_info_from_header(const struct gpt_header *h, uint64_t found_lba, struct gpt_info *info) {
    if (!h || !info) return -1;

    int64_t delta = (int64_t)found_lba - (int64_t)h->current_lba;
    int64_t part_lba = (int64_t)h->part_entry_lba + delta;
    if (part_lba < 0) return -1;

    info->part_lba = (uint64_t)part_lba;
    info->part_count = h->part_entry_count;
    info->part_size = h->part_entry_size;
    return 0;
}

int gpt_read_header(struct block_device *bd, struct gpt_info *info) {
    if (!bd || !info || bd->sector_size < 512 || bd->sector_size > 4096) {
        return -1;
    }

    struct gpt_header h;
    if (gpt_read_header_at(bd, 1, &h) == 0 && header_sane(&h, bd->sector_size)) {
        return gpt_info_from_header(&h, 1, info);
    }

    if (bd->total_sectors > 1) {
        uint64_t backup_lba = bd->total_sectors - 1;
        if (backup_lba != 1 &&
            gpt_read_header_at(bd, backup_lba, &h) == 0 &&
            header_sane(&h, bd->sector_size)) {
            return gpt_info_from_header(&h, backup_lba, info);
        }
    }

    uint64_t scan_limit = GPT_SCAN_MAX_LBA;
    if (bd->total_sectors != 0 && bd->total_sectors < scan_limit) {
        scan_limit = bd->total_sectors;
    }
    for (uint64_t lba = 2; lba < scan_limit; lba++) {
        if (gpt_read_header_at(bd, lba, &h) != 0) continue;
        if (!header_sane(&h, bd->sector_size)) continue;
        if (gpt_info_from_header(&h, lba, info) == 0) {
            return 0;
        }
    }

    return -1;
}

int gpt_read_part(struct block_device *bd, const struct gpt_info *info, uint32_t index, struct gpt_part *out) {
    if (!bd || !info || !out) {
        return -1;
    }
    if (index >= info->part_count || info->part_size < 128 || info->part_size > bd->sector_size) {
        return -1;
    }

    uint64_t entries_per_lba = bd->sector_size / info->part_size;
    if (entries_per_lba == 0) {
        return -1;
    }
    uint64_t lba = info->part_lba + (index / entries_per_lba);
    uint32_t idx_in_lba = index % entries_per_lba;

    uint8_t buf[4096];
    if (bd->sector_size > sizeof(buf)) {
        return -1;
    }
    if (block_device_read(bd, lba, buf, 1) != 0) {
        return -1;
    }

    uint8_t *p = buf + idx_in_lba * info->part_size;
    // GPT entry layout
    for (int i = 0; i < 16; i++) out->type_guid[i] = p[i];
    for (int i = 0; i < 16; i++) out->uniq_guid[i] = p[16 + i];
    out->first_lba = *(uint64_t *)(p + 32);
    out->last_lba = *(uint64_t *)(p + 40);
    out->attrs = *(uint64_t *)(p + 48);
    /* GPT name field is exactly 72 bytes (36 UTF-16LE chars) at offset 56. */
    for (int i = 0; i < 72; i++) out->name_utf16[i] = (char)p[56 + i];

    return 0;
}

int gpt_find_by_index(struct block_device *bd, uint32_t index, struct gpt_loc *out) {
    if (!out) return -1;
    struct gpt_info info;
    if (gpt_read_header(bd, &info) != 0) return -1;
    struct gpt_part p;
    if (gpt_read_part(bd, &info, index, &p) != 0) return -1;
    out->start_lba = p.first_lba;
    out->size_lba = (p.last_lba >= p.first_lba) ? (p.last_lba - p.first_lba + 1) : 0;
    for (int i = 0; i < 16; i++) {
        out->type_guid[i] = p.type_guid[i];
        out->uniq_guid[i] = p.uniq_guid[i];
    }
    return 0;
}

int gpt_find_by_type(struct block_device *bd, const uint8_t type_guid[16], struct gpt_loc *out) {
    if (!bd || !out || !type_guid) return -1;
    struct gpt_info info;
    if (gpt_read_header(bd, &info) != 0) return -1;
    for (uint32_t i = 0; i < info.part_count; i++) {
        struct gpt_part p;
        if (gpt_read_part(bd, &info, i, &p) != 0) continue;
        if (guid_eq(p.type_guid, type_guid)) {
            out->start_lba = p.first_lba;
            out->size_lba = (p.last_lba >= p.first_lba) ? (p.last_lba - p.first_lba + 1) : 0;
            for (int j = 0; j < 16; j++) {
                out->type_guid[j] = p.type_guid[j];
                out->uniq_guid[j] = p.uniq_guid[j];
            }
            return 0;
        }
    }
    return -1;
}

int part_init(void) {
    struct block_device *bd = nvme_get_block_device();
    if (!bd) {
        kprint("GPT: no block device\n");
        return -1;
    }

    struct gpt_info info;
    if (gpt_read_header(bd, &info) != 0) {
        kprint("GPT: header read failed\n");
        return -1;
    }

    kprint("GPT: entries=%u entry_size=%u\n", info.part_count, info.part_size);
    for (uint32_t i = 0; i < info.part_count; i++) {
        struct gpt_part p;
        if (gpt_read_part(bd, &info, i, &p) != 0) continue;
        if (p.first_lba == 0 || p.last_lba == 0) continue;
        char type_hex[33];
        guid_hex(p.type_guid, type_hex);
        kprint("GPT: idx=%u start=%llu size=%llu type=%s\n",
               i, p.first_lba, (p.last_lba - p.first_lba + 1), type_hex);
    }
    return 0;
}
