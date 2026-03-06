#ifndef TATER_PART_H
#define TATER_PART_H

#include <stdint.h>

struct block_device;

struct gpt_part {
    uint8_t type_guid[16];
    uint8_t uniq_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    char name_utf16[72];
};

struct gpt_info {
    uint64_t part_lba;
    uint32_t part_count;
    uint32_t part_size;
};

struct gpt_loc {
    uint64_t start_lba;
    uint64_t size_lba;
    uint8_t type_guid[16];
    uint8_t uniq_guid[16];
};

int gpt_read_header(struct block_device *bd, struct gpt_info *info);
int gpt_read_part(struct block_device *bd, const struct gpt_info *info, uint32_t index, struct gpt_part *out);
int gpt_find_by_index(struct block_device *bd, uint32_t index, struct gpt_loc *out);
int gpt_find_by_type(struct block_device *bd, const uint8_t type_guid[16], struct gpt_loc *out);
int part_init(void);

#endif
