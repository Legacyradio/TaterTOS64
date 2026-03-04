/*
 * ToTFS — Tater OS Total Filesystem kernel driver
 *
 * Read/write extent-based filesystem for NVMe partitions.
 */

#include "totfs.h"
#include "vfs.h"
#include "part.h"

void kprint(const char *fmt, ...);

/* ── Block I/O ──────────────────────────────────────────────────────────── */

int totfs_read_block(struct totfs_fs *fs, uint64_t block_num, void *buf) {
    uint64_t lba = fs->part_lba + block_num * fs->sectors_per_block;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < fs->sectors_per_block; i++) {
        if (fs->bd->read(fs->bd->ctx, lba + i, p + i * fs->bd->sector_size, 1) != 0)
            return -1;
    }
    return 0;
}

int totfs_write_block(struct totfs_fs *fs, uint64_t block_num, const void *buf) {
    uint64_t lba = fs->part_lba + block_num * fs->sectors_per_block;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < fs->sectors_per_block; i++) {
        if (fs->bd->write(fs->bd->ctx, lba + i, p + i * fs->bd->sector_size, 1) != 0)
            return -1;
    }
    return 0;
}

/* ── Probe / Mount ──────────────────────────────────────────────────────── */

int totfs_probe(struct block_device *bd, uint64_t part_lba) {
    /* Read the first sector (512 bytes) and check for TOTF magic */
    uint8_t sector[512];
    if (bd->read(bd->ctx, part_lba, sector, 1) != 0) return 0;
    uint32_t magic = *(uint32_t *)sector;
    return magic == TOTFS_MAGIC;
}

int totfs_mount(struct totfs_fs *fs, struct block_device *bd, uint64_t part_lba) {
    fs->bd = bd;
    fs->part_lba = part_lba;
    fs->sectors_per_block = TOTFS_BLOCK_SIZE / (uint32_t)bd->sector_size;

    /* Read superblock */
    uint8_t blk[TOTFS_BLOCK_SIZE];
    if (totfs_read_block(fs, 0, blk) != 0) {
        kprint("ToTFS: failed to read superblock\n");
        return -1;
    }

    /* Copy superblock from block buffer */
    uint8_t *sbp = (uint8_t *)&fs->sb;
    for (uint32_t i = 0; i < sizeof(fs->sb); i++) sbp[i] = blk[i];

    if (fs->sb.magic != TOTFS_MAGIC) {
        kprint("ToTFS: bad magic 0x%08x\n", fs->sb.magic);
        return -1;
    }
    if (fs->sb.version != TOTFS_VERSION) {
        kprint("ToTFS: unsupported version %u\n", fs->sb.version);
        return -1;
    }

    /* Read bitmaps into RAM */
    if (totfs_read_block(fs, fs->sb.inode_bitmap_start, fs->inode_bitmap) != 0) {
        kprint("ToTFS: failed to read inode bitmap\n");
        return -1;
    }
    if (totfs_read_block(fs, fs->sb.block_bitmap_start, fs->block_bitmap) != 0) {
        kprint("ToTFS: failed to read block bitmap\n");
        return -1;
    }

    kprint("ToTFS: mounted at LBA %llu, %llu blocks, %llu free\n",
           (unsigned long long)part_lba,
           (unsigned long long)fs->sb.total_blocks,
           (unsigned long long)fs->sb.free_blocks);
    return 0;
}

/* ── Inode I/O ──────────────────────────────────────────────────────────── */

int totfs_read_inode(struct totfs_fs *fs, uint32_t inum, struct totfs_inode *out) {
    if (inum >= fs->sb.total_inodes) return -1;
    uint32_t inodes_per_block = TOTFS_BLOCK_SIZE / TOTFS_INODE_SIZE;
    uint32_t blk_idx = inum / inodes_per_block;
    uint32_t off_in  = (inum % inodes_per_block) * TOTFS_INODE_SIZE;

    uint8_t blk[TOTFS_BLOCK_SIZE];
    if (totfs_read_block(fs, fs->sb.inode_table_start + blk_idx, blk) != 0)
        return -1;

    uint8_t *src = blk + off_in;
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) dst[i] = src[i];
    return 0;
}

