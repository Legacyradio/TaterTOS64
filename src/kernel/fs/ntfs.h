/*
 * NTFS — Read-Only NTFS Filesystem Support for TaterTOS64v3
 *
 * On-disk structures + kernel driver declarations.
 * Supports reading NTFS partitions (e.g. Windows NVMe on Dell Precision 7530).
 */

#ifndef TATER_NTFS_H
#define TATER_NTFS_H

#include <stdint.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define NTFS_MAGIC              0x454C4946  /* "FILE" LE */
#define NTFS_OEM_ID             "NTFS    "  /* 8 bytes in boot sector */

/* Well-known MFT entry indices */
#define NTFS_FILE_MFT           0
#define NTFS_FILE_MFTMIRR       1
#define NTFS_FILE_LOGFILE       2
#define NTFS_FILE_VOLUME        3
#define NTFS_FILE_ATTRDEF       4
#define NTFS_FILE_ROOT          5   /* root directory */
#define NTFS_FILE_BITMAP        6
#define NTFS_FILE_BOOT          7
#define NTFS_FILE_BADCLUS       8
#define NTFS_FILE_SECURE        9
#define NTFS_FILE_UPCASE        10

/* Attribute type codes */
#define NTFS_ATTR_STANDARD_INFO     0x10
#define NTFS_ATTR_ATTRIBUTE_LIST    0x20
#define NTFS_ATTR_FILE_NAME         0x30
#define NTFS_ATTR_OBJECT_ID         0x40
#define NTFS_ATTR_SECURITY_DESC     0x50
#define NTFS_ATTR_VOLUME_NAME       0x60
#define NTFS_ATTR_VOLUME_INFO       0x70
#define NTFS_ATTR_DATA              0x80
#define NTFS_ATTR_INDEX_ROOT        0x90
#define NTFS_ATTR_INDEX_ALLOC       0xA0
#define NTFS_ATTR_BITMAP            0xB0
#define NTFS_ATTR_REPARSE_POINT     0xC0
#define NTFS_ATTR_END               0xFFFFFFFF

/* MFT entry flags */
#define NTFS_MFT_FLAG_IN_USE        0x01
#define NTFS_MFT_FLAG_DIRECTORY     0x02

/* Index entry flags */
#define NTFS_INDEX_ENTRY_NODE       0x01  /* has sub-node VCN */
#define NTFS_INDEX_ENTRY_END        0x02  /* last entry in node */

/* Filename namespace */
#define NTFS_NAMESPACE_POSIX        0
#define NTFS_NAMESPACE_WIN32        1
#define NTFS_NAMESPACE_DOS          2
#define NTFS_NAMESPACE_WIN32_DOS    3

/* Maximum data runs we decode per attribute. */
#define NTFS_MAX_RUNS               512

/* MFT cache sizing (per mounted NTFS volume) */
#define NTFS_MFT_CACHE_SIZE         64

struct ntfs_mft_cache_entry {
    uint64_t index;   /* MFT record number */
    uint32_t stamp;   /* LRU stamp */
    uint8_t *buf;     /* cached, fixup-applied record */
};

/* GPT type GUID for Microsoft Basic Data partition (mixed endian) */
/* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
static const uint8_t NTFS_BASIC_DATA_GUID[16] = {
    0xA2, 0xA0, 0xD0, 0xEB,  /* LE32: EBD0A0A2 */
    0xE5, 0xB9,              /* LE16: B9E5 */
    0x33, 0x44,              /* LE16: 4433 */
    0x87, 0xC0,              /* BE */
    0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7  /* BE */
};

/* ── On-Disk Structures ────────────────────────────────────────────────── */

