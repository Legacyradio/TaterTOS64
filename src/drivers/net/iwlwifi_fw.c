// Intel WiFi firmware loader + TLV parser

#include "iwlwifi_fw.h"
#include "../../kernel/fs/vfs.h"
#include "../../kernel/mm/heap.h"

#define IWLWIFI_FW_MAX_SIZE (8u * 1024u * 1024u)

static void fw_zero(void *ptr, uint32_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < len; i++) p[i] = 0;
}

static uint32_t fw_u32le(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int fw_try_load(const char *path, struct iwlwifi_fw_image *out)
    __attribute__((unused));
static int fw_try_load(const char *path, struct iwlwifi_fw_image *out) {
    struct vfs_file *f = vfs_open(path);
    if (!f) return -1;

    uint32_t size = vfs_size(f);
    if (size == 0 || size > IWLWIFI_FW_MAX_SIZE) {
        vfs_close(f);
        return -2;
    }

    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        vfs_close(f);
        return -3;
    }

    int rd = vfs_read(f, buf, size);
    vfs_close(f);
    if (rd < 0 || (uint32_t)rd != size) {
        kfree(buf);
        return -4;
    }

    fw_zero(out, (uint32_t)sizeof(*out));
    out->path = path;
    out->data = (const uint8_t *)buf;
    out->size = size;
    out->magic_offset = 0xFFFFFFFFu;
    if (size >= 4 && fw_u32le(buf) == IWL_UCODE_TLV_MAGIC) {
        out->magic = IWL_UCODE_TLV_MAGIC;
        out->magic_offset = 0;
    } else if (size >= 8 && fw_u32le(buf + 4) == IWL_UCODE_TLV_MAGIC) {
        out->magic = IWL_UCODE_TLV_MAGIC;
        out->magic_offset = 4;
    }
    return 0;
}

/* Embedded firmware blob (linked from iwlwifi_fw_blob.c) */
extern const unsigned char iwlwifi_9260_fw_blob[];
extern unsigned int  iwlwifi_9260_fw_blob_len;

int iwlwifi_fw_load(struct iwlwifi_fw_image *out) {
    if (!out) return -1;
    fw_zero(out, (uint32_t)sizeof(*out));

    uint32_t size = (uint32_t)iwlwifi_9260_fw_blob_len;
    if (size == 0 || size > IWLWIFI_FW_MAX_SIZE) return -2;

    /* Use embedded blob directly — no heap copy needed */
    out->path = "embedded";
    out->data = iwlwifi_9260_fw_blob;
    out->size = size;
    out->embedded = 1;
    out->magic_offset = 0xFFFFFFFFu;
    if (size >= 4 && fw_u32le(out->data) == IWL_UCODE_TLV_MAGIC) {
        out->magic = IWL_UCODE_TLV_MAGIC;
        out->magic_offset = 0;
    } else if (size >= 8 && fw_u32le(out->data + 4) == IWL_UCODE_TLV_MAGIC) {
        out->magic = IWL_UCODE_TLV_MAGIC;
        out->magic_offset = 4;
    }

    iwlwifi_dbg("IWL9260: firmware loaded from embedded blob size=%u\n", size);
    return 0;
}

void iwlwifi_fw_release(struct iwlwifi_fw_image *fw) {
    if (!fw) return;
    if (fw->data && !fw->embedded) kfree((void *)(uintptr_t)fw->data);
    fw_zero(fw, (uint32_t)sizeof(*fw));
}

/*
 * Intel iwlwifi TLV firmware header layout:
 *   offset 0: uint32_t zero (always 0, or sometimes magic directly)
 *   offset 4: uint32_t magic (IWL_UCODE_TLV_MAGIC = 0x5A4B3C2D)
 *   offset 8: char human_readable[64]
 *   offset 72: uint32_t ver (major.minor.api.serial packed)
 *   offset 76: uint32_t build
 *   offset 80: uint64_t ignore (timestamp)
 *   offset 88: TLV entries start
 *
 * Each TLV entry:
 *   uint32_t type
 *   uint32_t length
 *   uint8_t  data[length]  (padded to 4-byte alignment)
 */

#define IWL_TLV_HDR_SIZE    88u  /* bytes before TLV chain starts */
#define IWL_TLV_ENTRY_HDR   8u   /* type(4) + length(4) */

static void fw_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static char fw_hex_digit(uint8_t v) {
    return "0123456789abcdef"[v & 0xFu];
}

