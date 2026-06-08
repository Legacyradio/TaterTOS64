#include "devfs.h"

#include <errno.h>
#include <stdint.h>

#include "vfs.h"
#include "../entropy/entropy.h"

enum devfs_node {
    DEVFS_NODE_NONE = 0,
    DEVFS_NODE_NULL,
    DEVFS_NODE_ZERO,
    DEVFS_NODE_RANDOM,
    DEVFS_NODE_URANDOM
};

static struct fs_ops devfs_ops;

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static enum devfs_node devfs_lookup(const char *path) {
    if (!path || path[0] == 0) return DEVFS_NODE_NONE;
    if (streq(path, "null")) return DEVFS_NODE_NULL;
    if (streq(path, "zero")) return DEVFS_NODE_ZERO;
    if (streq(path, "random")) return DEVFS_NODE_RANDOM;
    if (streq(path, "urandom")) return DEVFS_NODE_URANDOM;
    return DEVFS_NODE_NONE;
}

static int devfs_open(void *fs_data, const char *path, struct vfs_file *out) {
    (void)fs_data;
    if (!out) return -EINVAL;
    enum devfs_node node = devfs_lookup(path);
    if (node == DEVFS_NODE_NONE) return -ENOENT;
    out->private[0] = (uint8_t)node;
    out->size = 0;
    return 0;
}

static int devfs_read(struct vfs_file *f, void *buf, uint32_t len) {
    if (!f || !buf) return -EINVAL;
    enum devfs_node node = (enum devfs_node)f->private[0];
    if (len == 0) return 0;
    if (node == DEVFS_NODE_NULL) return 0;
    if (node == DEVFS_NODE_ZERO) {
        uint8_t *p = (uint8_t *)buf;
        for (uint32_t i = 0; i < len; i++) p[i] = 0;
        return (int)len;
    }
    if (node == DEVFS_NODE_RANDOM || node == DEVFS_NODE_URANDOM) {
        int rc = entropy_getbytes(buf, len);
        if (rc < 0) return rc;
        return (int)len;
    }
    return -EBADF;
}

static int devfs_write(struct vfs_file *f, const void *buf, uint32_t len) {
    (void)buf;
    if (!f) return -EINVAL;
    enum devfs_node node = (enum devfs_node)f->private[0];
    if (node == DEVFS_NODE_NULL ||
        node == DEVFS_NODE_ZERO ||
        node == DEVFS_NODE_RANDOM ||
        node == DEVFS_NODE_URANDOM) {
        return (int)len;
    }
    return -EBADF;
}

static int devfs_close(struct vfs_file *f) {
    (void)f;
    return 0;
}

static int devfs_readdir(void *fs_data, const char *path,
                         int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                         void *ctx) {
    (void)fs_data;
    if (!cb) return -EINVAL;
    if (path && path[0] != 0) return -ENOTDIR;
    if (cb("null", 0, 0, ctx)) return 1;
    if (cb("zero", 0, 0, ctx)) return 1;
    if (cb("random", 0, 0, ctx)) return 1;
    if (cb("urandom", 0, 0, ctx)) return 1;
    return 0;
}

static int devfs_stat(void *fs_data, const char *path, struct vfs_stat *out) {
    (void)fs_data;
    if (!out) return -EINVAL;
    if (!path || path[0] == 0) {
        out->size = 0;
        out->attr = 0x10u;
        return 0;
    }
    if (devfs_lookup(path) == DEVFS_NODE_NONE) return -ENOENT;
    out->size = 0;
    out->attr = 0;
    return 0;
}

static int64_t devfs_seek(struct vfs_file *f, int64_t offset, int whence) {
    (void)f;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

int devfs_mount(void) {
    devfs_ops.open = devfs_open;
    devfs_ops.read = devfs_read;
    devfs_ops.write = devfs_write;
    devfs_ops.close = devfs_close;
    devfs_ops.readdir = devfs_readdir;
    devfs_ops.stat = devfs_stat;
    devfs_ops.create = 0;
    devfs_ops.mkdir = 0;
    devfs_ops.unlink = 0;
    devfs_ops.seek = devfs_seek;
    devfs_ops.truncate = 0;
    devfs_ops.rename = 0;
    return vfs_mount("/dev", &devfs_ops, 0);
}
