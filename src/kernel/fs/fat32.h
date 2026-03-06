#ifndef TATER_FAT32_H
#define TATER_FAT32_H

#include <stdint.h>
#include "part.h"

struct fat32_fs {
    struct block_device *bd;
    uint64_t part_lba;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t total_clusters;
    uint32_t root_cluster;
    uint64_t fat_lba;
    uint64_t data_lba;
};

#define FAT32_ATTR_DIR 0x10

struct fat32_file {
    struct fat32_fs *fs;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t pos;
    uint32_t dir_cluster;
    uint32_t dir_offset;
    uint8_t attr;
};

int fat32_mount(struct fat32_fs *fs, struct block_device *bd, uint64_t part_lba);
int fat32_open(struct fat32_fs *fs, const char *path, struct fat32_file *out);
int fat32_read(struct fat32_file *f, void *buf, uint32_t len);
int fat32_write(struct fat32_file *f, const void *buf, uint32_t len);
int fat32_readdir(struct fat32_fs *fs, uint32_t dir_cluster,
                  int (*cb)(const char *name, uint8_t attr, uint32_t size, void *ctx),
                  void *ctx);
int fat32_stat(struct fat32_fs *fs, const char *path, uint32_t *size_out, uint8_t *attr_out);
int fat32_probe_bpb(struct block_device *bd, uint64_t part_lba);
int fat32_init(struct block_device *bd);

#endif