int totfs_write_inode(struct totfs_fs *fs, uint32_t inum, const struct totfs_inode *inode) {
    if (inum >= fs->sb.total_inodes) return -1;
    uint32_t inodes_per_block = TOTFS_BLOCK_SIZE / TOTFS_INODE_SIZE;
    uint32_t blk_idx = inum / inodes_per_block;
    uint32_t off_in  = (inum % inodes_per_block) * TOTFS_INODE_SIZE;

    uint8_t blk[TOTFS_BLOCK_SIZE];
    if (totfs_read_block(fs, fs->sb.inode_table_start + blk_idx, blk) != 0)
        return -1;

    const uint8_t *src = (const uint8_t *)inode;
    uint8_t *dst = blk + off_in;
    for (uint32_t i = 0; i < sizeof(*inode); i++) dst[i] = src[i];

    return totfs_write_block(fs, fs->sb.inode_table_start + blk_idx, blk);
}

/* ── Directory Lookup ───────────────────────────────────────────────────── */

static int totfs_name_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return 1;
}

int totfs_dir_lookup(struct totfs_fs *fs, struct totfs_inode *dir,
                     const char *name, uint32_t namelen, uint32_t *child_inum) {
    uint8_t blk[TOTFS_BLOCK_SIZE];
    for (uint32_t e = 0; e < dir->extent_count && e < TOTFS_NUM_EXTENTS; e++) {
        for (uint32_t b = 0; b < dir->extents[e].block_count; b++) {
            if (totfs_read_block(fs, dir->extents[e].start_block + b, blk) != 0)
                return -1;
            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(blk + off);
                if (de->rec_len == 0) break;
                if (de->inode != 0 &&
                    totfs_name_eq(de->name, de->name_len, name, namelen)) {
                    *child_inum = de->inode;
                    return 0;
                }
                off += de->rec_len;
            }
        }
    }
    return -1;  /* not found */
}

/* ── Path Resolution ────────────────────────────────────────────────────── */

int totfs_resolve_path(struct totfs_fs *fs, const char *path, uint32_t *out_inum) {
    /* Skip leading slashes */
    while (*path == '/') path++;
    if (!*path) {
        *out_inum = TOTFS_ROOT_INODE;
        return 0;
    }

    uint32_t cur_inum = TOTFS_ROOT_INODE;
    while (*path) {
        /* Extract component */
        const char *comp = path;
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;

        struct totfs_inode dir;
        if (totfs_read_inode(fs, cur_inum, &dir) != 0) return -1;
        if (dir.type != TOTFS_TYPE_DIR) return -1;

        if (totfs_dir_lookup(fs, &dir, comp, clen, &cur_inum) != 0)
            return -1;

        path += clen;
        while (*path == '/') path++;
    }

    *out_inum = cur_inum;
    return 0;
}

/* ── Extent-based File Read ─────────────────────────────────────────────── */

/* Find the physical block for a logical file block using extents */
static int totfs_find_phys_block(struct totfs_inode *inode, uint32_t logical_block,
                                  uint64_t *phys_block) {
    for (uint32_t e = 0; e < inode->extent_count && e < TOTFS_NUM_EXTENTS; e++) {
        uint32_t estart = inode->extents[e].file_block;
        uint32_t eend   = estart + inode->extents[e].block_count;
        if (logical_block >= estart && logical_block < eend) {
            *phys_block = inode->extents[e].start_block + (logical_block - estart);
            return 0;
        }
    }
    return -1;  /* block not mapped */
}

