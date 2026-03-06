/*
 * NTFS — Read-Only NTFS Filesystem Driver for TaterTOS64v3
 *
 * Supports reading files and directories from NTFS partitions.
 * Write operations are not supported (returns -1).
 */

#include "ntfs.h"
#include "vfs.h"
#include "part.h"

void kprint(const char *fmt, ...);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
void nvme_trace_set(uint32_t trace_id, uint32_t budget);

#define NTFS_TRACE_BUDGET 96u
#define NTFS_LCN_SPARSE UINT64_MAX

static uint32_t g_ntfs_trace_id = 0;
static uint32_t g_ntfs_trace_budget = 0;
static int g_ntfs_trace_active = 0;

static void ntfs_trace_emit(const char *tag, uint64_t a, uint64_t b, uint64_t c) {
    if (!g_ntfs_trace_active || g_ntfs_trace_budget == 0) return;
    g_ntfs_trace_budget--;
    kprint("NTFS_TRACE[%u] %s a=%llu b=%llu c=%llu\n",
           g_ntfs_trace_id, tag,
           (unsigned long long)a,
           (unsigned long long)b,
           (unsigned long long)c);
    if (g_ntfs_trace_budget == 0) {
        kprint("NTFS_TRACE[%u] budget exhausted\n", g_ntfs_trace_id);
    }
}

static void ntfs_trace_start(const char *phase, const char *path, uint64_t mft_index) {
    g_ntfs_trace_active = 1;
    g_ntfs_trace_budget = NTFS_TRACE_BUDGET;
    g_ntfs_trace_id++;
    if (g_ntfs_trace_id == 0) g_ntfs_trace_id = 1;
    kprint("NTFS_TRACE[%u] begin %s path=\"%s\" mft=%llu\n",
           g_ntfs_trace_id, phase, path ? path : "",
           (unsigned long long)mft_index);
    nvme_trace_set(g_ntfs_trace_id, NTFS_TRACE_BUDGET);
}

static void ntfs_trace_stop(const char *phase, int rc) {
    if (g_ntfs_trace_active) {
        kprint("NTFS_TRACE[%u] end %s rc=%d\n", g_ntfs_trace_id, phase, rc);
    }
    g_ntfs_trace_active = 0;
    g_ntfs_trace_budget = 0;
    nvme_trace_set(0, 0);
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void ntfs_memcpy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static void ntfs_memset(void *dst, uint8_t val, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++) d[i] = val;
}

static int ntfs_memcmp(const void *a, const void *b, uint32_t len) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static char ntfs_toupper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* ── Small MFT LRU cache (per-volume) ──────────────────────────────────── */

static void ntfs_mft_cache_init(struct ntfs_fs *fs) {
    if (!fs) return;
    fs->mft_cache_tick = 1;
    for (uint32_t i = 0; i < NTFS_MFT_CACHE_SIZE; i++) {
        fs->mft_cache[i].index = UINT64_MAX;
        fs->mft_cache[i].stamp = 0;
        fs->mft_cache[i].buf = 0;
    }
}

static uint8_t *ntfs_mft_cache_lookup(struct ntfs_fs *fs, uint64_t index) {
    if (!fs) return 0;
    for (uint32_t i = 0; i < NTFS_MFT_CACHE_SIZE; i++) {
        if (fs->mft_cache[i].index == index && fs->mft_cache[i].buf) {
            fs->mft_cache[i].stamp = ++fs->mft_cache_tick;
            return fs->mft_cache[i].buf;
        }
    }
    return 0;
}

static void ntfs_mft_cache_store(struct ntfs_fs *fs, uint64_t index, void *record) {
    if (!fs || !record) return;

    /* Exact hit: refresh content and stamp. */
    for (uint32_t i = 0; i < NTFS_MFT_CACHE_SIZE; i++) {
        if (fs->mft_cache[i].index == index && fs->mft_cache[i].buf) {
            ntfs_memcpy(fs->mft_cache[i].buf, record, fs->mft_record_size);
            fs->mft_cache[i].stamp = ++fs->mft_cache_tick;
            return;
        }
    }

    /* Choose slot: prefer empty, else oldest stamp. */
    uint32_t victim = 0;
    uint32_t oldest_stamp = UINT32_MAX;
    for (uint32_t i = 0; i < NTFS_MFT_CACHE_SIZE; i++) {
        if (!fs->mft_cache[i].buf) { victim = i; oldest_stamp = 0; break; }
        if (fs->mft_cache[i].stamp < oldest_stamp) {
            oldest_stamp = fs->mft_cache[i].stamp;
            victim = i;
        }
    }

    if (!fs->mft_cache[victim].buf) {
        fs->mft_cache[victim].buf = (uint8_t *)kmalloc(fs->mft_record_size);
        if (!fs->mft_cache[victim].buf) return;
    }

    ntfs_memcpy(fs->mft_cache[victim].buf, record, fs->mft_record_size);
    fs->mft_cache[victim].index = index;
    fs->mft_cache[victim].stamp = ++fs->mft_cache_tick;
}

/* Convert UTF-16LE to ASCII (best effort — non-ASCII becomes '?') */
static uint32_t ntfs_utf16_to_ascii(const uint16_t *utf16, uint32_t ulen,
                                     char *ascii, uint32_t amax) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < ulen && j + 1 < amax; i++) {
        uint16_t ch = utf16[i];
        if (ch >= 0x20 && ch < 0x7F) {
            ascii[j++] = (char)ch;
        } else {
            ascii[j++] = '?';
        }
    }
    ascii[j] = 0;
    return j;
}

/* Case-insensitive name compare (ASCII vs UTF-16LE single char) */
static int ntfs_name_match(const char *ascii, uint32_t alen,
                            const uint16_t *utf16, uint32_t ulen) {
    if (alen != ulen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        char a = ntfs_toupper(ascii[i]);
        uint16_t u = utf16[i];
        char b = (u >= 0x20 && u < 0x7F) ? ntfs_toupper((char)u) : '?';
        if (a != b) return 0;
    }
    return 1;
}

static struct ntfs_attr_header *ntfs_find_data_attr(void *mft_record,
                                                     uint32_t record_size);
static int ntfs_read_nonresident(struct ntfs_fs *fs, struct ntfs_attr_header *attr,
                                 void *buf, uint64_t offset, uint64_t len);

