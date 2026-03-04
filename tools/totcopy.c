/*
 * totcopy — Copy files into a ToTFS image (host-side tool)
 *
 * Usage:
 *   totcopy <image> <part_offset> <host_file> <dest_path>
 *   totcopy <image> <part_offset> --list <dir_path>
 *
 * Compiles with system gcc (not cross compiler):
 *   gcc -O2 -o tools/totcopy tools/totcopy.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TOTFS_HOST_TOOL
#include "../src/kernel/fs/totfs.h"

/* ── Helpers ─────────────────────────────────────────────────────────── */

static FILE *g_img;
static uint64_t g_part_off;
static struct totfs_superblock g_sb;
static uint8_t g_inode_bmp[TOTFS_BLOCK_SIZE];
static uint8_t g_block_bmp[TOTFS_BLOCK_SIZE];

static void read_block(uint64_t bnum, void *buf) {
    uint64_t off = g_part_off + bnum * TOTFS_BLOCK_SIZE;
    fseek(g_img, (long)off, SEEK_SET);
    if (fread(buf, 1, TOTFS_BLOCK_SIZE, g_img) != TOTFS_BLOCK_SIZE) {
        fprintf(stderr, "read_block(%llu): short read\n", (unsigned long long)bnum);
    }
}

static void write_block(uint64_t bnum, const void *buf) {
    uint64_t off = g_part_off + bnum * TOTFS_BLOCK_SIZE;
    fseek(g_img, (long)off, SEEK_SET);
    fwrite(buf, 1, TOTFS_BLOCK_SIZE, g_img);
}

static int bit_test(const uint8_t *bmp, uint32_t bit) {
    return (bmp[bit / 8] >> (bit % 8)) & 1;
}

static void bit_set(uint8_t *bmp, uint32_t bit) {
    bmp[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void read_inode(uint32_t inum, struct totfs_inode *out) {
    uint32_t blk_idx = inum / (TOTFS_BLOCK_SIZE / TOTFS_INODE_SIZE);
    uint32_t off_in  = (inum % (TOTFS_BLOCK_SIZE / TOTFS_INODE_SIZE)) * TOTFS_INODE_SIZE;
    uint8_t buf[TOTFS_BLOCK_SIZE];
    read_block(TOTFS_INODE_TABLE_BLK + blk_idx, buf);
    memcpy(out, buf + off_in, sizeof(*out));
}

static void write_inode(uint32_t inum, const struct totfs_inode *inode) {
    uint32_t blk_idx = inum / (TOTFS_BLOCK_SIZE / TOTFS_INODE_SIZE);
    uint32_t off_in  = (inum % (TOTFS_BLOCK_SIZE / TOTFS_INODE_SIZE)) * TOTFS_INODE_SIZE;
    uint8_t buf[TOTFS_BLOCK_SIZE];
    read_block(TOTFS_INODE_TABLE_BLK + blk_idx, buf);
    memcpy(buf + off_in, inode, sizeof(*inode));
    write_block(TOTFS_INODE_TABLE_BLK + blk_idx, buf);
}

static uint32_t alloc_inode(void) {
    for (uint32_t i = 2; i < g_sb.total_inodes && i < TOTFS_BLOCK_SIZE * 8; i++) {
        if (!bit_test(g_inode_bmp, i)) {
            bit_set(g_inode_bmp, i);
            g_sb.free_inodes--;
            return i;
        }
    }
    fprintf(stderr, "alloc_inode: no free inodes\n");
    exit(1);
}

static uint64_t alloc_block(void) {
    for (uint64_t i = g_sb.data_start; i < g_sb.total_blocks && i < TOTFS_BLOCK_SIZE * 8; i++) {
        if (!bit_test(g_block_bmp, (uint32_t)i)) {
            bit_set(g_block_bmp, (uint32_t)i);
            g_sb.free_blocks--;
            return i;
        }
    }
    fprintf(stderr, "alloc_block: no free blocks\n");
    exit(1);
}

/* Try to allocate a contiguous run of blocks starting from a given block */
static uint64_t alloc_blocks_contig(uint32_t count, uint32_t *got) {
    uint64_t start = 0;
    uint32_t run = 0;
    for (uint64_t i = g_sb.data_start; i < g_sb.total_blocks && i < TOTFS_BLOCK_SIZE * 8; i++) {
        if (!bit_test(g_block_bmp, (uint32_t)i)) {
            if (run == 0) start = i;
            run++;
            if (run >= count) {
                /* Mark all as used */
                for (uint32_t j = 0; j < run; j++) {
                    bit_set(g_block_bmp, (uint32_t)(start + j));
                }
                g_sb.free_blocks -= run;
                *got = run;
                return start;
            }
        } else {
            run = 0;
        }
    }
    /* Couldn't get full run, allocate whatever we found */
    if (run > 0) {
        for (uint32_t j = 0; j < run; j++) {
            bit_set(g_block_bmp, (uint32_t)(start + j));
        }
        g_sb.free_blocks -= run;
        *got = run;
        return start;
    }
    fprintf(stderr, "alloc_blocks_contig: no free blocks\n");
    exit(1);
}

static int name_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return 1;
}

/* Look up a name in a directory inode, return child inode number or 0 */
static uint32_t dir_lookup(struct totfs_inode *dir, const char *name, uint32_t namelen) {
    uint8_t buf[TOTFS_BLOCK_SIZE];
    for (uint32_t e = 0; e < dir->extent_count; e++) {
        for (uint32_t b = 0; b < dir->extents[e].block_count; b++) {
            read_block(dir->extents[e].start_block + b, buf);
            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(buf + off);
                if (de->rec_len == 0) break;
                if (de->inode != 0 && name_eq(de->name, de->name_len, name, namelen)) {
                    return de->inode;
                }
                off += de->rec_len;
            }
        }
    }
    return 0;
}

