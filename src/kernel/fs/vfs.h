#ifndef TATER_VFS_H
#define TATER_VFS_H

#include <stdint.h>
#include "part.h"

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
    uint8_t  pad;
    uint32_t sector_size;
    uint64_t total_sectors;
    char     root_mount[16];
    char     secondary_mount[16];
};

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

int vfs_init(struct block_device *bd);
int vfs_init_ramdisk(uint64_t phys_base, uint64_t size);
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

#endif