/* Read arbitrary bytes from a partition using device-reported logical sectors. */
static int ntfs_dev_read_bytes(struct block_device *bd, uint64_t part_lba,
                               uint64_t byte_off, void *buf, uint32_t len) {
    if (!bd || !bd->read || !buf) return -1;
    if (len == 0) return 0;

    uint64_t dev_sec_size = bd->sector_size;
    if (dev_sec_size < 512 || dev_sec_size > 4096) dev_sec_size = 512;

    ntfs_trace_emit("dev_read:req", byte_off, len, dev_sec_size);

    uint8_t *out = (uint8_t *)buf;
    uint8_t *sec = 0;

    uint64_t lba = byte_off / dev_sec_size;
    uint32_t in_sec = (uint32_t)(byte_off % dev_sec_size);
    uint32_t remaining = len;

    if (in_sec != 0) {
        sec = (uint8_t *)kmalloc(dev_sec_size);
        if (!sec) return -1;
        if (part_lba > UINT64_MAX - lba) {
            kfree(sec);
            return -1;
        }
        ntfs_trace_emit("dev_read:head", part_lba + lba, 1, dev_sec_size);
        if (bd->read(bd->ctx, part_lba + lba, sec, 1) != 0) {
            ntfs_trace_emit("dev_read:head_err", part_lba + lba, 1, dev_sec_size);
            kfree(sec);
            return -1;
        }
        uint32_t chunk = (uint32_t)(dev_sec_size - in_sec);
        if (chunk > remaining) chunk = remaining;
        ntfs_memcpy(out, sec + in_sec, chunk);
        out += chunk;
        remaining -= chunk;
        lba++;
        in_sec = 0;
    }

    uint32_t max_bytes = 1024u * 1024u;
    uint32_t max_sectors = (uint32_t)(max_bytes / dev_sec_size);
    if (max_sectors == 0) max_sectors = 1;

    while (remaining >= dev_sec_size) {
        uint32_t sectors = (uint32_t)(remaining / dev_sec_size);
        if (sectors > max_sectors) sectors = max_sectors;
        if (part_lba > UINT64_MAX - lba) {
            if (sec) kfree(sec);
            return -1;
        }
        ntfs_trace_emit("dev_read:bulk", part_lba + lba, sectors, dev_sec_size);
        if (bd->read(bd->ctx, part_lba + lba, out, sectors) != 0) {
            ntfs_trace_emit("dev_read:bulk_err", part_lba + lba, sectors, dev_sec_size);
            if (sec) kfree(sec);
            return -1;
        }
        uint32_t bytes = (uint32_t)(sectors * dev_sec_size);
        out += bytes;
        remaining -= bytes;
        lba += sectors;
    }

    if (remaining > 0) {
        if (!sec) {
            sec = (uint8_t *)kmalloc(dev_sec_size);
            if (!sec) return -1;
        }
        if (part_lba > UINT64_MAX - lba) {
            kfree(sec);
            return -1;
        }
        ntfs_trace_emit("dev_read:tail", part_lba + lba, 1, dev_sec_size);
        if (bd->read(bd->ctx, part_lba + lba, sec, 1) != 0) {
            ntfs_trace_emit("dev_read:tail_err", part_lba + lba, 1, dev_sec_size);
            kfree(sec);
            return -1;
        }
        ntfs_memcpy(out, sec, remaining);
    }

    if (sec) kfree(sec);
    return 0;
}

/* Read partition boot sector safely regardless of logical sector size.
 * Copies first 512 bytes into 'boot512' and validates the 0x55AA trailer
 * using BPB bytes/sector. */
static int ntfs_read_boot_sector(struct block_device *bd, uint64_t part_lba,
                                 uint8_t boot512[512], uint16_t *out_bps) {
    if (!bd || !bd->read || !boot512) return -1;

    /* Tiered geometry: read raw bytes first, then trust BPB bytes/sector. */
    if (ntfs_dev_read_bytes(bd, part_lba, 0, boot512, 512) != 0) return -1;

    uint16_t bps = (uint16_t)boot512[0x0B] | ((uint16_t)boot512[0x0C] << 8);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0) return -1;

    uint8_t *sector = (uint8_t *)kmalloc(bps);
    if (!sector) return -1;
    if (ntfs_dev_read_bytes(bd, part_lba, 0, sector, bps) != 0) {
        kfree(sector);
        return -1;
    }
    if (sector[bps - 2] != 0x55 || sector[bps - 1] != 0xAA) {
        kfree(sector);
        return -1;
    }

    if (out_bps) *out_bps = bps;
    ntfs_memcpy(boot512, sector, 512);
    kfree(sector);
    return 0;
}

/* Read NTFS sectors from partition-relative LBA.
 * rel_lba/count are in NTFS bytes_per_sector units, not device sector units. */
static int ntfs_read_sectors(struct ntfs_fs *fs, uint64_t rel_lba,
                              void *buf, uint32_t count) {
    if (!fs || !buf || fs->bytes_per_sector == 0) return -1;
    if (count == 0) return 0;
    if (rel_lba > UINT64_MAX / fs->bytes_per_sector) return -1;

    uint64_t byte_off = rel_lba * fs->bytes_per_sector;
    uint64_t byte_len = (uint64_t)count * fs->bytes_per_sector;
    if (byte_len > UINT32_MAX) return -1;

    return ntfs_dev_read_bytes(fs->bd, fs->part_lba, byte_off, buf, (uint32_t)byte_len);
}

/* Read a cluster from the partition */
static int ntfs_read_cluster(struct ntfs_fs *fs, uint64_t lcn, void *buf) {
    if (!fs || fs->sectors_per_cluster == 0) return -1;
    if (lcn > UINT64_MAX / fs->sectors_per_cluster) return -1;
    uint64_t lba = lcn * fs->sectors_per_cluster;
    ntfs_trace_emit("cluster:read", lcn, fs->sectors_per_cluster, fs->cluster_size);
    return ntfs_read_sectors(fs, lba, buf, fs->sectors_per_cluster);
}

/* Bootstrap reader: assumes $MFT is contiguous from boot-sector mft_lcn. */
static int ntfs_read_mft_entry_linear_raw(struct ntfs_fs *fs, uint64_t index, void *buf) {
    if (!fs || !buf || fs->bytes_per_sector == 0 || fs->mft_record_size == 0) return -1;
    if (index > UINT64_MAX / fs->mft_record_size) return -1;
    if (fs->mft_lcn > UINT64_MAX / fs->sectors_per_cluster) return -1;

    uint64_t entry_off = index * fs->mft_record_size;
    uint64_t mft_base_lba = fs->mft_lcn * fs->sectors_per_cluster;
    uint64_t lba_delta = entry_off / fs->bytes_per_sector;
    uint32_t in_sector = (uint32_t)(entry_off % fs->bytes_per_sector);
    if (mft_base_lba > UINT64_MAX - lba_delta) return -1;
    uint64_t lba = mft_base_lba + lba_delta;

    uint32_t remaining = fs->mft_record_size;
    uint8_t *out = (uint8_t *)buf;
    uint8_t *sec = (uint8_t *)kmalloc(fs->bytes_per_sector);
    if (!sec) return -1;

    while (remaining > 0) {
        if (ntfs_read_sectors(fs, lba, sec, 1) != 0) {
            kfree(sec);
            return -1;
        }

        uint32_t chunk = fs->bytes_per_sector - in_sector;
        if (chunk > remaining) chunk = remaining;
        ntfs_memcpy(out, sec + in_sector, chunk);
        out += chunk;
        remaining -= chunk;
        in_sector = 0;
        if (remaining > 0) {
            if (lba == UINT64_MAX) {
                kfree(sec);
                return -1;
            }
            lba++;
        }
    }

    kfree(sec);
    return 0;
}