/* Resolve a path like "/foo/bar.txt" → inode number. Returns 0 on failure. */
static uint32_t resolve_path(const char *path) {
    if (!path || !*path) return 0;
    while (*path == '/') path++;
    if (!*path) return TOTFS_ROOT_INODE;

    uint32_t cur_inum = TOTFS_ROOT_INODE;
    while (*path) {
        /* Extract next component */
        const char *comp = path;
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;

        struct totfs_inode dir;
        read_inode(cur_inum, &dir);
        if (dir.type != TOTFS_TYPE_DIR) return 0;

        uint32_t child = dir_lookup(&dir, comp, clen);
        if (!child) return 0;
        cur_inum = child;
        path += clen;
        while (*path == '/') path++;
    }
    return cur_inum;
}

/* Get parent directory path and basename from a full path */
static void split_path(const char *path, char *parent, uint32_t pmax,
                        const char **basename, uint32_t *blen) {
    /* Find last '/' */
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash <= 0) {
        /* Root directory is parent */
        strncpy(parent, "/", pmax);
        *basename = path;
        while (**basename == '/') (*basename)++;
        *blen = (uint32_t)strlen(*basename);
    } else {
        uint32_t plen = (uint32_t)last_slash;
        if (plen >= pmax) plen = pmax - 1;
        memcpy(parent, path, plen);
        parent[plen] = 0;
        *basename = path + last_slash + 1;
        *blen = (uint32_t)strlen(*basename);
    }
}

/* Add a directory entry to a directory inode. May allocate a new block. */
static void add_dirent(uint32_t dir_inum, uint32_t child_inum,
                        const char *name, uint32_t namelen, uint8_t file_type) {
    struct totfs_inode dir;
    read_inode(dir_inum, &dir);

    uint16_t needed = (uint16_t)(((8 + namelen) + 3u) & ~3u);

    /* Try to find space in existing directory blocks */
    for (uint32_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].block_count; b++) {
            uint8_t buf[TOTFS_BLOCK_SIZE];
            uint64_t bnum = dir.extents[e].start_block + b;
            read_block(bnum, buf);

            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(buf + off);
                if (de->rec_len == 0) break;

                if (de->inode == 0 && de->rec_len >= needed) {
                    /* Reuse deleted slot */
                    de->inode = child_inum;
                    de->name_len = (uint8_t)namelen;
                    de->file_type = file_type;
                    memcpy(de->name, name, namelen);
                    write_block(bnum, buf);
                    return;
                }

                /* Check if this entry has slack space at the end */
                uint16_t actual = (uint16_t)(((8 + de->name_len) + 3u) & ~3u);
                uint16_t slack = de->rec_len - actual;
                if (slack >= needed) {
                    /* Split: shrink current entry, insert new one in slack */
                    de->rec_len = actual;
                    struct totfs_dirent *ne = (struct totfs_dirent *)(buf + off + actual);
                    ne->inode = child_inum;
                    ne->rec_len = slack;
                    ne->name_len = (uint8_t)namelen;
                    ne->file_type = file_type;
                    memcpy(ne->name, name, namelen);
                    write_block(bnum, buf);
                    return;
                }
                off += de->rec_len;
            }
        }
    }

    /* Need a new block for the directory */
    if (dir.extent_count >= TOTFS_NUM_EXTENTS) {
        fprintf(stderr, "add_dirent: directory out of extents\n");
        exit(1);
    }
    uint64_t new_blk = alloc_block();
    uint8_t buf[TOTFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    struct totfs_dirent *de = (struct totfs_dirent *)buf;
    de->inode = child_inum;
    de->rec_len = (uint16_t)TOTFS_BLOCK_SIZE;  /* sole entry covers whole block */
    de->name_len = (uint8_t)namelen;
    de->file_type = file_type;
    memcpy(de->name, name, namelen);
    write_block(new_blk, buf);

    /* Add extent to directory inode */
    uint32_t ei = dir.extent_count;
    dir.extents[ei].start_block = new_blk;
    dir.extents[ei].block_count = 1;
    dir.extents[ei].file_block  = (uint32_t)(dir.size / TOTFS_BLOCK_SIZE);
    dir.extent_count++;
    dir.size += TOTFS_BLOCK_SIZE;
    dir.blocks_used++;
    write_inode(dir_inum, &dir);
}