static void fw_hex_bytes(const uint8_t *src, uint32_t len, char *dst, uint32_t dst_len) {
    uint32_t di = 0;
    if (!dst || dst_len == 0) return;

    for (uint32_t i = 0; i < len; i++) {
        if (i != 0) {
            if (di + 1 >= dst_len) break;
            dst[di++] = ' ';
        }
        if (di + 2 >= dst_len) break;
        dst[di++] = fw_hex_digit((uint8_t)(src[i] >> 4));
        dst[di++] = fw_hex_digit(src[i]);
    }

    dst[di] = '\0';
}

static void iwlwifi_fw_log_tlv(uint32_t tlv_count,
                               uint32_t pos,
                               uint32_t tlv_type,
                               uint32_t tlv_len,
                               const uint8_t *tlv_hdr,
                               const uint8_t *tlv_data) {
    char hdr_hex[IWL_TLV_ENTRY_HDR * 3u];
    char payload_hex[16u * 3u];
    uint32_t payload_len = (tlv_len < 16u) ? tlv_len : 16u;

    fw_hex_bytes(tlv_hdr, IWL_TLV_ENTRY_HDR, hdr_hex, (uint32_t)sizeof(hdr_hex));
    if (payload_len != 0) {
        fw_hex_bytes(tlv_data, payload_len, payload_hex, (uint32_t)sizeof(payload_hex));
        iwlwifi_dbg("IWL9260: TLV #%u off=0x%x type=%u len=%u hdr=[%s] pay16=[%s]\n",
                    tlv_count, pos, tlv_type, tlv_len, hdr_hex, payload_hex);
        return;
    }

    iwlwifi_dbg("IWL9260: TLV #%u off=0x%x type=%u len=%u hdr=[%s]\n",
                tlv_count, pos, tlv_type, tlv_len, hdr_hex);
}

static int iwlwifi_fw_store_sec(struct iwl_fw_section *sec,
                                const uint8_t *tlv_data,
                                uint32_t tlv_len,
                                uint32_t tlv_type,
                                uint32_t tlv_count) {
    if (!sec || !tlv_data)
        return -1;

    if (tlv_len < 4u) {
        iwlwifi_dbg("IWL9260: TLV #%u type=%u malformed section len=%u\n",
                    tlv_count, tlv_type, tlv_len);
        return -6;
    }

    sec->offset = fw_u32le(tlv_data);
    sec->data = tlv_data + 4u;
    sec->size = tlv_len - 4u;
    return 0;
}

