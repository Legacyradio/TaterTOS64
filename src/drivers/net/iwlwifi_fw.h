#ifndef TATER_IWLWIFI_FW_H
#define TATER_IWLWIFI_FW_H

#include <stdint.h>

#define IWLWIFI_FW_CANON_PATH "/firmware/iwlwifi-9260.ucode"
#define IWLWIFI_FW_ALT_PATH   "/firmware/iwlwifi-9260-th-b0-jf-b0-46.ucode"
#define IWL_UCODE_TLV_MAGIC   0x0a4c5749u  /* "IWL\n" in LE */

/* TLV header (from Linux iwl-fw-file.h) */
struct iwl_ucode_tlv {
    uint32_t type;
    uint32_t length;
    /* data follows immediately */
};

/* Key TLV type IDs */
#define IWL_UCODE_TLV_INVALID          0
#define IWL_UCODE_TLV_INST             1   /* CPU1 instructions (legacy) */
#define IWL_UCODE_TLV_DATA             2   /* CPU1 data (legacy) */
#define IWL_UCODE_TLV_INIT             3   /* Init instructions (legacy) */
#define IWL_UCODE_TLV_INIT_DATA        4   /* Init data (legacy) */
#define IWL_UCODE_TLV_BOOT             5
#define IWL_UCODE_TLV_PROBE_MAX_LEN    6
#define IWL_UCODE_TLV_MEM_DESC         7
#define IWL_UCODE_TLV_RUNT_EVTLOG_PTR  8
#define IWL_UCODE_TLV_RUNT_EVTLOG_SIZE 9
#define IWL_UCODE_TLV_RUNT_ERRLOG_PTR  10
#define IWL_UCODE_TLV_INIT_EVTLOG_PTR  11
#define IWL_UCODE_TLV_INIT_EVTLOG_SIZE 12
#define IWL_UCODE_TLV_INIT_ERRLOG_PTR  13
#define IWL_UCODE_TLV_ENHANCE_SENS_TBL 14
#define IWL_UCODE_TLV_PHY_CALIBRATION  15
#define IWL_UCODE_TLV_FLAGS            18
#define IWL_UCODE_TLV_SEC_RT           19   /* Runtime section (gen2) */
#define IWL_UCODE_TLV_SEC_INIT         20   /* Init section (gen2) */
#define IWL_UCODE_TLV_SEC_WOWLAN       21
#define IWL_UCODE_TLV_DEF_CALIB        22
#define IWL_UCODE_TLV_PHY_SKU          23
#define IWL_UCODE_TLV_SECURE_SEC_RT    24
#define IWL_UCODE_TLV_SECURE_SEC_INIT  25
#define IWL_UCODE_TLV_SECURE_SEC_WOW   26
#define IWL_UCODE_TLV_NUM_OF_CPU       27
#define IWL_UCODE_TLV_CSCHEME          28
#define IWL_UCODE_TLV_API_CHANGES_SET  29
#define IWL_UCODE_TLV_ENABLED_CAPABILITIES 30
#define IWL_UCODE_TLV_N_SCAN_CHANNELS  31
#define IWL_UCODE_TLV_PAGING           32
#define IWL_UCODE_TLV_SEC_RT_USNIFFER  34
#define IWL_UCODE_TLV_FW_VERSION       36
#define IWL_UCODE_TLV_FW_DBG_DEST      38
#define IWL_UCODE_TLV_FW_DBG_CONF      39
#define IWL_UCODE_TLV_FW_DBG_TRIGGER   40
#define IWL_UCODE_TLV_CMD_VERSIONS     48
#define IWL_UCODE_TLV_FW_GSCAN_CAPA    50
#define IWL_UCODE_TLV_FW_MEM_SEG       51

/* Max paging sections we'll track */
#define IWL_MAX_PAGING_SECTIONS         32
#define IWL_MAX_SEC_SECTIONS            32

struct iwl_fw_section {
    const uint8_t *data;  /* payload only; excludes the 4-byte TLV offset */
    uint32_t size;        /* payload size only */
    uint32_t offset;      /* device SRAM destination from the TLV header */
};

/* Raw firmware image loaded from VFS */
struct iwlwifi_fw_image {
    const char *path;
    const uint8_t *data;
    uint32_t size;
    uint32_t magic;
    uint32_t magic_offset;
    uint8_t  embedded;   /* 1 = data points to linked blob, don't kfree */
};

/* Parsed firmware pieces extracted from TLV chain */
struct iwl_fw_pieces {
    /* Firmware version from header */
    uint8_t  ver_major;
    uint8_t  ver_minor;
    uint8_t  ver_api;
    uint8_t  ver_serial;
    uint32_t build;
    char     human_readable[64];

    /* CPU1 runtime sections (INST/DATA or SEC_RT) */
    const uint8_t *cpu1_inst_data;
    uint32_t       cpu1_inst_size;
    const uint8_t *cpu1_data_data;
    uint32_t       cpu1_data_size;

    /* CPU2/init sections (INIT/INIT_DATA or SEC_INIT) */
    const uint8_t *cpu2_inst_data;
    uint32_t       cpu2_inst_size;
    const uint8_t *cpu2_data_data;
    uint32_t       cpu2_data_size;

    /* Secure multi-section images (runtime/init TLV streams) */
    struct iwl_fw_section sec_rt[IWL_MAX_SEC_SECTIONS];
    uint32_t              sec_rt_count;
    struct iwl_fw_section sec_init[IWL_MAX_SEC_SECTIONS];
    uint32_t              sec_init_count;

    /* Paging sections */
    const uint8_t *paging_data[IWL_MAX_PAGING_SECTIONS];
    uint32_t       paging_size[IWL_MAX_PAGING_SECTIONS];
    uint32_t       paging_count;

    /* Capabilities and flags */
    uint32_t flags;
    uint32_t phy_config;
    uint32_t num_cpus;

    /* Total TLV entries parsed */
    uint32_t tlv_count;
};

int iwlwifi_fw_load(struct iwlwifi_fw_image *out);
void iwlwifi_fw_release(struct iwlwifi_fw_image *fw);
int iwlwifi_fw_parse_tlv(const struct iwlwifi_fw_image *fw, struct iwl_fw_pieces *out);
void iwlwifi_dbg(const char *fmt, ...);

#endif
