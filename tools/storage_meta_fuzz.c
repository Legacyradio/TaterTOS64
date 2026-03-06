#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "src/kernel/fs/vfs.h"
#include "src/kernel/fs/part.h"
#include "src/kernel/fs/ntfs.h"

struct mem_disk {
    uint8_t *bytes;
    uint64_t sector_size;
    uint64_t sectors;
};

static uint64_t rng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void write_le64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

static int mem_read(void *ctx, uint64_t lba, void *buf, uint32_t count) {
    struct mem_disk *d = (struct mem_disk *)ctx;
    if (!d || !buf) return -1;
    if (count == 0) return 0;
    if (lba >= d->sectors) return -1;
    if ((uint64_t)count > (d->sectors - lba)) return -1;

    uint64_t off = lba * d->sector_size;
    uint64_t len = (uint64_t)count * d->sector_size;
    memcpy(buf, d->bytes + off, (size_t)len);
    return 0;
}

static void mutate_bytes(uint8_t *buf, size_t len, uint64_t *state, uint32_t flips) {
    for (uint32_t i = 0; i < flips; i++) {
        size_t idx = (size_t)(rng_next(state) % (len ? len : 1u));
        buf[idx] ^= (uint8_t)rng_next(state);
    }
}

static void build_valid_gpt(uint8_t *disk, uint64_t sector_size, uint64_t sectors) {
    memset(disk, 0, (size_t)(sector_size * sectors));

    uint8_t *hdr = disk + sector_size * 1u;
    uint8_t *ent = disk + sector_size * 2u;

    memcpy(hdr + 0, "EFI PART", 8);
    write_le32(hdr + 8, 0x00010000u);
    write_le32(hdr + 12, 92u);
    write_le64(hdr + 24, 1u);
    write_le64(hdr + 32, sectors - 1u);
    write_le64(hdr + 40, 34u);
    write_le64(hdr + 48, (sectors > 34u) ? (sectors - 34u) : 2u);
    write_le64(hdr + 72, 2u);
    write_le32(hdr + 80, 16u);
    write_le32(hdr + 84, 128u);

    for (uint32_t i = 0; i < 16; i++) {
        ent[i] = (uint8_t)(0xA0u + i);
        ent[16u + i] = (uint8_t)(0x10u + i);
    }
    write_le64(ent + 32, 2048u);
    write_le64(ent + 40, 4095u);
    write_le64(ent + 48, 0u);

    /* "DATA" as UTF-16LE in the 72-byte name field */
    ent[56] = 'D'; ent[57] = 0;
    ent[58] = 'A'; ent[59] = 0;
    ent[60] = 'T'; ent[61] = 0;
    ent[62] = 'A'; ent[63] = 0;
}

static int run_gpt_fuzz(uint32_t iterations) {
    const uint64_t sector_size = 512u;
    const uint64_t sectors = 64u;
    uint8_t *disk = (uint8_t *)calloc((size_t)(sector_size * sectors), 1u);
    if (!disk) {
        fprintf(stderr, "storage_meta_fuzz: alloc failed\n");
        return 1;
    }

    build_valid_gpt(disk, sector_size, sectors);

    struct mem_disk md = {disk, sector_size, sectors};
    struct block_device bd;
    memset(&bd, 0, sizeof(bd));
    bd.sector_size = sector_size;
    bd.total_sectors = sectors;
    bd.read = mem_read;
    bd.ctx = &md;

    struct gpt_info info;
    struct gpt_part part;
    if (gpt_read_header(&bd, &info) != 0 || gpt_read_part(&bd, &info, 0, &part) != 0) {
        fprintf(stderr, "storage_meta_fuzz: baseline GPT parse failed\n");
        free(disk);
        return 1;
    }

    uint8_t base_hdr[512];
    uint8_t base_entry[128];
    memcpy(base_hdr, disk + sector_size * 1u, sizeof(base_hdr));
    memcpy(base_entry, disk + sector_size * 2u, sizeof(base_entry));

    uint64_t state = 0x4f6f6b6f6d656761ULL;
    for (uint32_t i = 0; i < iterations; i++) {
        uint8_t *hdr = disk + sector_size * 1u;
        uint8_t *ent = disk + sector_size * 2u;

        memcpy(hdr, base_hdr, sizeof(base_hdr));
        memcpy(ent, base_entry, sizeof(base_entry));

        mutate_bytes(hdr, sizeof(base_hdr), &state,
                     (uint32_t)(1u + (rng_next(&state) % 6u)));
        mutate_bytes(ent, sizeof(base_entry), &state,
                     (uint32_t)(1u + (rng_next(&state) % 6u)));

        if (gpt_read_header(&bd, &info) == 0) {
            uint64_t idx64 = rng_next(&state);
            if (info.part_count != 0) {
                idx64 %= info.part_count;
            } else {
                idx64 = 0;
            }
            (void)gpt_read_part(&bd, &info, (uint32_t)idx64, &part);
            (void)gpt_read_part(&bd, &info, info.part_count, &part);
        }
    }

    free(disk);
    return 0;
}