/* ── --list command ──────────────────────────────────────────────────── */

static void do_list(const char *dir_path) {
    uint32_t dir_inum = resolve_path(dir_path);
    if (!dir_inum) {
        fprintf(stderr, "list: path not found: %s\n", dir_path);
        exit(1);
    }
    struct totfs_inode dir;
    read_inode(dir_inum, &dir);
    if (dir.type != TOTFS_TYPE_DIR) {
        fprintf(stderr, "list: not a directory: %s\n", dir_path);
        exit(1);
    }

    printf("Directory: %s (inode %u, %llu bytes, %u extents)\n",
           dir_path, dir_inum, (unsigned long long)dir.size, dir.extent_count);

    for (uint32_t e = 0; e < dir.extent_count; e++) {
        for (uint32_t b = 0; b < dir.extents[e].block_count; b++) {
            uint8_t buf[TOTFS_BLOCK_SIZE];
            read_block(dir.extents[e].start_block + b, buf);
            uint32_t off = 0;
            while (off + 8 <= TOTFS_BLOCK_SIZE) {
                struct totfs_dirent *de = (struct totfs_dirent *)(buf + off);
                if (de->rec_len == 0) break;
                if (de->inode != 0) {
                    char name[256];
                    memcpy(name, de->name, de->name_len);
                    name[de->name_len] = 0;

                    struct totfs_inode child;
                    read_inode(de->inode, &child);
                    const char *tstr = (child.type == TOTFS_TYPE_DIR) ? "DIR " : "FILE";
                    printf("  [%s] inode=%4u size=%8llu  %s\n",
                           tstr, de->inode, (unsigned long long)child.size, name);
                }
                off += de->rec_len;
            }
        }
    }
}

/* ── Copy file ───────────────────────────────────────────────────────── */

