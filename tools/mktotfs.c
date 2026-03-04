/*
 * mktotfs — Format a partition region as ToTFS
 *
 * Usage: mktotfs <image_file> <offset_bytes> <size_bytes>
 *
 * Compiles with system gcc (not cross compiler):
 *   gcc -O2 -o tools/mktotfs tools/mktotfs.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TOTFS_HOST_TOOL
#include "../src/kernel/fs/totfs.h"

static void write_block(FILE *f, uint64_t part_offset, uint64_t block_num,
                         const void *data, uint32_t len) {
    uint64_t off = part_offset + block_num * TOTFS_BLOCK_SIZE;
    fseek(f, (long)off, SEEK_SET);
    fwrite(data, 1, len, f);
}

static void set_bit(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: mktotfs <image_file> <offset_bytes> <size_bytes>\n");
        return 1;
    }

    const char *image_path = argv[1];
    uint64_t part_offset = (uint64_t)strtoull(argv[2], NULL, 0);
    uint64_t part_size   = (uint64_t)strtoull(argv[3], NULL, 0);

    if (part_size < TOTFS_BLOCK_SIZE * (TOTFS_DATA_START_BLK + 2)) {
        fprintf(stderr, "Partition too small (need at least %u bytes)\n",
                (unsigned)(TOTFS_BLOCK_SIZE * (TOTFS_DATA_START_BLK + 2)));
        return 1;
    }

    uint64_t total_blocks = part_size / TOTFS_BLOCK_SIZE;

    FILE *f = fopen(image_path, "r+b");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", image_path);
        return 1;
    }

    /* ── Superblock ──────────────────────────────────────────────────── */
    struct totfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic              = TOTFS_MAGIC;
    sb.version            = TOTFS_VERSION;
    sb.block_size         = TOTFS_BLOCK_SIZE;
    sb.log2_block_size    = TOTFS_LOG2_BLOCK;
    sb.total_blocks       = total_blocks;
    sb.total_inodes       = TOTFS_MAX_INODES;
    /* Metadata uses blocks 0..66 + 1 data block for root dir = 68 */
    sb.free_blocks        = total_blocks - (TOTFS_DATA_START_BLK + 1);
    sb.free_inodes        = TOTFS_MAX_INODES - 2;  /* inodes 0 and 1 used */
    sb.inode_bitmap_start = TOTFS_INODE_BITMAP_BLK;
    sb.inode_bitmap_blocks = 1;
    sb.block_bitmap_start = TOTFS_BLOCK_BITMAP_BLK;
    sb.block_bitmap_blocks = 1;
    sb.inode_table_start  = TOTFS_INODE_TABLE_BLK;
    sb.inode_table_blocks = TOTFS_INODE_TABLE_BLKS;
    sb.data_start         = TOTFS_DATA_START_BLK;
    sb.root_inode         = TOTFS_ROOT_INODE;
    sb.inode_size         = TOTFS_INODE_SIZE;
    memcpy(sb.volume_label, "TaterTOS64v3", 12);

    /* Write superblock (padded to full block) */
    uint8_t blk[TOTFS_BLOCK_SIZE];
    memset(blk, 0, sizeof(blk));
    memcpy(blk, &sb, sizeof(sb));
    write_block(f, part_offset, TOTFS_SUPERBLOCK_BLK, blk, TOTFS_BLOCK_SIZE);

    /* ── Inode Bitmap ────────────────────────────────────────────────── */
    uint8_t inode_bitmap[TOTFS_BLOCK_SIZE];
    memset(inode_bitmap, 0, sizeof(inode_bitmap));
    set_bit(inode_bitmap, 0);   /* inode 0 reserved */
    set_bit(inode_bitmap, 1);   /* inode 1 = root dir */
    write_block(f, part_offset, TOTFS_INODE_BITMAP_BLK, inode_bitmap, TOTFS_BLOCK_SIZE);

    /* ── Block Bitmap ────────────────────────────────────────────────── */
    uint8_t block_bitmap[TOTFS_BLOCK_SIZE];
    memset(block_bitmap, 0, sizeof(block_bitmap));
    /* Mark metadata blocks 0..66 as used */
    for (uint32_t i = 0; i < TOTFS_DATA_START_BLK; i++) {
        set_bit(block_bitmap, i);
    }
    /* Mark block 67 used (root directory data block) */
    set_bit(block_bitmap, TOTFS_DATA_START_BLK);
    write_block(f, part_offset, TOTFS_BLOCK_BITMAP_BLK, block_bitmap, TOTFS_BLOCK_SIZE);

    /* ── Inode Table (zero entire thing first) ───────────────────────── */
    memset(blk, 0, sizeof(blk));
    for (uint32_t i = 0; i < TOTFS_INODE_TABLE_BLKS; i++) {
        write_block(f, part_offset, TOTFS_INODE_TABLE_BLK + i, blk, TOTFS_BLOCK_SIZE);
    }

    /* Write root inode (inode 1) */
    struct totfs_inode root;
    memset(&root, 0, sizeof(root));
    root.type = TOTFS_TYPE_DIR;
    root.permissions = 0755;
    root.link_count = 2;  /* "." and parent */
    root.size = TOTFS_BLOCK_SIZE;  /* one data block */
    root.blocks_used = 1;
    root.extent_count = 1;
    root.extents[0].start_block = TOTFS_DATA_START_BLK;
    root.extents[0].block_count = 1;
    root.extents[0].file_block  = 0;

    /* Inode 1 is at offset 256 within inode table block 0 */
    memset(blk, 0, sizeof(blk));
    /* Read existing inode table block 0 (we already zeroed it) */
    memcpy(blk + TOTFS_INODE_SIZE, &root, sizeof(root));  /* inode 1 at byte 256 */
    write_block(f, part_offset, TOTFS_INODE_TABLE_BLK, blk, TOTFS_BLOCK_SIZE);

    /* ── Root Directory Data Block ───────────────────────────────────── */
    uint8_t dir_block[TOTFS_BLOCK_SIZE];
    memset(dir_block, 0, sizeof(dir_block));

    /* "." entry */
    struct totfs_dirent *dot = (struct totfs_dirent *)dir_block;
    dot->inode = TOTFS_ROOT_INODE;
    dot->name_len = 1;
    dot->file_type = TOTFS_TYPE_DIR;
    uint16_t dot_rec_len = (uint16_t)((8 + 1 + 3) & ~3u);  /* 12 bytes, 4-aligned */
    dot->rec_len = dot_rec_len;
    dir_block[8] = '.';  /* name immediately after 8-byte header */

    /* ".." entry */
    struct totfs_dirent *dotdot = (struct totfs_dirent *)(dir_block + dot_rec_len);
    dotdot->inode = TOTFS_ROOT_INODE;  /* root's parent is itself */
    dotdot->name_len = 2;
    dotdot->file_type = TOTFS_TYPE_DIR;
    /* Last entry in block: rec_len covers rest of block */
    dotdot->rec_len = (uint16_t)(TOTFS_BLOCK_SIZE - dot_rec_len);
    dir_block[dot_rec_len + 8] = '.';
    dir_block[dot_rec_len + 9] = '.';

    write_block(f, part_offset, TOTFS_DATA_START_BLK, dir_block, TOTFS_BLOCK_SIZE);

    fclose(f);

    printf("mktotfs: formatted %llu blocks (%llu MB) at offset %llu\n",
           (unsigned long long)total_blocks,
           (unsigned long long)(total_blocks * TOTFS_BLOCK_SIZE / (1024 * 1024)),
           (unsigned long long)part_offset);
    printf("  free_blocks=%llu free_inodes=%llu root_inode=%u\n",
           (unsigned long long)sb.free_blocks,
           (unsigned long long)sb.free_inodes,
           sb.root_inode);

    return 0;
}