/* Read bytes from logical $MFT stream via cached $MFT data runs. */
static int ntfs_read_mft_stream(struct ntfs_fs *fs, uint64_t offset, void *buf, uint32_t len) {
    if (len == 0) return 0;
    if (fs->mft_run_count == 0) return -1;

    ntfs_trace_emit("mft_stream:begin", offset, len, fs->mft_run_count);

    uint8_t *out = (uint8_t *)buf;
    uint8_t *clus_buf = (uint8_t *)kmalloc(fs->cluster_size);
    if (!clus_buf) return -1;

    while (len > 0) {
        uint64_t run_file_off = 0;
        uint32_t found_run = (uint32_t)-1;

        for (uint32_t r = 0; r < fs->mft_run_count; r++) {
            if (fs->mft_runs[r].length == 0) continue;
            if (fs->mft_runs[r].length > UINT64_MAX / fs->cluster_size) {
                kfree(clus_buf);
                return -1;
            }
            uint64_t run_bytes = fs->mft_runs[r].length * fs->cluster_size;
            if (run_file_off > UINT64_MAX - run_bytes) {
                kfree(clus_buf);
                return -1;
            }
            if (offset < run_file_off + run_bytes) {
                found_run = r;
                break;
            }
            run_file_off += run_bytes;
        }

        if (found_run == (uint32_t)-1) {
            kfree(clus_buf);
            return -1;
        }
        if (fs->mft_runs[found_run].lcn == NTFS_LCN_SPARSE) {
            kfree(clus_buf);
            return -1;
        }

        uint64_t in_run = offset - run_file_off;
        uint64_t clus_idx = in_run / fs->cluster_size;
        uint32_t clus_off = (uint32_t)(in_run % fs->cluster_size);
        uint32_t chunk = fs->cluster_size - clus_off;
        if (chunk > len) chunk = len;

        ntfs_trace_emit("mft_stream:cluster", fs->mft_runs[found_run].lcn + clus_idx,
                        fs->sectors_per_cluster, fs->cluster_size);
        if (ntfs_read_cluster(fs, fs->mft_runs[found_run].lcn + clus_idx, clus_buf) != 0) {
            kfree(clus_buf);
            return -1;
        }

        ntfs_memcpy(out, clus_buf + clus_off, chunk);
        out += chunk;
        offset += chunk;
        len -= chunk;
    }

    kfree(clus_buf);
    return 0;
}

/* ── Probe / Mount ──────────────────────────────────────────────────────── */

int ntfs_probe(struct block_device *bd, uint64_t part_lba) {
    uint8_t boot[512];
    uint16_t bps = 0;
    if (!bd || !bd->read) return 0;
    if (ntfs_read_boot_sector(bd, part_lba, boot, &bps) != 0) return 0;
    (void)bps;

    /* Check OEM ID at offset 3: "NTFS    " */
    if (ntfs_memcmp(boot + 3, NTFS_OEM_ID, 8) != 0) return 0;

    return 1;
}