int iwlwifi_fw_parse_tlv(const struct iwlwifi_fw_image *fw, struct iwl_fw_pieces *out) {
    if (!fw || !out || !fw->data) return -1;
    fw_zero(out, (uint32_t)sizeof(*out));

    const uint8_t *data = fw->data;
    uint32_t size = fw->size;

    /* Validate magic position */
    if (fw->magic_offset != 0 && fw->magic_offset != 4) {
        iwlwifi_dbg("IWL9260: FW no TLV magic found\n");
        return -2;
    }

    /* Need at least the header */
    if (size < IWL_TLV_HDR_SIZE) {
        iwlwifi_dbg("IWL9260: FW too small for TLV header (%u bytes)\n", size);
        return -3;
    }

    /* Validate magic at expected position */
    uint32_t magic = fw_u32le(data + 4);
    if (magic != IWL_UCODE_TLV_MAGIC) {
        /* Try offset 0 */
        magic = fw_u32le(data);
        if (magic != IWL_UCODE_TLV_MAGIC) {
            iwlwifi_dbg("IWL9260: FW bad magic 0x%08x\n", magic);
            return -4;
        }
    }

    /* Parse header fields */
    fw_copy(out->human_readable, data + 8, 64);
    out->human_readable[63] = '\0';

    uint32_t ver = fw_u32le(data + 72);
    out->ver_major  = (uint8_t)((ver >> 24) & 0xFF);
    out->ver_minor  = (uint8_t)((ver >> 16) & 0xFF);
    out->ver_api    = (uint8_t)((ver >> 8) & 0xFF);
    out->ver_serial = (uint8_t)(ver & 0xFF);
    out->build      = fw_u32le(data + 76);
    out->num_cpus   = 1; /* default */

    iwlwifi_dbg("IWL9260: FW \"%s\"\n", out->human_readable);

    /* Walk the TLV chain starting at offset 88 */
    uint32_t pos = IWL_TLV_HDR_SIZE;
    uint32_t tlv_count = 0;
    uint32_t sec_rt_idx = 0;
    uint32_t sec_init_idx = 0;

    while (pos + IWL_TLV_ENTRY_HDR <= size) {
        uint32_t tlv_type = fw_u32le(data + pos);
        uint32_t tlv_len  = fw_u32le(data + pos + 4);
        const uint8_t *tlv_data = data + pos + IWL_TLV_ENTRY_HDR;

        /* Put the raw TLV dump into the WiFi debug buffer so the shell can read it. */
        iwlwifi_fw_log_tlv(tlv_count, pos, tlv_type, tlv_len, data + pos, tlv_data);

        /* Sanity check */
        if (pos + IWL_TLV_ENTRY_HDR + tlv_len > size) {
            iwlwifi_dbg("IWL9260: TLV #%u type=%u len=%u overflows (pos=%u size=%u)\n",
                        tlv_count, tlv_type, tlv_len, pos, size);
            break;
        }

        switch (tlv_type) {
        case IWL_UCODE_TLV_INST:
            out->cpu1_inst_data = tlv_data;
            out->cpu1_inst_size = tlv_len;
            break;

        case IWL_UCODE_TLV_DATA:
            out->cpu1_data_data = tlv_data;
            out->cpu1_data_size = tlv_len;
            break;

        case IWL_UCODE_TLV_INIT:
            out->cpu2_inst_data = tlv_data;
            out->cpu2_inst_size = tlv_len;
            break;

        case IWL_UCODE_TLV_INIT_DATA:
            out->cpu2_data_data = tlv_data;
            out->cpu2_data_size = tlv_len;
            break;

        case IWL_UCODE_TLV_SEC_RT:
        case IWL_UCODE_TLV_SECURE_SEC_RT:
            if (sec_rt_idx < IWL_MAX_SEC_SECTIONS) {
                int rc = iwlwifi_fw_store_sec(&out->sec_rt[sec_rt_idx],
                                              tlv_data, tlv_len,
                                              tlv_type, tlv_count);
                if (rc != 0)
                    return rc;
                sec_rt_idx++;
            }
            break;

        case IWL_UCODE_TLV_SEC_INIT:
        case IWL_UCODE_TLV_SECURE_SEC_INIT:
            if (sec_init_idx < IWL_MAX_SEC_SECTIONS) {
                int rc = iwlwifi_fw_store_sec(&out->sec_init[sec_init_idx],
                                              tlv_data, tlv_len,
                                              tlv_type, tlv_count);
                if (rc != 0)
                    return rc;
                sec_init_idx++;
            }
            break;

        case IWL_UCODE_TLV_PAGING:
            if (out->paging_count < IWL_MAX_PAGING_SECTIONS) {
                out->paging_data[out->paging_count] = tlv_data;
                out->paging_size[out->paging_count] = tlv_len;
                out->paging_count++;
            }
            break;

        case IWL_UCODE_TLV_NUM_OF_CPU:
            if (tlv_len >= 4) {
                out->num_cpus = fw_u32le(tlv_data);
            }
            break;

        case IWL_UCODE_TLV_PHY_SKU:
            if (tlv_len >= 4) {
                out->phy_config = fw_u32le(tlv_data);
            }
            break;

        case IWL_UCODE_TLV_FW_VERSION:
            /* Alternate version info; already got from header */
            break;

        default:
            /* Skip unknown TLVs silently */
            break;
        }

        tlv_count++;
        /* Advance to next TLV (length padded to 4-byte boundary) */
        uint32_t padded_len = (tlv_len + 3u) & ~3u;
        pos += IWL_TLV_ENTRY_HDR + padded_len;
    }

    out->tlv_count = tlv_count;
    out->sec_rt_count = sec_rt_idx;
    out->sec_init_count = sec_init_idx;
    iwlwifi_dbg("IWL9260: TLV parsed=%u rt=%u init=%u paging=%u phy=0x%08x\n",
                tlv_count, out->sec_rt_count, out->sec_init_count,
                out->paging_count, out->phy_config);

    /*
     * Accept either legacy INST/DATA images or secure multi-section runtime
     * images. 9260 runtime blobs can be SEC_RT-only and should not fail TLV
     * parsing just because no legacy INST TLV is present.
     */
    if (!out->cpu1_inst_data || out->cpu1_inst_size == 0) {
        if (out->sec_rt_count == 0) {
            iwlwifi_dbg("IWL9260: no legacy CPU1 INST and no SEC_RT runtime sections found\n");
            return -5;
        }
        iwlwifi_dbg("IWL9260: using SEC_RT runtime image without legacy CPU1 INST\n");
    }

    return 0;
}