/* NTFS Boot Sector (sector 0 of the partition) */
struct ntfs_boot_sector {
    uint8_t  jump[3];               /* 0x00: EB 52 90 */
    uint8_t  oem_id[8];             /* 0x03: "NTFS    " */
    uint16_t bytes_per_sector;      /* 0x0B */
    uint8_t  sectors_per_cluster;   /* 0x0D */
    uint16_t reserved_sectors;      /* 0x0E: unused in NTFS */
    uint8_t  unused1[3];            /* 0x10 */
    uint16_t unused2;               /* 0x13 */
    uint8_t  media_descriptor;      /* 0x15 */
    uint16_t unused3;               /* 0x16 */
    uint16_t sectors_per_track;     /* 0x18 */
    uint16_t num_heads;             /* 0x1A */
    uint32_t hidden_sectors;        /* 0x1C */
    uint32_t unused4;               /* 0x20 */
    uint32_t unused5;               /* 0x24: 0x00800080 */
    uint64_t total_sectors;         /* 0x28 */
    uint64_t mft_lcn;              /* 0x30: MFT start cluster */
    uint64_t mftmirr_lcn;         /* 0x38: MFT mirror start cluster */
    int8_t   clusters_per_mft_record; /* 0x40: if negative, 2^|val| bytes */
    uint8_t  unused6[3];            /* 0x41 */
    int8_t   clusters_per_index_block; /* 0x44 */
    uint8_t  unused7[3];            /* 0x45 */
    uint64_t serial_number;         /* 0x48 */
    uint32_t checksum;              /* 0x50 */
    /* Bootstrap code follows to 0x1FE, then 0x55AA */
} __attribute__((packed));

/* MFT Record (FILE record) header */
struct ntfs_mft_entry {
    uint32_t magic;                 /* 0x00: "FILE" = 0x454C4946 */
    uint16_t fixup_offset;          /* 0x04: offset to update sequence */
    uint16_t fixup_count;           /* 0x06: number of fixup entries */
    uint64_t lsn;                   /* 0x08: $LogFile sequence number */
    uint16_t sequence;              /* 0x10: sequence number */
    uint16_t hard_link_count;       /* 0x12 */
    uint16_t attrs_offset;          /* 0x14: offset to first attribute */
    uint16_t flags;                 /* 0x16: 0x01=in-use, 0x02=directory */
    uint32_t used_size;             /* 0x18: real size of record */
    uint32_t alloc_size;            /* 0x1C: allocated size of record */
    uint64_t base_record;           /* 0x20: base MFT record ref (0=self) */
    uint16_t next_attr_id;          /* 0x28 */
    uint16_t padding;               /* 0x2A: align to 4 bytes (XP+) */
    uint32_t mft_record_number;     /* 0x2C: self MFT record # (XP+) */
} __attribute__((packed));

/* Attribute header (common to resident and non-resident) */
struct ntfs_attr_header {
    uint32_t type;                  /* 0x00: attribute type */
    uint32_t length;                /* 0x04: total attribute length */
    uint8_t  non_resident;          /* 0x08: 0=resident, 1=non-resident */
    uint8_t  name_length;           /* 0x09: name length in UTF-16 chars */
    uint16_t name_offset;           /* 0x0A: offset to name */
    uint16_t flags;                 /* 0x0C: compressed, encrypted, sparse */
    uint16_t attr_id;               /* 0x0E: unique attribute ID */
} __attribute__((packed));

/* Resident attribute (follows ntfs_attr_header) */
struct ntfs_attr_resident {
    struct ntfs_attr_header hdr;
    uint32_t value_length;          /* 0x10: data size */
    uint16_t value_offset;          /* 0x14: offset from attr start to data */
    uint16_t indexed_flag;          /* 0x16 */
} __attribute__((packed));

/* Non-resident attribute (follows ntfs_attr_header) */
struct ntfs_attr_nonresident {
    struct ntfs_attr_header hdr;
    uint64_t start_vcn;             /* 0x10 */
    uint64_t end_vcn;               /* 0x18 */
    uint16_t data_run_offset;       /* 0x20: offset from attr start */
    uint16_t compression_unit;      /* 0x22 */
    uint32_t padding;               /* 0x24 */
    uint64_t alloc_size;            /* 0x28 */
    uint64_t real_size;             /* 0x30 */
    uint64_t init_size;             /* 0x38 */
} __attribute__((packed));