int ntfs_mount(struct ntfs_fs *fs, struct block_device *bd, uint64_t part_lba) {
    ntfs_memset(fs, 0, sizeof(*fs));
    fs->bd = bd;
    fs->part_lba = part_lba;
    ntfs_mft_cache_init(fs);

    /* Read boot sector */
    uint8_t sector[512];
    uint16_t bytes_per_sector = 0;
    if (ntfs_read_boot_sector(bd, part_lba, sector, &bytes_per_sector) != 0) {
        kprint("NTFS: failed to read boot sector\n");
        return -1;
    }

    struct ntfs_boot_sector *bs = (struct ntfs_boot_sector *)sector;

    /* Validate */
    if (ntfs_memcmp(bs->oem_id, NTFS_OEM_ID, 8) != 0) {
        kprint("NTFS: bad OEM ID\n");
        return -1;
    }

    fs->bytes_per_sector = bytes_per_sector;
    fs->sectors_per_cluster = bs->sectors_per_cluster;
    if (fs->sectors_per_cluster == 0) {
        kprint("NTFS: invalid sectors/cluster 0\n");
        return -1;
    }
    if (fs->bytes_per_sector > UINT32_MAX / fs->sectors_per_cluster) {
        kprint("NTFS: cluster size overflow\n");
        return -1;
    }
    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->mft_lcn = bs->mft_lcn;

    /* Compute MFT record size from clusters_per_mft_record:
     * If positive: record_size = val * cluster_size
     * If negative: record_size = 2^|val| bytes */
    if (bs->clusters_per_mft_record > 0) {
        fs->mft_record_size = (uint32_t)bs->clusters_per_mft_record * fs->cluster_size;
    } else {
        uint32_t shift = (uint32_t)(-(int32_t)bs->clusters_per_mft_record);
        if (shift > 30) {
            kprint("NTFS: invalid MFT record shift %u\n", shift);
            return -1;
        }
        fs->mft_record_size = 1u << shift;
    }
    fs->sectors_per_mft_record =
        (fs->mft_record_size + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    if (fs->sectors_per_mft_record == 0) fs->sectors_per_mft_record = 1;

    /* Bootstrap from MFT entry 0 and cache $MFT data runs for fragmented MFTs. */
    uint8_t *mft0 = (uint8_t *)kmalloc(fs->mft_record_size);
    if (!mft0) return -1;

    if (ntfs_read_mft_entry(fs, NTFS_FILE_MFT, mft0) != 0) {
        kprint("NTFS: failed to read MFT entry 0\n");
        kfree(mft0);
        return -1;
    }

    struct ntfs_attr_header *mft_data_attr =
        ntfs_find_data_attr(mft0, fs->mft_record_size);
    if (!mft_data_attr || !mft_data_attr->non_resident) {
        kprint("NTFS: MFT entry 0 missing non-resident $DATA\n");
        kfree(mft0);
        return -1;
    }

    struct ntfs_attr_nonresident *mft_nr =
        (struct ntfs_attr_nonresident *)mft_data_attr;
    if (mft_nr->data_run_offset >= mft_data_attr->length) {
        kprint("NTFS: MFT entry 0 invalid data-run offset\n");
        kfree(mft0);
        return -1;
    }

    const uint8_t *mft_run_ptr = (const uint8_t *)mft_data_attr + mft_nr->data_run_offset;
    uint32_t mft_run_space = mft_data_attr->length - mft_nr->data_run_offset;
    int mft_rc = ntfs_decode_data_runs(mft_run_ptr, mft_run_space,
                                       fs->mft_runs, NTFS_MAX_RUNS);
    if (mft_rc <= 0) {
        kprint("NTFS: failed to decode MFT data runs\n");
        kfree(mft0);
        return -1;
    }
    fs->mft_run_count = (uint32_t)mft_rc;
    fs->mft_data_size = mft_nr->real_size;

    uint64_t mft_run_cover = 0;
    for (uint32_t i = 0; i < fs->mft_run_count; i++) {
        if (fs->mft_runs[i].length > UINT64_MAX / fs->cluster_size) {
            kprint("NTFS: MFT run[%u] size overflow\n", i);
            kfree(mft0);
            return -1;
        }
        uint64_t run_bytes = fs->mft_runs[i].length * fs->cluster_size;
        if (mft_run_cover > UINT64_MAX - run_bytes) {
            kprint("NTFS: MFT run coverage overflow\n");
            kfree(mft0);
            return -1;
        }
        mft_run_cover += run_bytes;
    }
    if (mft_run_cover < fs->mft_data_size) {
        kprint("NTFS: MFT run coverage too small (%llu < %llu)\n",
               (unsigned long long)mft_run_cover,
               (unsigned long long)fs->mft_data_size);
        kfree(mft0);
        return -1;
    }

    kfree(mft0);

    kprint("NTFS: mounted at LBA %llu, %u B/sec, %u sec/clus, "
           "cluster=%u, MFT@LCN=%llu, rec=%u, mft_runs=%u\n",
           (unsigned long long)part_lba,
           fs->bytes_per_sector,
           fs->sectors_per_cluster,
           fs->cluster_size,
           (unsigned long long)fs->mft_lcn,
           fs->mft_record_size,
           fs->mft_run_count);

    return 0;
}

/* ── MFT Record Access ──────────────────────────────────────────────────── */

/* Apply fixup array to an MFT record or index block */
static int ntfs_apply_fixup(void *record, uint32_t record_size,
                             uint16_t fixup_offset, uint16_t fixup_count,
                             uint32_t sector_size) {
    uint8_t *rec = (uint8_t *)record;
    uint16_t *fixup_array = (uint16_t *)(rec + fixup_offset);

    /* fixup_array[0] = update sequence number (magic)
     * fixup_array[1..N] = replacement values for last 2 bytes of each sector */
    if (!record || record_size == 0 || sector_size == 0) return -1;
    if (record_size % sector_size != 0) return -1;
    if (fixup_offset > record_size - 2) return -1;
    uint32_t fixup_bytes = (uint32_t)fixup_count * 2u;
    if (fixup_bytes == 0 || fixup_bytes > record_size - fixup_offset) return -1;

    uint16_t seq_val = fixup_array[0];
    uint32_t num_sectors = record_size / sector_size;

    if (fixup_count < num_sectors + 1) return -1;

    for (uint32_t i = 0; i < num_sectors; i++) {
        uint16_t *last_word = (uint16_t *)(rec + (i + 1) * sector_size - 2);
        if (*last_word != seq_val) {
            kprint("NTFS: fixup mismatch sector %u: got 0x%04x expected 0x%04x\n",
                   i, *last_word, seq_val);
            return -1;
        }
        *last_word = fixup_array[i + 1];
    }
    return 0;
}

int ntfs_read_mft_entry(struct ntfs_fs *fs, uint64_t index, void *buf) {
    if (!fs || !buf || fs->mft_record_size == 0) return -1;
    if (index > UINT64_MAX / fs->mft_record_size) return -1;

    /* Fast path: cached record */
    uint8_t *cached = ntfs_mft_cache_lookup(fs, index);
    if (cached) {
        ntfs_memcpy(buf, cached, fs->mft_record_size);
        return 0;
    }

    ntfs_trace_emit("mft:read", index, fs->mft_record_size, fs->mft_run_count);

    uint64_t entry_off = index * fs->mft_record_size;
    if (entry_off > UINT64_MAX - fs->mft_record_size) return -1;
    if (fs->mft_data_size &&
        entry_off + fs->mft_record_size > fs->mft_data_size)
        return -1;

    if (fs->mft_run_count > 0) {
        if (ntfs_read_mft_stream(fs, entry_off, buf, fs->mft_record_size) != 0)
            return -1;
    } else {
        if (ntfs_read_mft_entry_linear_raw(fs, index, buf) != 0)
            return -1;
    }

    /* Check FILE magic */
    struct ntfs_mft_entry *mft = (struct ntfs_mft_entry *)buf;
    if (mft->magic != NTFS_MAGIC) {
        kprint("NTFS: MFT entry %llu bad magic 0x%08x\n",
               (unsigned long long)index, mft->magic);
        return -1;
    }

    /* Apply fixup */
    if (ntfs_apply_fixup(buf, fs->mft_record_size,
                         mft->fixup_offset, mft->fixup_count,
                         fs->bytes_per_sector) != 0) {
        return -1;
    }

    ntfs_mft_cache_store(fs, index, buf);
    return 0;
}

/* ── Attribute Walking ──────────────────────────────────────────────────── */

struct ntfs_attr_header *ntfs_find_attr(void *mft_record, uint32_t record_size,
                                         uint32_t type) {
    struct ntfs_mft_entry *mft = (struct ntfs_mft_entry *)mft_record;
    uint8_t *base = (uint8_t *)mft_record;
    uint32_t off = mft->attrs_offset;

    if (record_size < sizeof(struct ntfs_mft_entry)) return 0;
    if (off < sizeof(struct ntfs_mft_entry) || off >= record_size) return 0;
    while (off + sizeof(struct ntfs_attr_header) <= record_size) {
        struct ntfs_attr_header *ah = (struct ntfs_attr_header *)(base + off);
        if (ah->type == NTFS_ATTR_END || ah->type == 0) break;
        if (ah->length == 0 || ah->length > record_size - off) break;
        if (ah->type == type) return ah;
        off += ah->length;
    }
    return 0;
}

static struct ntfs_attr_header *ntfs_find_data_attr(void *mft_record,
                                                     uint32_t record_size) {
    struct ntfs_mft_entry *mft = (struct ntfs_mft_entry *)mft_record;
    uint8_t *base = (uint8_t *)mft_record;
    uint32_t off = mft->attrs_offset;
    struct ntfs_attr_header *unnamed_fallback = 0;
    struct ntfs_attr_header *named_fallback = 0;

    if (record_size < sizeof(struct ntfs_mft_entry)) return 0;
    if (off < sizeof(struct ntfs_mft_entry) || off >= record_size) return 0;
    while (off + sizeof(struct ntfs_attr_header) <= record_size) {
        struct ntfs_attr_header *ah = (struct ntfs_attr_header *)(base + off);
        if (ah->type == NTFS_ATTR_END || ah->type == 0) break;
        if (ah->length == 0 || ah->length > record_size - off) break;
        if (ah->type == NTFS_ATTR_DATA) {
            if (ah->name_length == 0) {
                if (!unnamed_fallback) unnamed_fallback = ah;
                if (!ah->non_resident) return ah;
                if (ah->length >= sizeof(struct ntfs_attr_nonresident)) {
                    struct ntfs_attr_nonresident *nr =
                        (struct ntfs_attr_nonresident *)ah;
                    if (nr->start_vcn == 0) return ah;
                }
            } else if (!named_fallback) {
                named_fallback = ah;
            }
        }
        off += ah->length;
    }
    return unnamed_fallback ? unnamed_fallback : named_fallback;
}

/* ── Data Run Decoding ──────────────────────────────────────────────────── */

static int ntfs_lcn_apply_delta(uint64_t *cur_lcn, int64_t delta) {
    if (!cur_lcn) return -1;
    if (delta >= 0) {
        uint64_t add = (uint64_t)delta;
        if (*cur_lcn > UINT64_MAX - add) return -1;
        *cur_lcn += add;
        return 0;
    }

    /* Absolute value without signed overflow on INT64_MIN. */
    uint64_t sub = (uint64_t)(-(delta + 1)) + 1;
    if (*cur_lcn < sub) return -1;
    *cur_lcn -= sub;
    return 0;
}

int ntfs_decode_data_runs(const uint8_t *run_ptr, uint32_t max_len,
                           struct ntfs_data_run *runs, uint32_t max_runs) {
    if (!run_ptr || !runs || max_runs == 0) return -1;

    uint32_t count = 0;
    uint32_t pos = 0;
    uint64_t prev_lcn = 0;  /* absolute LCN accumulator */
    int malformed = 0;

    while (pos < max_len && count < max_runs) {
        uint8_t header = run_ptr[pos];
        if (header == 0) break;  /* end of runs */

        uint8_t len_size = header & 0x0F;
        uint8_t off_size = (header >> 4) & 0x0F;
        pos++;

        if (len_size == 0 || len_size > 8) { malformed = 1; break; }
        if (off_size > 8) { malformed = 1; break; }
        if (pos + len_size + off_size > max_len) { malformed = 1; break; }

        /* Decode run length (unsigned) */
        uint64_t run_length = 0;
        for (uint8_t i = 0; i < len_size; i++) {
            run_length |= (uint64_t)run_ptr[pos + i] << (i * 8);
        }
        pos += len_size;
        if (run_length == 0) { malformed = 1; break; }

        /* Decode LCN offset (signed, delta from previous) */
        if (off_size > 0) {
            uint64_t raw_delta = 0;
            for (uint8_t i = 0; i < off_size; i++) {
                raw_delta |= (uint64_t)run_ptr[pos + i] << (i * 8);
            }
            /* Sign extend */
            if (run_ptr[pos + off_size - 1] & 0x80) {
                if (off_size < 8) {
                    raw_delta |= (~0ULL) << (off_size * 8);
                }
            }
            int64_t lcn_delta = (int64_t)raw_delta;
            pos += off_size;
            if (ntfs_lcn_apply_delta(&prev_lcn, lcn_delta) != 0) {
                malformed = 1;
                break;
            }
        } else {
            /* Sparse run (off_size == 0): LCN is meaningless, skip */
            runs[count].lcn = NTFS_LCN_SPARSE;
            runs[count].length = run_length;
            count++;
            continue;
        }

        runs[count].lcn = prev_lcn;
        runs[count].length = run_length;
        count++;
    }

    /* Prevent silent truncation when caller-provided run buffer is too small. */
    if (count == max_runs && pos < max_len && run_ptr[pos] != 0) return -1;
    if (malformed) return -1;

    return (int)count;
}

/* ── Data Reading ───────────────────────────────────────────────────────── */

/* Read data from a resident attribute */
static int ntfs_read_resident(struct ntfs_attr_header *attr, void *mft_record,
                               void *buf, uint64_t offset, uint64_t len) {
    struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)attr;
    if (!attr) return -1;
    if (attr->length < sizeof(struct ntfs_attr_resident)) return -1;
    if (res->value_offset < sizeof(struct ntfs_attr_resident)) return -1;
    if (res->value_offset > attr->length) return -1;
    if (res->value_offset + res->value_length > attr->length) return -1;
    uint8_t *data = (uint8_t *)attr + res->value_offset;
    uint32_t data_len = res->value_length;
    (void)mft_record;

    if (offset >= data_len) return 0;
    if (offset + len > data_len) len = data_len - offset;
    ntfs_memcpy(buf, data + offset, (uint32_t)len);
    return (int)len;
}