int totfs_read_file_data(struct totfs_fs *fs, struct totfs_inode *inode,
                         uint64_t offset, void *buf, uint64_t len) {
    if (offset >= inode->size) return 0;
    if (offset + len > inode->size) len = inode->size - offset;
    if (len == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t total_read = 0;
    uint8_t blk[TOTFS_BLOCK_SIZE];

    while (total_read < len) {
        uint64_t cur_off = offset + total_read;
        uint32_t logical_blk = (uint32_t)(cur_off / TOTFS_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)(cur_off % TOTFS_BLOCK_SIZE);

        uint64_t phys;
        if (totfs_find_phys_block(inode, logical_blk, &phys) != 0)
            break;

        if (totfs_read_block(fs, phys, blk) != 0)
            break;

        uint32_t chunk = TOTFS_BLOCK_SIZE - blk_off;
        if ((uint64_t)chunk > len - total_read) chunk = (uint32_t)(len - total_read);

        uint8_t *src = blk + blk_off;
        for (uint32_t i = 0; i < chunk; i++) out[total_read + i] = src[i];
        total_read += chunk;
    }

    return (int)total_read;
}

/* ── Write File Data ────────────────────────────────────────────────────── */

int totfs_write_file_data(struct totfs_fs *fs, struct totfs_inode *inode,
                          uint32_t inum, uint64_t offset, const void *buf, uint64_t len) {
    if (len == 0) return 0;

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t total_written = 0;
    uint8_t blk[TOTFS_BLOCK_SIZE];

    while (total_written < len) {
        uint64_t cur_off = offset + total_written;
        uint32_t logical_blk = (uint32_t)(cur_off / TOTFS_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)(cur_off % TOTFS_BLOCK_SIZE);

        uint64_t phys;
        if (totfs_find_phys_block(inode, logical_blk, &phys) != 0) {
            /* Need to allocate a new block */
            uint64_t new_blk;
            if (totfs_alloc_block(fs, &new_blk) != 0)
                break;

            /* Zero the new block */
            for (uint32_t i = 0; i < TOTFS_BLOCK_SIZE; i++) blk[i] = 0;
            totfs_write_block(fs, new_blk, blk);

            /* Add extent (try to extend last extent if contiguous) */
            if (inode->extent_count > 0) {
                struct totfs_extent *last = &inode->extents[inode->extent_count - 1];
                uint32_t last_end_file = last->file_block + last->block_count;
                uint64_t last_end_phys = last->start_block + last->block_count;
                if (logical_blk == last_end_file && new_blk == last_end_phys) {
                    last->block_count++;
                    phys = new_blk;
                    inode->blocks_used++;
                    goto have_block;
                }
            }
            if (inode->extent_count >= TOTFS_NUM_EXTENTS) {
                /* Out of extents, free the block and stop */
                totfs_free_block(fs, new_blk);
                break;
            }
            inode->extents[inode->extent_count].start_block = new_blk;
            inode->extents[inode->extent_count].block_count = 1;
            inode->extents[inode->extent_count].file_block  = logical_blk;
            inode->extent_count++;
            inode->blocks_used++;
            phys = new_blk;
        }

have_block:
        /* Read existing block if partial write */
        if (blk_off != 0 || (len - total_written) < TOTFS_BLOCK_SIZE) {
            if (totfs_read_block(fs, phys, blk) != 0) break;
        }

        uint32_t chunk = TOTFS_BLOCK_SIZE - blk_off;
        if ((uint64_t)chunk > len - total_written)
            chunk = (uint32_t)(len - total_written);

        for (uint32_t i = 0; i < chunk; i++) blk[blk_off + i] = in[total_written + i];

        if (totfs_write_block(fs, phys, blk) != 0) break;
        total_written += chunk;
    }

    /* Update file size if we wrote past the end */
    if (offset + total_written > inode->size) {
        inode->size = offset + total_written;
    }

    /* Write inode back */
    totfs_write_inode(fs, inum, inode);

    return (int)total_written;
}

/* ── Allocation ─────────────────────────────────────────────────────────── */

static int bitmap_test(const uint8_t *bmp, uint32_t bit) {
    return (bmp[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint8_t *bmp, uint32_t bit) {
    bmp[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bitmap_clear(uint8_t *bmp, uint32_t bit) {
    bmp[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

int totfs_alloc_block(struct totfs_fs *fs, uint64_t *out_block) {
    uint64_t max = fs->sb.total_blocks;
    if (max > TOTFS_BLOCK_SIZE * 8) max = TOTFS_BLOCK_SIZE * 8;
    for (uint64_t i = fs->sb.data_start; i < max; i++) {
        if (!bitmap_test(fs->block_bitmap, (uint32_t)i)) {
            bitmap_set(fs->block_bitmap, (uint32_t)i);
            fs->sb.free_blocks--;
            *out_block = i;
            /* Write bitmap + superblock */
            totfs_write_block(fs, fs->sb.block_bitmap_start, fs->block_bitmap);
            uint8_t sb_blk[TOTFS_BLOCK_SIZE];
            for (uint32_t j = 0; j < TOTFS_BLOCK_SIZE; j++) sb_blk[j] = 0;
            uint8_t *src = (uint8_t *)&fs->sb;
            for (uint32_t j = 0; j < sizeof(fs->sb); j++) sb_blk[j] = src[j];
            totfs_write_block(fs, 0, sb_blk);
            return 0;
        }
    }
    return -1;
}

int totfs_free_block(struct totfs_fs *fs, uint64_t block) {
    if (block >= fs->sb.total_blocks || block >= TOTFS_BLOCK_SIZE * 8) return -1;
    bitmap_clear(fs->block_bitmap, (uint32_t)block);
    fs->sb.free_blocks++;
    totfs_write_block(fs, fs->sb.block_bitmap_start, fs->block_bitmap);
    return 0;
}

int totfs_alloc_inode(struct totfs_fs *fs, uint32_t *out_inum) {
    uint32_t max = (uint32_t)fs->sb.total_inodes;
    if (max > TOTFS_BLOCK_SIZE * 8) max = TOTFS_BLOCK_SIZE * 8;
    for (uint32_t i = 2; i < max; i++) {
        if (!bitmap_test(fs->inode_bitmap, i)) {
            bitmap_set(fs->inode_bitmap, i);
            fs->sb.free_inodes--;
            *out_inum = i;
            totfs_write_block(fs, fs->sb.inode_bitmap_start, fs->inode_bitmap);
            uint8_t sb_blk[TOTFS_BLOCK_SIZE];
            for (uint32_t j = 0; j < TOTFS_BLOCK_SIZE; j++) sb_blk[j] = 0;
            uint8_t *src = (uint8_t *)&fs->sb;
            for (uint32_t j = 0; j < sizeof(fs->sb); j++) sb_blk[j] = src[j];
            totfs_write_block(fs, 0, sb_blk);
            return 0;
        }
    }
    return -1;
}

int totfs_free_inode(struct totfs_fs *fs, uint32_t inum) {
    if (inum >= fs->sb.total_inodes || inum >= TOTFS_BLOCK_SIZE * 8) return -1;
    bitmap_clear(fs->inode_bitmap, inum);
    fs->sb.free_inodes++;
    totfs_write_block(fs, fs->sb.inode_bitmap_start, fs->inode_bitmap);
    return 0;
}

/* ── VFS fs_ops Wrappers ────────────────────────────────────────────────── */

int totfs_open_vfs(void *fs_data, const char *path, struct vfs_file *out) {
    struct totfs_fs *fs = (struct totfs_fs *)fs_data;
    uint32_t inum;
    if (totfs_resolve_path(fs, path, &inum) != 0) return -1;

    struct totfs_inode inode;
    if (totfs_read_inode(fs, inum, &inode) != 0) return -1;

    struct totfs_file_private *priv = (struct totfs_file_private *)out->private;
    priv->fs = fs;
    priv->inode_num = inum;
    priv->type = inode.type;
    priv->_pad = 0;
    priv->size = inode.size;
    priv->pos = 0;
    priv->flags = 0;

    /* Cache up to 5 extents inline */
    uint32_t n = inode.extent_count;
    if (n > 5) n = 5;
    priv->extent_count = inode.extent_count;
    for (uint32_t i = 0; i < n; i++) priv->extents[i] = inode.extents[i];
    for (uint32_t i = n; i < 5; i++) {
        priv->extents[i].start_block = 0;
        priv->extents[i].block_count = 0;
        priv->extents[i].file_block  = 0;
    }

    out->size = inode.size;
    return 0;
}

int totfs_read_vfs(struct vfs_file *f, void *buf, uint32_t len) {
    struct totfs_file_private *priv = (struct totfs_file_private *)f->private;

    /* Re-read inode to get full extent list */
    struct totfs_inode inode;
    if (totfs_read_inode(priv->fs, priv->inode_num, &inode) != 0) return -1;

    int n = totfs_read_file_data(priv->fs, &inode, priv->pos, buf, len);
    if (n > 0) priv->pos += (uint64_t)n;
    return n;
}

int totfs_write_vfs(struct vfs_file *f, const void *buf, uint32_t len) {
    struct totfs_file_private *priv = (struct totfs_file_private *)f->private;

    struct totfs_inode inode;
    if (totfs_read_inode(priv->fs, priv->inode_num, &inode) != 0) return -1;

    int n = totfs_write_file_data(priv->fs, &inode, priv->inode_num,
                                   priv->pos, buf, len);
    if (n > 0) {
        priv->pos += (uint64_t)n;
        priv->size = inode.size;
        f->size = inode.size;
    }
    return n;
}

int totfs_close_vfs(struct vfs_file *f) {
    (void)f;
    return 0;
}

int totfs_readdir_vfs(void *fs_data, const char *path,
                      int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                      void *ctx) {
    struct totfs_fs *fs = (struct totfs_fs *)fs_data;
    uint32_t inum;

    /* Empty path or "/" means root */
    if (!path || !*path) {
        inum = TOTFS_ROOT_INODE;
    } else {
        if (totfs_resolve_path(fs, path, &inum) != 0) return -1;
    }

    struct totfs_inode dir;
    if (totfs_read_inode(fs, inum, &dir) != 0) return -1;
    if (dir.type != TOTFS_TYPE_DIR) return -1;

    uint8_t blk[TOTFS_BLOCK_SIZE];
    char name[256];

    for (uint32_t e = 0; e < dir.extent_count && e < TOTFS_NUM_EXTENTS; e++) {
        for (uint32_t b = 0; b < dir.extents[e].block_count; b++) {
            if (totfs_read_block(fs, dir.extents[e].start_block + b, blk) != 0)
                return -1;
            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(blk + off);
                if (de->rec_len == 0) break;
                if (de->inode != 0) {
                    /* Skip "." and ".." */
                    if (!(de->name_len == 1 && de->name[0] == '.') &&
                        !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                        uint32_t nl = de->name_len;
                        if (nl > 255) nl = 255;
                        for (uint32_t i = 0; i < nl; i++) name[i] = de->name[i];
                        name[nl] = 0;
                        struct totfs_inode inode;
                        uint64_t sz = 0;
                        uint32_t attr = 0x20;
                        if (totfs_read_inode(fs, de->inode, &inode) == 0) {
                            sz = inode.size;
                            if (inode.type == TOTFS_TYPE_DIR) attr = 0x10;
                        }
                        if (cb(name, sz, attr, ctx)) return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }
    return 0;
}

int totfs_stat_vfs(void *fs_data, const char *path, struct vfs_stat *out) {
    struct totfs_fs *fs = (struct totfs_fs *)fs_data;
    uint32_t inum;

    if (!path || !*path) {
        inum = TOTFS_ROOT_INODE;
    } else {
        if (totfs_resolve_path(fs, path, &inum) != 0) return -1;
    }

    struct totfs_inode inode;
    if (totfs_read_inode(fs, inum, &inode) != 0) return -1;

    if (out) {
        out->size = inode.size;
        out->attr = (inode.type == TOTFS_TYPE_DIR) ? 0x10 : 0x20;
    }
    return 0;
}

/* ── Create / Mkdir / Unlink ────────────────────────────────────────────── */

/* Split path into parent + basename */
static int totfs_split_path(const char *path, char *parent, uint32_t pmax,
                             const char **basename, uint32_t *blen) {
    while (*path == '/') path++;
    if (!*path) return -1;  /* can't create root */

    /* Find last '/' in remaining path */
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }

    if (last_slash < 0) {
        /* No subdirectory — parent is root */
        parent[0] = '/';
        parent[1] = 0;
        *basename = path;
        *blen = 0;
        while ((*basename)[*blen]) (*blen)++;
    } else {
        /* Copy parent portion */
        uint32_t plen = (uint32_t)last_slash;
        if (plen + 2 > pmax) plen = pmax - 2;
        parent[0] = '/';
        for (uint32_t i = 0; i < plen; i++) parent[i + 1] = path[i];
        parent[plen + 1] = 0;
        *basename = path + last_slash + 1;
        *blen = 0;
        while ((*basename)[*blen]) (*blen)++;
    }
    return 0;
}

/* Add a dirent to a directory inode */
static int totfs_add_dirent(struct totfs_fs *fs, uint32_t dir_inum,
                             uint32_t child_inum, const char *name,
                             uint32_t namelen, uint8_t file_type) {
    struct totfs_inode dir;
    if (totfs_read_inode(fs, dir_inum, &dir) != 0) return -1;

    uint16_t needed = (uint16_t)(((8 + namelen) + 3u) & ~3u);
    uint8_t blk[TOTFS_BLOCK_SIZE];

    /* Try to find space in existing blocks */
    for (uint32_t e = 0; e < dir.extent_count && e < TOTFS_NUM_EXTENTS; e++) {
        for (uint32_t b = 0; b < dir.extents[e].block_count; b++) {
            uint64_t bnum = dir.extents[e].start_block + b;
            if (totfs_read_block(fs, bnum, blk) != 0) return -1;

            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(blk + off);
                if (de->rec_len == 0) break;

                if (de->inode == 0 && de->rec_len >= needed) {
                    de->inode = child_inum;
                    de->name_len = (uint8_t)namelen;
                    de->file_type = file_type;
                    for (uint32_t i = 0; i < namelen; i++) de->name[i] = name[i];
                    return totfs_write_block(fs, bnum, blk);
                }

                uint16_t actual = (uint16_t)(((8 + de->name_len) + 3u) & ~3u);
                uint16_t slack = de->rec_len - actual;
                if (slack >= needed) {
                    de->rec_len = actual;
                    struct totfs_dirent *ne = (struct totfs_dirent *)(blk + off + actual);
                    ne->inode = child_inum;
                    ne->rec_len = slack;
                    ne->name_len = (uint8_t)namelen;
                    ne->file_type = file_type;
                    for (uint32_t i = 0; i < namelen; i++) ne->name[i] = name[i];
                    return totfs_write_block(fs, bnum, blk);
                }
                off += de->rec_len;
            }
        }
    }

    /* Allocate a new block for directory */
    if (dir.extent_count >= TOTFS_NUM_EXTENTS) return -1;

    uint64_t new_blk;
    if (totfs_alloc_block(fs, &new_blk) != 0) return -1;

    for (uint32_t i = 0; i < TOTFS_BLOCK_SIZE; i++) blk[i] = 0;
    struct totfs_dirent *de = (struct totfs_dirent *)blk;
    de->inode = child_inum;
    de->rec_len = (uint16_t)TOTFS_BLOCK_SIZE;
    de->name_len = (uint8_t)namelen;
    de->file_type = file_type;
    for (uint32_t i = 0; i < namelen; i++) de->name[i] = name[i];
    if (totfs_write_block(fs, new_blk, blk) != 0) return -1;

    uint32_t ei = dir.extent_count;
    dir.extents[ei].start_block = new_blk;
    dir.extents[ei].block_count = 1;
    dir.extents[ei].file_block  = (uint32_t)(dir.size / TOTFS_BLOCK_SIZE);
    dir.extent_count++;
    dir.size += TOTFS_BLOCK_SIZE;
    dir.blocks_used++;
    return totfs_write_inode(fs, dir_inum, &dir);
}

int totfs_create_vfs(void *fs_data, const char *path, uint16_t type) {
    struct totfs_fs *fs = (struct totfs_fs *)fs_data;
    char parent[128];
    const char *basename;
    uint32_t blen;

    if (totfs_split_path(path, parent, sizeof(parent), &basename, &blen) != 0)
        return -1;
    if (blen == 0) return -1;

    /* Resolve parent */
    uint32_t parent_inum;
    if (totfs_resolve_path(fs, parent, &parent_inum) != 0) return -1;

    /* Check parent is a directory */
    struct totfs_inode parent_dir;
    if (totfs_read_inode(fs, parent_inum, &parent_dir) != 0) return -1;
    if (parent_dir.type != TOTFS_TYPE_DIR) return -1;

    /* Check if already exists */
    uint32_t existing;
    if (totfs_dir_lookup(fs, &parent_dir, basename, blen, &existing) == 0)
        return -1;  /* already exists */

    /* Allocate inode */
    uint32_t new_inum;
    if (totfs_alloc_inode(fs, &new_inum) != 0) return -1;

    /* Initialize inode */
    struct totfs_inode inode;
    uint8_t *ip = (uint8_t *)&inode;
    for (uint32_t i = 0; i < sizeof(inode); i++) ip[i] = 0;
    inode.type = type;
    inode.permissions = (type == TOTFS_TYPE_DIR) ? 0755 : 0644;
    inode.link_count = 1;

    if (type == TOTFS_TYPE_DIR) {
        /* Allocate 1 block for "." and ".." */
        uint64_t dir_blk;
        if (totfs_alloc_block(fs, &dir_blk) != 0) {
            totfs_free_inode(fs, new_inum);
            return -1;
        }

        uint8_t dbuf[TOTFS_BLOCK_SIZE];
        for (uint32_t i = 0; i < TOTFS_BLOCK_SIZE; i++) dbuf[i] = 0;

        /* "." entry */
        struct totfs_dirent *dot = (struct totfs_dirent *)dbuf;
        dot->inode = new_inum;
        dot->name_len = 1;
        dot->file_type = TOTFS_TYPE_DIR;
        uint16_t dot_rec_len = (uint16_t)((8 + 1 + 3) & ~3u);
        dot->rec_len = dot_rec_len;
        dbuf[8] = '.';

        /* ".." entry */
        struct totfs_dirent *dotdot = (struct totfs_dirent *)(dbuf + dot_rec_len);
        dotdot->inode = parent_inum;
        dotdot->name_len = 2;
        dotdot->file_type = TOTFS_TYPE_DIR;
        dotdot->rec_len = (uint16_t)(TOTFS_BLOCK_SIZE - dot_rec_len);
        dbuf[dot_rec_len + 8] = '.';
        dbuf[dot_rec_len + 9] = '.';

        totfs_write_block(fs, dir_blk, dbuf);

        inode.size = TOTFS_BLOCK_SIZE;
        inode.blocks_used = 1;
        inode.extent_count = 1;
        inode.extents[0].start_block = dir_blk;
        inode.extents[0].block_count = 1;
        inode.extents[0].file_block  = 0;
        inode.link_count = 2;
    }

    /* Write inode */
    if (totfs_write_inode(fs, new_inum, &inode) != 0) return -1;

    /* Add directory entry to parent */
    return totfs_add_dirent(fs, parent_inum, new_inum, basename, blen,
                             (uint8_t)type);
}

int totfs_mkdir_vfs(void *fs_data, const char *path) {
    return totfs_create_vfs(fs_data, path, TOTFS_TYPE_DIR);
}

int totfs_unlink_vfs(void *fs_data, const char *path) {
    struct totfs_fs *fs = (struct totfs_fs *)fs_data;
    char parent[128];
    const char *basename;
    uint32_t blen;

    if (totfs_split_path(path, parent, sizeof(parent), &basename, &blen) != 0)
        return -1;
    if (blen == 0) return -1;

    /* Resolve parent */
    uint32_t parent_inum;
    if (totfs_resolve_path(fs, parent, &parent_inum) != 0) return -1;

    struct totfs_inode parent_dir;
    if (totfs_read_inode(fs, parent_inum, &parent_dir) != 0) return -1;
    if (parent_dir.type != TOTFS_TYPE_DIR) return -1;

    /* Find the entry to unlink */
    uint32_t target_inum;
    if (totfs_dir_lookup(fs, &parent_dir, basename, blen, &target_inum) != 0)
        return -1;

    /* Read target inode to free its blocks */
    struct totfs_inode target;
    if (totfs_read_inode(fs, target_inum, &target) != 0) return -1;

    /* Don't unlink non-empty directories */
    if (target.type == TOTFS_TYPE_DIR) {
        /* Check if directory has entries beyond "." and ".." */
        uint8_t blk[TOTFS_BLOCK_SIZE];
        for (uint32_t e = 0; e < target.extent_count && e < TOTFS_NUM_EXTENTS; e++) {
            for (uint32_t b = 0; b < target.extents[e].block_count; b++) {
                if (totfs_read_block(fs, target.extents[e].start_block + b, blk) != 0)
                    return -1;
                uint32_t off = 0;
                while (off + 8 <= TOTFS_BLOCK_SIZE) {
                    struct totfs_dirent *de = (struct totfs_dirent *)(blk + off);
                    if (de->rec_len == 0) break;
                    if (de->inode != 0 &&
                        !(de->name_len == 1 && de->name[0] == '.') &&
                        !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                        return -1;  /* directory not empty */
                    }
                    off += de->rec_len;
                }
            }
        }
    }

    /* Free data blocks */
    for (uint32_t e = 0; e < target.extent_count && e < TOTFS_NUM_EXTENTS; e++) {
        for (uint32_t b = 0; b < target.extents[e].block_count; b++) {
            totfs_free_block(fs, target.extents[e].start_block + b);
        }
    }

    /* Free inode */
    totfs_free_inode(fs, target_inum);

    /* Mark dirent inode=0 in parent */
    uint8_t blk[TOTFS_BLOCK_SIZE];
    for (uint32_t e = 0; e < parent_dir.extent_count && e < TOTFS_NUM_EXTENTS; e++) {
        for (uint32_t b = 0; b < parent_dir.extents[e].block_count; b++) {
            uint64_t bnum = parent_dir.extents[e].start_block + b;
            if (totfs_read_block(fs, bnum, blk) != 0) continue;

            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(blk + off);
                if (de->rec_len == 0) break;
                if (de->inode == target_inum &&
                    totfs_name_eq(de->name, de->name_len, basename, blen)) {
                    de->inode = 0;  /* mark deleted */
                    totfs_write_block(fs, bnum, blk);
                    return 0;
                }
                off += de->rec_len;
            }
        }
    }

    return 0;
}