/* $FILE_NAME attribute body */
struct ntfs_filename_attr {
    uint64_t parent_ref;            /* 0x00: parent MFT ref (48-bit index + 16-bit seq) */
    uint64_t creation_time;         /* 0x08 */
    uint64_t modification_time;     /* 0x10 */
    uint64_t mft_modification_time; /* 0x18 */
    uint64_t read_time;             /* 0x20 */
    uint64_t alloc_size;            /* 0x28 */
    uint64_t real_size;             /* 0x30 */
    uint32_t flags;                 /* 0x38 */
    uint32_t reparse_value;         /* 0x3C */
    uint8_t  name_length;           /* 0x40: in UTF-16 chars */
    uint8_t  name_type;             /* 0x41: namespace */
    /* UTF-16LE name[name_length] follows at 0x42 */
} __attribute__((packed));

/* $INDEX_ROOT attribute body header */
struct ntfs_index_root {
    uint32_t attr_type;             /* 0x00: indexed attribute type (0x30 for dirs) */
    uint32_t collation_rule;        /* 0x04 */
    uint32_t index_block_size;      /* 0x08: bytes per index allocation block */
    uint8_t  clusters_per_index;    /* 0x0C */
    uint8_t  padding[3];            /* 0x0D */
    /* Index node header follows at 0x10 */
} __attribute__((packed));

/* Index node header (used in both $INDEX_ROOT and $INDEX_ALLOCATION) */
struct ntfs_index_node_header {
    uint32_t entries_offset;        /* 0x00: offset from this header to first entry */
    uint32_t index_length;          /* 0x04: total used size (from this header) */
    uint32_t alloc_length;          /* 0x08: total allocated size */
    uint32_t flags;                 /* 0x0C: 0x01 = has children (large index) */
} __attribute__((packed));

/* Index entry (in directory index) */
struct ntfs_index_entry {
    uint64_t mft_ref;               /* 0x00: MFT reference of this entry's file */
    uint16_t entry_length;          /* 0x08: total length of this entry */
    uint16_t content_length;        /* 0x0A: length of content (filename attr) */
    uint32_t flags;                 /* 0x0C: NTFS_INDEX_ENTRY_NODE | _END */
    /* Content (ntfs_filename_attr copy) follows at 0x10 if content_length > 0 */
    /* If NTFS_INDEX_ENTRY_NODE: VCN of child node at (entry + entry_length - 8) */
} __attribute__((packed));

/* $INDEX_ALLOCATION block header */
struct ntfs_index_block {
    uint32_t magic;                 /* 0x00: "INDX" = 0x58444E49 */
    uint16_t fixup_offset;          /* 0x04 */
    uint16_t fixup_count;           /* 0x06 */
    uint64_t lsn;                   /* 0x08 */
    uint64_t vcn;                   /* 0x10: VCN of this index block */
    /* ntfs_index_node_header follows at 0x18 */
} __attribute__((packed));

/* ── Data run decoded entry ────────────────────────────────────────────── */

struct ntfs_data_run {
    uint64_t lcn;           /* absolute cluster number */
    uint64_t length;        /* length in clusters */
};

/* ── Kernel-side Structures ─────────────────────────────────────────────── */

struct block_device;    /* forward from part.h */
struct vfs_file;        /* forward from vfs.h */
struct vfs_stat;        /* forward from vfs.h */