/* Read data from a non-resident attribute using data runs */
static int ntfs_read_nonresident(struct ntfs_fs *fs, struct ntfs_attr_header *attr,
                                  void *buf, uint64_t offset, uint64_t len) {
    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)attr;
    if (!attr || attr->length < sizeof(struct ntfs_attr_nonresident)) return -1;
    uint64_t real_size = nr->real_size;
    if (nr->data_run_offset >= attr->length) return -1;

    ntfs_trace_emit("nonres:begin", offset, len, real_size);

    if (offset >= real_size) return 0;
    if (offset + len > real_size) len = real_size - offset;
    if (len == 0) return 0;

    /* Decode data runs */
    const uint8_t *run_ptr = (const uint8_t *)attr + nr->data_run_offset;
    uint32_t run_space = attr->length - nr->data_run_offset;
    struct ntfs_data_run *runs = (struct ntfs_data_run *)kmalloc(NTFS_MAX_RUNS * sizeof(struct ntfs_data_run));
    if (!runs) return -1;

    int run_count = ntfs_decode_data_runs(run_ptr, run_space, runs, NTFS_MAX_RUNS);
    if (run_count <= 0) {
        kfree(runs);
        return -1;
    }
    ntfs_trace_emit("nonres:runs", (uint64_t)run_count, run_space, nr->data_run_offset);

    /* Read requested data by walking data runs */
    uint8_t *out = (uint8_t *)buf;
    uint64_t total_read = 0;
    uint64_t cluster_offset = 0;  /* byte offset into logical file from start of runs */

    /* Allocate one cluster buffer */
    uint8_t *clus_buf = (uint8_t *)kmalloc(fs->cluster_size);
    if (!clus_buf) {
        kfree(runs);
        return -1;
    }

    for (int r = 0; r < run_count && total_read < len; r++) {
        if (runs[r].length > UINT64_MAX / fs->cluster_size) {
            kfree(clus_buf);
            kfree(runs);
            return -1;
        }
        uint64_t run_bytes = runs[r].length * fs->cluster_size;
        if (cluster_offset > UINT64_MAX - run_bytes) {
            kfree(clus_buf);
            kfree(runs);
            return -1;
        }
        ntfs_trace_emit("nonres:run", runs[r].lcn, runs[r].length, run_bytes);

        /* Check if this run overlaps with our requested range */
        if (cluster_offset + run_bytes <= offset) {
            cluster_offset += run_bytes;
            continue;
        }

        /* Sparse run (LCN=0): fill with zeros */
        if (runs[r].lcn == NTFS_LCN_SPARSE && runs[r].length > 0) {
            uint64_t skip = 0;
            if (offset > cluster_offset) skip = offset - cluster_offset;
            uint64_t avail = run_bytes - skip;
            if (avail > len - total_read) avail = len - total_read;
            ntfs_memset(out + total_read, 0, (uint32_t)avail);
            total_read += avail;
            cluster_offset += run_bytes;
            continue;
        }

        /* Read clusters in this run */
        for (uint64_t c = 0; c < runs[r].length && total_read < len; c++) {
            uint64_t clus_byte_off = cluster_offset + c * fs->cluster_size;
            uint64_t clus_byte_end = clus_byte_off + fs->cluster_size;

            if (clus_byte_end <= offset) continue;

            if (runs[r].lcn > UINT64_MAX - c) {
                kfree(clus_buf);
                kfree(runs);
                return (total_read > 0) ? (int)total_read : -1;
            }
            if (ntfs_read_cluster(fs, runs[r].lcn + c, clus_buf) != 0) {
                kfree(clus_buf);
                kfree(runs);
                return (total_read > 0) ? (int)total_read : -1;
            }

            uint32_t start_in_clus = 0;
            if (offset > clus_byte_off)
                start_in_clus = (uint32_t)(offset - clus_byte_off);

            uint32_t avail = fs->cluster_size - start_in_clus;
            if ((uint64_t)avail > len - total_read)
                avail = (uint32_t)(len - total_read);

            ntfs_memcpy(out + total_read, clus_buf + start_in_clus, avail);
            total_read += avail;
        }

        cluster_offset += run_bytes;
    }

    kfree(clus_buf);
    kfree(runs);
    return (int)total_read;
}

int ntfs_read_data(struct ntfs_fs *fs, struct ntfs_attr_header *attr,
                   void *mft_record, void *buf, uint64_t offset, uint64_t len) {
    if (!attr) return -1;
    if (attr->non_resident) {
        return ntfs_read_nonresident(fs, attr, buf, offset, len);
    } else {
        return ntfs_read_resident(attr, mft_record, buf, offset, len);
    }
}

/* ── Directory Lookup ───────────────────────────────────────────────────── */

/* Enumerate index entries from a buffer, calling cb for each.
 * If 'search_name' is non-NULL, returns the MFT ref of the match and stops. */