static void do_copy(const char *host_file, const char *dest_path) {
    /* Read host file */
    FILE *hf = fopen(host_file, "rb");
    if (!hf) {
        fprintf(stderr, "Cannot open host file: %s\n", host_file);
        exit(1);
    }
    fseek(hf, 0, SEEK_END);
    uint64_t file_size = (uint64_t)ftell(hf);
    fseek(hf, 0, SEEK_SET);

    uint8_t *file_data = NULL;
    if (file_size > 0) {
        file_data = (uint8_t *)malloc((size_t)file_size);
        if (!file_data) {
            fprintf(stderr, "malloc(%llu) failed\n", (unsigned long long)file_size);
            exit(1);
        }
        if (fread(file_data, 1, (size_t)file_size, hf) != (size_t)file_size) {
            fprintf(stderr, "short read on %s\n", host_file);
            exit(1);
        }
    }
    fclose(hf);

    /* Resolve parent directory */
    char parent[256];
    const char *basename;
    uint32_t blen;
    split_path(dest_path, parent, sizeof(parent), &basename, &blen);
    if (blen == 0) {
        fprintf(stderr, "Invalid dest path: %s\n", dest_path);
        exit(1);
    }

    uint32_t parent_inum = resolve_path(parent);
    if (!parent_inum) {
        fprintf(stderr, "Parent directory not found: %s\n", parent);
        exit(1);
    }

    /* Check if file already exists */
    struct totfs_inode parent_dir;
    read_inode(parent_inum, &parent_dir);
    uint32_t existing = dir_lookup(&parent_dir, basename, blen);
    if (existing) {
        fprintf(stderr, "File already exists: %s (inode %u)\n", dest_path, existing);
        exit(1);
    }

    /* Allocate inode */
    uint32_t new_inum = alloc_inode();

    /* Allocate data blocks and write file content */
    uint32_t blocks_needed = (uint32_t)((file_size + TOTFS_BLOCK_SIZE - 1) / TOTFS_BLOCK_SIZE);
    if (blocks_needed == 0) blocks_needed = 0;  /* empty file */

    struct totfs_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.type = TOTFS_TYPE_FILE;
    inode.permissions = 0644;
    inode.link_count = 1;
    inode.size = file_size;

    /* Allocate blocks in contiguous runs, building extents */
    uint32_t remaining = blocks_needed;
    uint32_t logical_block = 0;
    uint32_t ext_idx = 0;

    while (remaining > 0) {
        if (ext_idx >= TOTFS_NUM_EXTENTS) {
            fprintf(stderr, "File too fragmented (>%d extents)\n", TOTFS_NUM_EXTENTS);
            exit(1);
        }
        uint32_t got = 0;
        uint64_t start = alloc_blocks_contig(remaining, &got);
        inode.extents[ext_idx].start_block = start;
        inode.extents[ext_idx].block_count = got;
        inode.extents[ext_idx].file_block  = logical_block;
        ext_idx++;
        logical_block += got;
        remaining -= got;
    }
    inode.extent_count = ext_idx;
    inode.blocks_used = blocks_needed;

    /* Write file data to allocated blocks */
    uint64_t written = 0;
    for (uint32_t e = 0; e < inode.extent_count; e++) {
        for (uint32_t b = 0; b < inode.extents[e].block_count; b++) {
            uint8_t buf[TOTFS_BLOCK_SIZE];
            memset(buf, 0, sizeof(buf));
            uint64_t chunk = file_size - written;
            if (chunk > TOTFS_BLOCK_SIZE) chunk = TOTFS_BLOCK_SIZE;
            if (chunk > 0 && file_data) {
                memcpy(buf, file_data + written, (size_t)chunk);
            }
            write_block(inode.extents[e].start_block + b, buf);
            written += chunk;
        }
    }

    /* Write inode */
    write_inode(new_inum, &inode);

    /* Add directory entry to parent */
    add_dirent(parent_inum, new_inum, basename, blen, TOTFS_TYPE_FILE);

    free(file_data);

    printf("totcopy: %s -> %s (inode %u, %llu bytes, %u blocks, %u extents)\n",
           host_file, dest_path, new_inum,
           (unsigned long long)file_size, blocks_needed, ext_idx);
}

/* ── Flush metadata ──────────────────────────────────────────────────── */

static void flush_metadata(void) {
    /* Write bitmaps */
    write_block(g_sb.inode_bitmap_start, g_inode_bmp);
    write_block(g_sb.block_bitmap_start, g_block_bmp);

    /* Write superblock */
    uint8_t blk[TOTFS_BLOCK_SIZE];
    memset(blk, 0, sizeof(blk));
    memcpy(blk, &g_sb, sizeof(g_sb));
    write_block(0, blk);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  totcopy <image> <part_offset> <host_file> <dest_path>\n");
        fprintf(stderr, "  totcopy <image> <part_offset> --list <dir_path>\n");
        return 1;
    }

    const char *image = argv[1];
    g_part_off = (uint64_t)strtoull(argv[2], NULL, 0);

    g_img = fopen(image, "r+b");
    if (!g_img) {
        fprintf(stderr, "Cannot open %s\n", image);
        return 1;
    }

    /* Read superblock */
    uint8_t sb_blk[TOTFS_BLOCK_SIZE];
    read_block(0, sb_blk);
    memcpy(&g_sb, sb_blk, sizeof(g_sb));
    if (g_sb.magic != TOTFS_MAGIC) {
        fprintf(stderr, "Not a ToTFS partition (magic=%08x)\n", g_sb.magic);
        fclose(g_img);
        return 1;
    }

    /* Read bitmaps */
    read_block(g_sb.inode_bitmap_start, g_inode_bmp);
    read_block(g_sb.block_bitmap_start, g_block_bmp);

    if (argc >= 5 && strcmp(argv[3], "--list") == 0) {
        do_list(argv[4]);
    } else if (argc >= 5) {
        do_copy(argv[3], argv[4]);
        flush_metadata();
    } else {
        fprintf(stderr, "Invalid arguments\n");
        fclose(g_img);
        return 1;
    }

    fclose(g_img);
    return 0;
}
