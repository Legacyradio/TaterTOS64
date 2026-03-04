/*
 * ToTFS — Tater OS Total Filesystem
 *
 * On-disk format + kernel driver declarations.
 * Shared between kernel and host tools (mktotfs, totcopy).
 */

#ifndef TATER_TOTFS_H
#define TATER_TOTFS_H

#include <stdint.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define TOTFS_MAGIC       0x544F5446  /* "TOTF" LE */
#define TOTFS_VERSION     1
#define TOTFS_BLOCK_SIZE  4096
#define TOTFS_LOG2_BLOCK  12

#define TOTFS_TYPE_FILE   1
#define TOTFS_TYPE_DIR    2

#define TOTFS_NUM_EXTENTS 10
#define TOTFS_MAX_INODES  1024
#define TOTFS_INODE_SIZE  256
#define TOTFS_ROOT_INODE  1

/* Fixed layout (all in 4K-block numbers) */
#define TOTFS_SUPERBLOCK_BLK    0
#define TOTFS_INODE_BITMAP_BLK  1
#define TOTFS_BLOCK_BITMAP_BLK  2
#define TOTFS_INODE_TABLE_BLK   3
#define TOTFS_INODE_TABLE_BLKS  64   /* 1024 inodes × 256 bytes / 4096 */
#define TOTFS_DATA_START_BLK    67   /* 3 + 64 */

/* GPT partition type GUID for ToTFS */
static const uint8_t TOTFS_PART_GUID[16] = {
    0x46, 0x54, 0x4F, 0x54,  /* "TOTF" LE32 */
    0x00, 0x53,              /* LE16 */
    0x54, 0x41,              /* LE16 */
    0x45, 0x52,              /* BE */
    0x54, 0x4F, 0x53, 0x36, 0x34, 0x00  /* "TOS64\0" BE */
};

/* ── On-Disk Structures ────────────────────────────────────────────────── */

struct totfs_superblock {
    uint32_t magic;                /* 0x00: TOTFS_MAGIC */
    uint32_t version;              /* 0x04 */
    uint32_t block_size;           /* 0x08: always 4096 */
    uint32_t log2_block_size;      /* 0x0C: always 12 */
    uint64_t total_blocks;         /* 0x10 */
    uint64_t total_inodes;         /* 0x18 */
    uint64_t free_blocks;          /* 0x20 */
    uint64_t free_inodes;          /* 0x28 */
    uint64_t inode_bitmap_start;   /* 0x30: block 1 */
    uint64_t inode_bitmap_blocks;  /* 0x38: 1 */
    uint64_t block_bitmap_start;   /* 0x40: block 2 */
    uint64_t block_bitmap_blocks;  /* 0x48: 1 */
    uint64_t inode_table_start;    /* 0x50: block 3 */
    uint64_t inode_table_blocks;   /* 0x58: 64 */
    uint64_t data_start;           /* 0x60: block 67 */
    uint32_t root_inode;           /* 0x68: always 1 */
    uint32_t inode_size;           /* 0x6C: always 256 */
    uint8_t  volume_label[32];     /* 0x70 */
    uint8_t  reserved[112];        /* pad to 256 bytes */
} __attribute__((packed));

struct totfs_extent {
    uint64_t start_block;   /* physical block number */
    uint32_t block_count;   /* contiguous blocks */
    uint32_t file_block;    /* starting logical block in file */
} __attribute__((packed));  /* 16 bytes */

struct totfs_inode {
    uint16_t type;            /* 0x00: TYPE_FILE or TYPE_DIR */
    uint16_t permissions;     /* 0x02: rwx bits (not enforced v1) */
    uint32_t link_count;      /* 0x04 */
    uint64_t size;            /* 0x08: 64-bit file size */
    uint64_t blocks_used;     /* 0x10 */
    uint64_t ctime;           /* 0x18: ms since boot */
    uint64_t mtime;           /* 0x20 */
    uint32_t uid;             /* 0x28: 0 for v1 */
    uint32_t gid;             /* 0x2C: 0 for v1 */
    uint32_t extent_count;    /* 0x30 */
    uint32_t flags;           /* 0x34: reserved */
    uint64_t indirect_block;  /* 0x38: overflow extents block (0=none) */
    struct totfs_extent extents[TOTFS_NUM_EXTENTS]; /* 0x40: 160 bytes */
    uint8_t  reserved[32];    /* 0xE0: pad to 256 */
} __attribute__((packed));