struct ntfs_dir_ctx {
    struct ntfs_fs *fs;
    const char *search_name;
    uint32_t search_len;
    uint64_t found_mft;
    int found;
    int enum_stop;
    int (*enum_cb)(const char *name, uint64_t size, uint32_t attr, void *ctx);
    void *enum_ctx;
};

static int ntfs_get_file_info(struct ntfs_fs *fs, uint64_t mft_index,
                               uint64_t *out_size, int *out_is_dir);

static int ntfs_walk_index_entries(uint8_t *entries_base, uint32_t entries_len,
                                    struct ntfs_dir_ctx *ctx) {
    uint32_t off = 0;
    char name_buf[256];

    while (off + sizeof(struct ntfs_index_entry) <= entries_len) {
        struct ntfs_index_entry *ie = (struct ntfs_index_entry *)(entries_base + off);

        if (ie->entry_length == 0) break;
        if (ie->entry_length < sizeof(struct ntfs_index_entry)) break;
        if (off + ie->entry_length > entries_len) break;

        /* Check if this is the end marker */
        if (ie->flags & NTFS_INDEX_ENTRY_END) break;

        /* Entry has content (filename attr copy) at offset 0x10 */
        if (ie->content_length >= sizeof(struct ntfs_filename_attr)) {
            if ((uint32_t)ie->content_length + 0x10 > ie->entry_length) {
                off += ie->entry_length;
                continue;
            }
            struct ntfs_filename_attr *fn =
                (struct ntfs_filename_attr *)((uint8_t *)ie + 0x10);

            /* Skip DOS-only names (prefer Win32 or Win32+DOS) */
            if (fn->name_type != NTFS_NAMESPACE_DOS) {
                uint16_t *uname = (uint16_t *)((uint8_t *)fn + 0x42);
                uint32_t ulen = fn->name_length;
                uint32_t name_bytes = ulen * 2u;
                if (0x42 + name_bytes > ie->content_length) {
                    off += ie->entry_length;
                    continue;
                }
                uint64_t mft_ref = ie->mft_ref & 0x0000FFFFFFFFFFFFULL;

                if (ctx->search_name) {
                    /* Looking for a specific name */
                    if (ntfs_name_match(ctx->search_name, ctx->search_len,
                                         uname, ulen)) {
                        ctx->found_mft = mft_ref;
                        ctx->found = 1;
                        return 1;
                    }
                } else if (ctx->enum_cb) {
                    /* Enumerating all entries */
                    ntfs_utf16_to_ascii(uname, ulen, name_buf, sizeof(name_buf));
                    uint32_t attr = (fn->flags & 0x10) ? 0x10 : 0x20;
                    uint64_t size = fn->real_size;

                    /* If size is 0 or it's a directory, try to get authoritative info. */
                    if (ctx->fs && (size == 0 || (attr & 0x10))) {
                        uint64_t real_sz = 0;
                        int is_dir_real = 0;
                        if (ntfs_get_file_info(ctx->fs, mft_ref, &real_sz, &is_dir_real) == 0) {
                            size = real_sz;
                            attr = is_dir_real ? 0x10 : 0x20;
                        }
                    }

                    if (ctx->enum_cb(name_buf, size, attr, ctx->enum_ctx)) {
                        ctx->enum_stop = 1;
                        return 1;
                    }
                }
            }
        }

        off += ie->entry_length;
    }
    return 0;
}

/* Lookup a name in a directory's $INDEX_ROOT attribute */
static int ntfs_lookup_index_root(void *mft_record, uint32_t record_size,
                                   struct ntfs_dir_ctx *ctx) {
    struct ntfs_attr_header *ir_attr = ntfs_find_attr(mft_record, record_size,
                                                       NTFS_ATTR_INDEX_ROOT);
    if (!ir_attr || ir_attr->non_resident) return -1;

    struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)ir_attr;
    uint8_t *ir_data = (uint8_t *)ir_attr + res->value_offset;
    uint32_t ir_len = res->value_length;

    if (ir_len < sizeof(struct ntfs_index_root) + sizeof(struct ntfs_index_node_header))
        return -1;

    struct ntfs_index_node_header *node =
        (struct ntfs_index_node_header *)(ir_data + sizeof(struct ntfs_index_root));
    uint32_t node_space = ir_len - sizeof(struct ntfs_index_root);
    if (node->entries_offset > node_space) return -1;
    if (node->index_length < node->entries_offset ||
        node->index_length > node_space) return -1;

    uint8_t *entries_base = (uint8_t *)node + node->entries_offset;
    uint32_t entries_len = node->index_length - node->entries_offset;

    ntfs_walk_index_entries(entries_base, entries_len, ctx);
    if (ctx->enum_stop) return 0;
    return ctx->found ? 0 : -1;
}

/* Lookup a name in $INDEX_ALLOCATION (large directories) */
static int ntfs_lookup_index_alloc(struct ntfs_fs *fs, void *mft_record,
                                    uint32_t record_size,
                                    struct ntfs_dir_ctx *ctx) {
    struct ntfs_attr_header *ia_attr = ntfs_find_attr(mft_record, record_size,
                                                       NTFS_ATTR_INDEX_ALLOC);
    if (!ia_attr) return -1;  /* No index allocation — small dir, only root */
    if (!ia_attr->non_resident) return -1;

    /* Read entire $INDEX_ALLOCATION via data runs */
    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)ia_attr;
    uint64_t alloc_size = nr->alloc_size;

    /* Sanity check: don't read more than 4MB of index data */
    if (alloc_size > 4 * 1024 * 1024) alloc_size = 4 * 1024 * 1024;

    uint8_t *ia_buf = (uint8_t *)kmalloc(alloc_size);
    if (!ia_buf) return -1;

    int rd = ntfs_read_nonresident(fs, ia_attr, ia_buf, 0, alloc_size);
    if (rd <= 0) {
        kfree(ia_buf);
        return -1;
    }

    /* Get index block size from $INDEX_ROOT */
    struct ntfs_attr_header *ir_attr = ntfs_find_attr(mft_record, record_size,
                                                       NTFS_ATTR_INDEX_ROOT);
    uint32_t index_block_size = 4096;  /* default */
    if (ir_attr && !ir_attr->non_resident) {
        struct ntfs_attr_resident *ir_res = (struct ntfs_attr_resident *)ir_attr;
        uint8_t *ir_data = (uint8_t *)ir_attr + ir_res->value_offset;
        struct ntfs_index_root *ir = (struct ntfs_index_root *)ir_data;
        if (ir->index_block_size >= 512 && ir->index_block_size <= 65536)
            index_block_size = ir->index_block_size;
    }

    /* Walk each INDX block */
    uint64_t pos = 0;
    while (pos + index_block_size <= (uint64_t)rd) {
        struct ntfs_index_block *ib = (struct ntfs_index_block *)(ia_buf + pos);

        /* Check INDX magic = 0x58444E49 */
        if (ib->magic == 0x58444E49) {
            /* Apply fixup */
            uint32_t sectors_in_block = index_block_size / fs->bytes_per_sector;
            if (ib->fixup_count >= sectors_in_block + 1) {
                if (ntfs_apply_fixup(ib, index_block_size,
                                      ib->fixup_offset, ib->fixup_count,
                                      fs->bytes_per_sector) != 0) {
                    pos += index_block_size;
                    continue;
                }
            }

            /* Node header starts at offset 0x18 in the INDX block */
            struct ntfs_index_node_header *node =
                (struct ntfs_index_node_header *)((uint8_t *)ib + 0x18);

            if (node->entries_offset > node->index_length) {
                pos += index_block_size;
                continue;
            }
            if (node->entries_offset > index_block_size - 0x18) {
                pos += index_block_size;
                continue;
            }
            uint8_t *entries_base = (uint8_t *)node + node->entries_offset;
            uint32_t entries_len = node->index_length - node->entries_offset;
            uint32_t max_entries = index_block_size - 0x18 - node->entries_offset;
            if (entries_len > max_entries) entries_len = max_entries;

            ntfs_walk_index_entries(entries_base, entries_len, ctx);
            if (ctx->enum_stop) {
                kfree(ia_buf);
                return 0;
            }
            if (ctx->found) {
                kfree(ia_buf);
                return 0;
            }
        }

        pos += index_block_size;
    }

    kfree(ia_buf);
    return ctx->found ? 0 : -1;
}