static int run_ntfs_fuzz(uint32_t iterations) {
    struct ntfs_data_run runs[32];
    uint8_t buf[256];
    uint64_t state = 0x6e7466735f66757aULL;

    for (uint32_t i = 0; i < iterations; i++) {
        uint32_t len = (uint32_t)(1u + (rng_next(&state) % sizeof(buf)));
        for (uint32_t j = 0; j < len; j++) {
            buf[j] = (uint8_t)rng_next(&state);
        }
        if ((rng_next(&state) & 3u) == 0) {
            buf[len - 1u] = 0;
        }

        int rc = ntfs_decode_data_runs(buf, len, runs, 32);
        if (rc > 32) {
            fprintf(stderr, "storage_meta_fuzz: ntfs rc overflow (%d)\n", rc);
            return 1;
        }
        if (rc >= 0) {
            for (int r = 0; r < rc; r++) {
                if (runs[r].length == 0) {
                    fprintf(stderr, "storage_meta_fuzz: ntfs zero-length run accepted\n");
                    return 1;
                }
            }
        }
    }

    /* Targeted malformed cases */
    {
        uint8_t malformed_oversize[] = {0xF1u, 0x01u, 0x00u};
        if (ntfs_decode_data_runs(malformed_oversize, sizeof(malformed_oversize), runs, 32) != -1) {
            fprintf(stderr, "storage_meta_fuzz: expected malformed_oversize failure\n");
            return 1;
        }

        uint8_t malformed_zero_len[] = {0x11u, 0x00u, 0x01u, 0x00u};
        if (ntfs_decode_data_runs(malformed_zero_len, sizeof(malformed_zero_len), runs, 32) != -1) {
            fprintf(stderr, "storage_meta_fuzz: expected malformed_zero_len failure\n");
            return 1;
        }

        uint8_t valid_sparse[] = {0x01u, 0x03u, 0x00u};
        int rc = ntfs_decode_data_runs(valid_sparse, sizeof(valid_sparse), runs, 32);
        if (rc != 1 || runs[0].lcn != UINT64_MAX || runs[0].length != 3u) {
            fprintf(stderr, "storage_meta_fuzz: sparse run decode mismatch\n");
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    uint32_t gpt_iters = 20000u;
    uint32_t ntfs_iters = 50000u;

    if (argc > 1) gpt_iters = (uint32_t)strtoul(argv[1], 0, 10);
    if (argc > 2) ntfs_iters = (uint32_t)strtoul(argv[2], 0, 10);
    if (gpt_iters == 0) gpt_iters = 1;
    if (ntfs_iters == 0) ntfs_iters = 1;

    if (run_gpt_fuzz(gpt_iters) != 0) return 1;
    if (run_ntfs_fuzz(ntfs_iters) != 0) return 1;

    printf("storage_meta_fuzz: OK (gpt=%u ntfs=%u)\n", gpt_iters, ntfs_iters);
    return 0;
}