/* Mount state for one NTFS volume */
struct ntfs_fs {
    struct block_device *bd;
    uint64_t part_lba;              /* LBA of partition start on disk */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;          /* bytes_per_sector * sectors_per_cluster */
    uint32_t mft_record_size;       /* typically 1024 */
    uint64_t mft_lcn;              /* MFT start cluster */
    uint32_t sectors_per_mft_record;
    uint64_t mft_data_size;         /* $MFT logical size in bytes */
    uint32_t mft_run_count;         /* decoded $MFT data-run count */
    struct ntfs_data_run mft_runs[NTFS_MAX_RUNS];
    /* LRU cache of recently-read MFT records */
    uint32_t mft_cache_tick;
    struct ntfs_mft_cache_entry mft_cache[NTFS_MFT_CACHE_SIZE];
};

/* Per-open-file private state — must fit in vfs_file.private[128] */
struct ntfs_file_private {
    struct ntfs_fs *fs;             /* 8 */
    uint64_t mft_index;            /* 8 — MFT record index */
    uint64_t size;                  /* 8 — file real size */
    uint64_t pos;                   /* 8 — current read position */
    uint16_t is_dir;                /* 2 */
    uint16_t _pad[3];              /* 6 */
    /* Cached data runs for $DATA attribute */
    uint32_t run_count;             /* 4 */
    uint32_t _pad2;                /* 4 */
    struct ntfs_data_run runs[8];   /* 128 — 8 runs × 16 bytes each */
    /* Resident data cache (small files) */
    uint32_t resident_size;         /* 4 */
    uint32_t resident_offset;       /* 4 — offset within MFT record */
};  /* total: 8+8+8+8+2+6+4+4+128+4+4 = 184 — too big for 128, will trim */

/* Trimmed version that actually fits in 128 bytes */
struct ntfs_file_priv {
    struct ntfs_fs *fs;             /* 8 */
    uint64_t mft_index;            /* 8 */
    uint64_t size;                  /* 8 */
    uint64_t pos;                   /* 8 */
    uint16_t is_dir;                /* 2 */
    uint16_t is_resident;           /* 2 */
    uint32_t run_count;             /* 4 */
    struct ntfs_data_run runs[5];   /* 80 — 5 runs × 16 bytes each */
    uint32_t resident_size;         /* 4 */
    uint32_t resident_off;          /* 4 */
};  /* total: 8+8+8+8+2+2+4+80+4+4 = 128 exact */

/* ── Kernel Driver API ──────────────────────────────────────────────────── */

/* Probe / Mount */
int ntfs_probe(struct block_device *bd, uint64_t part_lba);
int ntfs_mount(struct ntfs_fs *fs, struct block_device *bd, uint64_t part_lba);

/* MFT access */
int ntfs_read_mft_entry(struct ntfs_fs *fs, uint64_t index, void *buf);

/* Attribute walking */
struct ntfs_attr_header *ntfs_find_attr(void *mft_record, uint32_t record_size,
                                         uint32_t type);

/* Data run decoding */
int ntfs_decode_data_runs(const uint8_t *run_ptr, uint32_t max_len,
                           struct ntfs_data_run *runs, uint32_t max_runs);

/* Data reading */
int ntfs_read_data(struct ntfs_fs *fs, struct ntfs_attr_header *attr,
                   void *mft_record, void *buf, uint64_t offset, uint64_t len);

/* Path resolution */
int ntfs_resolve_path(struct ntfs_fs *fs, const char *path, uint64_t *out_mft_index);

/* VFS fs_ops wrappers (read-only) */
int ntfs_open_vfs(void *fs_data, const char *path, struct vfs_file *out);
int ntfs_read_vfs(struct vfs_file *f, void *buf, uint32_t len);
int ntfs_close_vfs(struct vfs_file *f);
int ntfs_readdir_vfs(void *fs_data, const char *path,
                     int (*cb)(const char *name, uint64_t size, uint32_t attr, void *ctx),
                     void *ctx);
int ntfs_stat_vfs(void *fs_data, const char *path, struct vfs_stat *out);

#endif /* TATER_NTFS_H */