/* Lookup a filename in a directory MFT entry */
static int ntfs_dir_lookup(struct ntfs_fs *fs, uint64_t dir_mft_index,
                            const char *name, uint32_t namelen,
                            uint64_t *out_mft_index) {
    if (!fs || !name || !out_mft_index) return -1;

    uint8_t *mft_buf = (uint8_t *)kmalloc(fs->mft_record_size);
    if (!mft_buf) return -1;

    if (ntfs_read_mft_entry(fs, dir_mft_index, mft_buf) != 0) {
        kfree(mft_buf);
        return -1;
    }

    struct ntfs_dir_ctx ctx;
    ntfs_memset(&ctx, 0, sizeof(ctx));
    ctx.fs = fs;
    ctx.search_name = name;
    ctx.search_len = namelen;

    /* Try $INDEX_ROOT first */
    if (ntfs_lookup_index_root(mft_buf, fs->mft_record_size, &ctx) == 0) {
        *out_mft_index = ctx.found_mft;
        kfree(mft_buf);
        return 0;
    }

    /* Try $INDEX_ALLOCATION for large directories */
    ctx.found = 0;
    if (ntfs_lookup_index_alloc(fs, mft_buf, fs->mft_record_size, &ctx) == 0) {
        *out_mft_index = ctx.found_mft;
        kfree(mft_buf);
        return 0;
    }

    kfree(mft_buf);
    return -1;
}

/* ── Path Resolution ────────────────────────────────────────────────────── */

int ntfs_resolve_path(struct ntfs_fs *fs, const char *path,
                       uint64_t *out_mft_index) {
    /* Skip leading slashes */
    while (*path == '/') path++;
    if (!*path) {
        *out_mft_index = NTFS_FILE_ROOT;
        return 0;
    }

    uint64_t cur_mft = NTFS_FILE_ROOT;

    while (*path) {
        /* Extract path component */
        const char *comp = path;
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;

        uint64_t child_mft;
        if (ntfs_dir_lookup(fs, cur_mft, comp, clen, &child_mft) != 0)
            return -1;

        cur_mft = child_mft;
        path += clen;
        while (*path == '/') path++;
    }

    *out_mft_index = cur_mft;
    return 0;
}

/* ── Directory Enumeration ──────────────────────────────────────────────── */

static int ntfs_readdir_impl(struct ntfs_fs *fs, uint64_t dir_mft_index,
                              int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                              void *ctx) {
    uint8_t *mft_buf = (uint8_t *)kmalloc(fs->mft_record_size);
    if (!mft_buf) return -1;

    ntfs_trace_emit("readdir:mft", dir_mft_index, fs->mft_record_size, 0);
    if (ntfs_read_mft_entry(fs, dir_mft_index, mft_buf) != 0) {
        ntfs_trace_emit("readdir:mft_err", dir_mft_index, 0, 0);
        kfree(mft_buf);
        return -1;
    }

    struct ntfs_dir_ctx dctx;
    ntfs_memset(&dctx, 0, sizeof(dctx));
    dctx.fs = fs;
    dctx.search_name = 0;
    dctx.enum_cb = cb;
    dctx.enum_ctx = ctx;

    /* Enumerate from $INDEX_ROOT */
    ntfs_trace_emit("readdir:index_root", dir_mft_index, 0, 0);
    ntfs_lookup_index_root(mft_buf, fs->mft_record_size, &dctx);
    if (dctx.enum_stop) {
        kfree(mft_buf);
        return 0;
    }

    /* Also enumerate from $INDEX_ALLOCATION if present */
    ntfs_trace_emit("readdir:index_alloc", dir_mft_index, 0, 0);
    ntfs_lookup_index_alloc(fs, mft_buf, fs->mft_record_size, &dctx);

    kfree(mft_buf);
    return 0;
}

/* ── Get file info from MFT entry ───────────────────────────────────────── */

static int ntfs_get_filename_real_size(void *mft_record, uint32_t record_size,
                                       uint64_t *out_size) {
    struct ntfs_mft_entry *mft = (struct ntfs_mft_entry *)mft_record;
    uint8_t *base = (uint8_t *)mft_record;
    uint32_t off = mft->attrs_offset;
    uint64_t best = 0;
    int found = 0;

    while (off + sizeof(struct ntfs_attr_header) <= record_size) {
        struct ntfs_attr_header *ah = (struct ntfs_attr_header *)(base + off);
        if (ah->type == NTFS_ATTR_END || ah->type == 0) break;
        if (ah->length == 0 || ah->length > record_size - off) break;

        if (ah->type == NTFS_ATTR_FILE_NAME && !ah->non_resident &&
            ah->length >= sizeof(struct ntfs_attr_resident)) {
            struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)ah;
            if (res->value_offset < ah->length &&
                res->value_length >= sizeof(struct ntfs_filename_attr) &&
                res->value_offset + sizeof(struct ntfs_filename_attr) <= ah->length) {
                struct ntfs_filename_attr *fn =
                    (struct ntfs_filename_attr *)(base + off + res->value_offset);
                if (!found || fn->real_size > best) best = fn->real_size;
                found = 1;
            }
        }

        off += ah->length;
    }

    if (!found) return -1;
    if (out_size) *out_size = best;
    return 0;
}