struct totfs_dirent {
    uint32_t inode;      /* 0 = deleted/free slot */
    uint16_t rec_len;    /* total record length (4-byte aligned) */
    uint8_t  name_len;   /* 1-255 */
    uint8_t  file_type;  /* 1=file, 2=dir */
    char     name[];     /* NOT null-terminated on disk */
} __attribute__((packed));

/* ── Kernel-side Structures ─────────────────────────────────────────────── */

#ifndef TOTFS_HOST_TOOL  /* Only in kernel builds */

struct block_device;    /* forward from part.h */
struct vfs_file;        /* forward from vfs.h */
struct vfs_stat;        /* forward from vfs.h */

/* Mount state (global static, one per mounted ToTFS volume) */
struct totfs_fs {
    struct block_device *bd;
    uint64_t part_lba;           /* LBA of partition start on NVMe */
    uint32_t sectors_per_block;  /* 4096 / sector_size */
    struct totfs_superblock sb;
    uint8_t  inode_bitmap[TOTFS_BLOCK_SIZE];  /* cached in RAM */
    uint8_t  block_bitmap[TOTFS_BLOCK_SIZE];  /* cached in RAM */
};

/* Per-open-file private state, fits in vfs_file.private[128] */
struct totfs_file_private {
    struct totfs_fs *fs;           /* 8 */
    uint32_t inode_num;            /* 4 */
    uint16_t type;                 /* 2 */
    uint16_t _pad;                 /* 2 */
    uint64_t size;                 /* 8 */
    uint64_t pos;                  /* 8 — current read/write position */
    uint32_t extent_count;         /* 4 */
    uint32_t flags;                /* 4 */
    struct totfs_extent extents[5]; /* 80 — first 5 cached inline */
    uint8_t  reserved[8];          /* 8 */
};                                 /* Total: 128 bytes exact */

/* ── Kernel Driver API ──────────────────────────────────────────────────── */

/* Probe / Mount */
int totfs_probe(struct block_device *bd, uint64_t part_lba);
int totfs_mount(struct totfs_fs *fs, struct block_device *bd, uint64_t part_lba);

/* Block I/O */
int totfs_read_block(struct totfs_fs *fs, uint64_t block_num, void *buf);
int totfs_write_block(struct totfs_fs *fs, uint64_t block_num, const void *buf);

/* Inode I/O */
int totfs_read_inode(struct totfs_fs *fs, uint32_t inum, struct totfs_inode *out);
int totfs_write_inode(struct totfs_fs *fs, uint32_t inum, const struct totfs_inode *inode);

/* Path resolution */
int totfs_resolve_path(struct totfs_fs *fs, const char *path, uint32_t *out_inum);
int totfs_dir_lookup(struct totfs_fs *fs, struct totfs_inode *dir,
                     const char *name, uint32_t namelen, uint32_t *child_inum);

/* File data */
int totfs_read_file_data(struct totfs_fs *fs, struct totfs_inode *inode,
                         uint64_t offset, void *buf, uint64_t len);
int totfs_write_file_data(struct totfs_fs *fs, struct totfs_inode *inode,
                          uint32_t inum, uint64_t offset, const void *buf, uint64_t len);

/* Allocation */
int totfs_alloc_block(struct totfs_fs *fs, uint64_t *out_block);
int totfs_free_block(struct totfs_fs *fs, uint64_t block);
int totfs_alloc_inode(struct totfs_fs *fs, uint32_t *out_inum);
int totfs_free_inode(struct totfs_fs *fs, uint32_t inum);

/* VFS fs_ops wrappers */
int totfs_open_vfs(void *fs_data, const char *path, struct vfs_file *out);
int totfs_read_vfs(struct vfs_file *f, void *buf, uint32_t len);
int totfs_write_vfs(struct vfs_file *f, const void *buf, uint32_t len);
int totfs_close_vfs(struct vfs_file *f);
int totfs_readdir_vfs(void *fs_data, const char *path,
                      int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                      void *ctx);
int totfs_stat_vfs(void *fs_data, const char *path, struct vfs_stat *out);

/* Create / Mkdir / Unlink */
int totfs_create_vfs(void *fs_data, const char *path, uint16_t type);
int totfs_mkdir_vfs(void *fs_data, const char *path);
int totfs_unlink_vfs(void *fs_data, const char *path);

#endif /* !TOTFS_HOST_TOOL */

#endif /* TATER_TOTFS_H */
