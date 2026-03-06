#ifndef TATER_VFS_H
#define TATER_VFS_H

#include <stdint.h>

struct block_device {
    uint64_t sector_size;
    uint64_t total_sectors;
    int (*read_sector)(struct block_device *bd, uint64_t lba, void *buf);
    int (*write_sector)(struct block_device *bd, uint64_t lba, const void *buf);
    /*
     * Compatibility callbacks for legacy multi-sector callers.
     * New code should prefer read_sector/write_sector.
     */
    int (*read)(void *ctx, uint64_t lba, void *buf, uint32_t count);
    int (*write)(void *ctx, uint64_t lba, const void *buf, uint32_t count);
    void *ctx;
};

static inline int block_device_read(struct block_device *bd, uint64_t lba, void *buf, uint32_t count) {
    if (!bd || !buf) return -1;
    if (count == 0) return 0;
    if (bd->read) return bd->read(bd->ctx, lba, buf, count);
    if (!bd->read_sector) return -1;
    if (bd->sector_size == 0) return -1;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (bd->read_sector(bd, lba + i, p + (uint64_t)i * bd->sector_size) != 0) return -1;
    }
    return 0;
}

static inline int block_device_write(struct block_device *bd, uint64_t lba, const void *buf, uint32_t count) {
    if (!bd || !buf) return -1;
    if (count == 0) return 0;
    if (bd->write) return bd->write(bd->ctx, lba, buf, count);
    if (!bd->write_sector) return -1;
    if (bd->sector_size == 0) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (bd->write_sector(bd, lba + i, p + (uint64_t)i * bd->sector_size) != 0) return -1;
    }
    return 0;
}

struct vfs_stat {
    uint64_t size;
    uint32_t attr;
};

struct vfs_file;
struct vfs_mount;

struct fs_ops {
    int (*open)(void *fs_data, const char *path, struct vfs_file *out);
    int (*read)(struct vfs_file *f, void *buf, uint32_t len);
    int (*write)(struct vfs_file *f, const void *buf, uint32_t len);
    int (*close)(struct vfs_file *f);
    int (*readdir)(void *fs_data, const char *path,
                   int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                   void *ctx);
    int (*stat)(void *fs_data, const char *path, struct vfs_stat *out);
    int (*create)(void *fs_data, const char *path, uint16_t type);
    int (*mkdir)(void *fs_data, const char *path);
    int (*unlink)(void *fs_data, const char *path);
};

struct vfs_mount {
    char mountpoint[64];
    struct fs_ops *ops;
    void *fs_data;
    uint32_t open_refs;
    uint8_t active;
    uint8_t _pad[3];
    struct vfs_mount *next;
};

struct vfs_file {
    struct vfs_mount *mount;
    uint64_t size;
    uint8_t private[128];
};

struct vfs_storage_info {
    uint8_t  nvme_detected;
    uint8_t  root_fs_type;        /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  secondary_fs_type;   /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  flags;               /* bit0=root source is ramdisk/live media */
    uint32_t sector_size;
    uint64_t total_sectors;
    char     root_mount[16];
    char     secondary_mount[16];
};

#define VFS_STORAGE_FLAG_ROOT_RAMDISK_SOURCE 0x01u

struct vfs_path_fs_info {
    uint8_t  fs_type;             /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  pad[3];
    char     mount[16];
};

#define VFS_MAX_MOUNT_INFO 16u

struct vfs_mount_info {
    uint8_t  fs_type;             /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  pad[3];
    char     mount[64];
};

struct vfs_mounts_info {
    uint32_t count;
    struct vfs_mount_info entries[VFS_MAX_MOUNT_INFO];
};

struct vfs_mount_dbg {
    char     mount[64];
    uint8_t  fs_type;             /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  pad[3];
    uint32_t sector_size;         /* logical sector size for backing device */
    uint32_t block_size;          /* cluster/block size in bytes (0 if n/a) */
    uint64_t part_lba;            /* starting LBA of the partition (0 if ramdisk) */
};

struct vfs_mounts_dbg {
    uint32_t count;
    struct vfs_mount_dbg entries[VFS_MAX_MOUNT_INFO];
};

int vfs_init(struct block_device *bd);
int vfs_init_ramdisk(uint64_t phys_base, uint64_t size);
void vfs_set_storage_device(struct block_device *bd);
int vfs_mount(const char *mountpoint, struct fs_ops *ops, void *fs_data);
int vfs_umount(const char *mountpoint);
struct vfs_file *vfs_open(const char *path);
int vfs_read(struct vfs_file *f, void *buf, uint32_t len);
int vfs_write(struct vfs_file *f, const void *buf, uint32_t len);
int vfs_close(struct vfs_file *f);
uint32_t vfs_size(struct vfs_file *f);
int vfs_readdir(const char *path, int (*cb)(const char *name, void *ctx), void *ctx);
int vfs_readdir_ex(const char *path,
                   int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                   void *ctx);
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_create(const char *path, uint16_t type);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_mount_secondary(struct block_device *bd, const char *mountpoint);
int vfs_get_storage_info(struct vfs_storage_info *out);
int vfs_get_path_fs_info(const char *path, struct vfs_path_fs_info *out);
int vfs_get_mounts_info(struct vfs_mounts_info *out);
int vfs_get_mounts_dbg(struct vfs_mounts_dbg *out);

#endif