static int ntfs_get_file_info(struct ntfs_fs *fs, uint64_t mft_index,
                               uint64_t *out_size, int *out_is_dir) {
    if (!fs) return -1;

    uint8_t *mft_buf = (uint8_t *)kmalloc(fs->mft_record_size);
    if (!mft_buf) return -1;

    if (ntfs_read_mft_entry(fs, mft_index, mft_buf) != 0) {
        kfree(mft_buf);
        return -1;
    }

    struct ntfs_mft_entry *mft = (struct ntfs_mft_entry *)mft_buf;
    int is_dir = (mft->flags & NTFS_MFT_FLAG_DIRECTORY) ? 1 : 0;

    if (out_is_dir)
        *out_is_dir = is_dir;

    if (out_size) {
        uint64_t size = 0;
        int size_from_primary = 0;

        /* Get size from $DATA attribute (best effort, do not fail on malformed). */
        struct ntfs_attr_header *data_attr =
            ntfs_find_data_attr(mft_buf, fs->mft_record_size);

        /* Directories may legally omit $DATA; treat as size=0 without fallback. */
        if (!is_dir && data_attr) {
            if (data_attr->non_resident) {
                if (data_attr->length >= sizeof(struct ntfs_attr_nonresident)) {
                    struct ntfs_attr_nonresident *nr =
                        (struct ntfs_attr_nonresident *)data_attr;
                    size = nr->real_size;
                    if (nr->start_vcn == 0) size_from_primary = 1;
                }
            } else {
                if (data_attr->length >= sizeof(struct ntfs_attr_resident)) {
                    struct ntfs_attr_resident *res =
                        (struct ntfs_attr_resident *)data_attr;
                    if (res->value_offset >= sizeof(struct ntfs_attr_resident) &&
                        res->value_offset + res->value_length <= data_attr->length) {
                        size = res->value_length;
                        size_from_primary = 1;
                    }
                }
            }
        }

        /* Fallback for fragmented attributes where this record only carries
         * a continuation extent (start_vcn != 0), and as a general safety
         * fallback when parsed size is zero. */
        if (!is_dir && (!size_from_primary || size == 0)) {
            uint64_t fn_size = 0;
            if (ntfs_get_filename_real_size(mft_buf, fs->mft_record_size, &fn_size) == 0) {
                size = fn_size;
            }
        }

        *out_size = size;
    }

    kfree(mft_buf);
    return 0;
}

/* ── VFS Wrappers ───────────────────────────────────────────────────────── */

int ntfs_open_vfs(void *fs_data, const char *path, struct vfs_file *out) {
    struct ntfs_fs *fs = (struct ntfs_fs *)fs_data;
    struct ntfs_file_priv *priv = (struct ntfs_file_priv *)out->private;
    uint64_t mft_index;

    if (ntfs_resolve_path(fs, path, &mft_index) != 0)
        return -1;

    uint64_t size = 0;
    int is_dir = 0;
    if (ntfs_get_file_info(fs, mft_index, &size, &is_dir) != 0)
        return -1;

    priv->fs = fs;
    priv->mft_index = mft_index;
    priv->size = size;
    priv->pos = 0;
    priv->is_dir = (uint16_t)is_dir;
    priv->is_resident = 0;
    priv->run_count = 0;
    priv->resident_size = 0;
    priv->resident_off = 0;

    /* Pre-cache data runs for non-directory files */
    if (!is_dir) {
        uint8_t *mft_buf = (uint8_t *)kmalloc(fs->mft_record_size);
        if (mft_buf) {
            if (ntfs_read_mft_entry(fs, mft_index, mft_buf) == 0) {
                struct ntfs_attr_header *data_attr =
                    ntfs_find_data_attr(mft_buf, fs->mft_record_size);
                if (data_attr) {
                    if (data_attr->non_resident) {
                        if (data_attr->length < sizeof(struct ntfs_attr_nonresident)) {
                            goto precache_done;
                        }
                        struct ntfs_attr_nonresident *nr =
                            (struct ntfs_attr_nonresident *)data_attr;
                        if (nr->data_run_offset >= data_attr->length) {
                            goto precache_done;
                        }
                        const uint8_t *run_ptr =
                            (const uint8_t *)data_attr + nr->data_run_offset;
                        uint32_t run_space = data_attr->length - nr->data_run_offset;
                        int rc = ntfs_decode_data_runs(run_ptr, run_space,
                                                        priv->runs, 5);
                        if (rc > 0) priv->run_count = (uint32_t)rc;
                    } else {
                        if (data_attr->length < sizeof(struct ntfs_attr_resident)) {
                            goto precache_done;
                        }
                        struct ntfs_attr_resident *res =
                            (struct ntfs_attr_resident *)data_attr;
                        if (res->value_offset < sizeof(struct ntfs_attr_resident) ||
                            res->value_offset + res->value_length > data_attr->length) {
                            goto precache_done;
                        }
                        priv->is_resident = 1;
                        priv->resident_size = res->value_length;
                        /* Store offset from MFT record start to resident data */
                        priv->resident_off = (uint32_t)((uint8_t *)data_attr -
                                              mft_buf) + res->value_offset;
                    }
                }
            }
precache_done:
            kfree(mft_buf);
        }
    }

    out->size = size;
    return 0;
}

int ntfs_read_vfs(struct vfs_file *f, void *buf, uint32_t len) {
    struct ntfs_file_priv *priv = (struct ntfs_file_priv *)f->private;
    struct ntfs_fs *fs = priv->fs;

    if (priv->is_dir) return -1;
    if (priv->pos >= priv->size) return 0;
    if (priv->pos + len > priv->size)
        len = (uint32_t)(priv->size - priv->pos);
    if (len == 0) return 0;

    /* Read the MFT entry and find $DATA attribute for fresh reading */
    uint8_t *mft_buf = (uint8_t *)kmalloc(fs->mft_record_size);
    if (!mft_buf) return -1;

    if (ntfs_read_mft_entry(fs, priv->mft_index, mft_buf) != 0) {
        kfree(mft_buf);
        return -1;
    }

    struct ntfs_attr_header *data_attr =
        ntfs_find_data_attr(mft_buf, fs->mft_record_size);
    if (!data_attr) {
        kfree(mft_buf);
        return -1;
    }

    int n = ntfs_read_data(fs, data_attr, mft_buf, buf, priv->pos, len);
    kfree(mft_buf);

    if (n > 0) priv->pos += (uint64_t)n;
    return n;
}

int ntfs_close_vfs(struct vfs_file *f) {
    (void)f;
    return 0;
}

int ntfs_readdir_vfs(void *fs_data, const char *path,
                     int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                     void *ctx) {
    struct ntfs_fs *fs = (struct ntfs_fs *)fs_data;
    uint64_t mft_index;

    if (!path || !*path) {
        mft_index = NTFS_FILE_ROOT;
    } else {
        if (ntfs_resolve_path(fs, path, &mft_index) != 0)
            return -1;
    }

    /* Verify it's a directory */
    int is_dir = 0;
    int rc = -1;
    ntfs_trace_start("readdir", path, mft_index);
    if (ntfs_get_file_info(fs, mft_index, 0, &is_dir) != 0) {
        ntfs_trace_emit("readdir:stat_err", mft_index, 0, 0);
        ntfs_trace_stop("readdir", -1);
        return -1;
    }
    if (!is_dir) {
        ntfs_trace_emit("readdir:not_dir", mft_index, 0, 0);
        ntfs_trace_stop("readdir", -1);
        return -1;
    }

    rc = ntfs_readdir_impl(fs, mft_index, cb, ctx);
    ntfs_trace_stop("readdir", rc);
    return rc;
}

int ntfs_stat_vfs(void *fs_data, const char *path, struct vfs_stat *out) {
    struct ntfs_fs *fs = (struct ntfs_fs *)fs_data;
    uint64_t mft_index;

    if (!path || !*path) {
        mft_index = NTFS_FILE_ROOT;
    } else {
        if (ntfs_resolve_path(fs, path, &mft_index) != 0)
            return -1;
    }

    uint64_t size = 0;
    int is_dir = 0;
    if (ntfs_get_file_info(fs, mft_index, &size, &is_dir) != 0)
        return -1;

    if (out) {
        out->size = size;
        out->attr = is_dir ? 0x10 : 0x20;
    }
    return 0;
}
