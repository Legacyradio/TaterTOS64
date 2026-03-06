// ACPI extended features: _PRT, _OSC, _DSM, thermal, battery, backlight

#include <stdint.h>
#include "extended.h"
#include "ec.h"
#include "events.h"
#include "fadt.h"
#include "namespace.h"
#include "aml_exec.h"
#include "aml_types.h"
#include "resources.h"
#include "tables.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../mm/vmm.h"
#include "../../drivers/pci/pci.h"

void kprint(const char *fmt, ...);
void acpi_shutdown(void);
void *kmalloc(uint64_t size);

static struct fry_battery_status g_batt;
static struct acpi_node *g_batt_nodes[4];
static uint32_t g_batt_count;

/* EC init tracking — surfaced via sysinfo for bare-metal debug */
static uint8_t g_ec_node_found;   /* PNP0C09 found in namespace */
static uint8_t g_ec_reg_called;   /* _REG(3,1) was invoked */
static uint8_t g_ec_ini_found;    /* _INI was found and called */
static uint8_t g_ec_gpe_found;    /* _GPE object found */
static uint8_t g_ec_gpe_num = 0xFF; /* GPE number (0xFF = none) */
static uint8_t g_ec_ports_source; /* 0=default 1=ECDT 2=_CRS */
static uint16_t g_lpc_ioe_before = 0xFFFFu; /* Intel LPC/eSPI IOE[15:0] at 00:1f.0 + 0x82 */
static uint16_t g_lpc_ioe_after = 0xFFFFu;  /* Value after forcing EC decode bit */
static uint8_t g_lpc_bus;
static uint8_t g_lpc_slot = 0x1Fu;
static uint8_t g_lpc_func = 0u;
static uint16_t g_lpc_vendor = 0xFFFFu;
static uint16_t g_lpc_device = 0xFFFFu;
static uint8_t g_lpc_class_code = 0xFFu;
static uint8_t g_lpc_subclass = 0xFFu;
static uint8_t g_lpc_prog_if = 0xFFu;
static uint16_t g_lpc_cmd = 0xFFFFu;
static uint32_t g_lpc_reg80_before = 0xFFFFFFFFu;
static uint32_t g_lpc_reg80_after = 0xFFFFFFFFu;
static uint32_t g_lpc_reg84 = 0xFFFFFFFFu;
static uint32_t g_lpc_reg88 = 0xFFFFFFFFu;
static uint8_t g_lpc_write_attempted;
static uint8_t g_lpc_write_performed;

/* Intel PCH SBREG/PCR constants (from Intel PCH datasheets + coreboot) */
#define SBREG_BAR_DEFAULT    0xFD000000ULL
#define P2SB_CFG_HIDE_OFF    0xE0u
#define P2SB_CFG_HIDE_BIT    (1u << 8)
/*
 * DMI PID is PCH-family specific:
 *  - Coffee/Cannon Lake commonly uses 0x88
 *  - Some newer Intel PCH families use different IDs (for example 0xEF/0x2F)
 * Probe a small candidate set at runtime instead of hardcoding one PID.
 */
#define PID_DMI_DEFAULT      0xEFu
#define PID_DMI_CANNONLAKE   0x88u
#define PID_DMI_PANTHERLAKE  0x2Fu
#define PCR_DMI_LPCIOD       0x2770u
#define PCR_DMI_LPCIOE       0x2774u
#define IOE_KBC_60_64        (1u << 10)
#define IOE_EC_62_66         (1u << 11)
#define SBREG_BAR_ALT0       0xFE000000ULL
#define SBREG_BAR_ALT1       0xFC000000ULL
#define SBREG_BAR_ALT2       0xFB000000ULL
/* eSPI PCR PID candidates — varies by PCH family, scan all three */
#define PID_ESPI_A           0x72u   /* Cannon Lake / Ice Lake */
#define PID_ESPI_B           0xC7u   /* Skylake / Coffee Lake (shared with LPC) */
#define PID_ESPI_C           0x6Eu   /* Alternate seen in some 300-series docs */

/*
 * eSPI PCR register offsets (from Intel EDK2 CoffeelakeSiliconPkg PchRegsLpc.h)
 * These are offsets within the eSPI PID's 64KB region in SBREG_BAR.
 */
#define R_ESPI_PCR_SLV_CFG_CTL   0x4000u  /* Slave Config Register Control */
#define R_ESPI_PCR_SLV_CFG_DATA  0x4004u  /* Slave Config Register Data */
#define R_ESPI_PCR_PCERR_SLV0    0x4020u  /* Peripheral Channel Error Slave 0 */
#define R_ESPI_PCR_VWERR_SLV0    0x4030u  /* Virtual Wire Error Slave 0 */
#define R_ESPI_PCR_FCERR_SLV0    0x4040u  /* Flash Channel Error Slave 0 */
#define R_ESPI_PCR_LNKERR_SLV0   0x4050u  /* Link Error Slave 0 */
#define R_ESPI_PCR_CFG_VAL       0xC00Cu  /* Config Validation (bit 0 = eSPI strap) */

/* eSPI PCI config space (D31:F0) */
#define R_ESPI_CFG_PCBC          0xDCu    /* Peripheral Channel BIOS Control */
#define B_ESPI_CFG_PCBC_ESPI_EN  (1u << 2) /* eSPI Enable pin strap */

/* eSPI slave register addresses (accessed indirectly via SLV_CFG_CTL/DATA) */
#define ESPI_SLAVE_GEN_CFG       0x0008u  /* General/Device Configuration */
#define ESPI_SLAVE_CH0_CAP       0x0010u  /* Peripheral Channel Cap & Config */
#define ESPI_SLAVE_CH1_CAP       0x0020u  /* Virtual Wire Cap & Config */
#define ESPI_CLEAR_MAX_PASSES    3
#define ESPI_CLEAR_SETTLE_LOOPS  2000000u  /* ~2ms — reduced for fast boot */

/*
 * fry446: eSPI channel initialization registers
 *
 * Master-to-Slave VW transmit registers (PCR[eSPI] + offset).
 * Each 32-bit register controls one VW index group with 4 wires.
 * Format (Coffee Lake / 300-series PCH):
 *   [7:0]   VW Index number
 *   [8]     Wire 0 data value
 *   [9]     Wire 1 data value
 *   [10]    Wire 2 data value
 *   [11]    Wire 3 data value
 *   [12]    Wire 0 valid/enable
 *   [13]    Wire 1 valid/enable
 *   [14]    Wire 2 valid/enable
 *   [15]    Wire 3 valid/enable
 *   [31:16] Platform-specific / reserved
 *
 * Slave-to-Master VW receive registers are at a lower base.
 *
 * Standard VW Index assignments (eSPI Base Spec Rev 1.6, Table 5):
 *   Index 2:  SLP_S3#[w0], SLP_S4#[w1], SLP_S5#[w2]
 *   Index 3:  SUS_STAT#[w0]       (or OOB_RST_ACK[w2] slave→master)
 *   Index 4:  OOB_RST_WARN[w0], PLT_RST#[w1], HOST_RST_WARN[w2]
 *   Index 5:  SLP_A#[w0]
 *   Index 6:  SLP_LAN#[w0], SLP_WLAN#[w1]
 *   Index 7:  HOST_C10[w0]
 *   Index 40+: SUS_WARN#, SUS_ACK, etc.
 *
 * PLT_RST# is active-low: 0 = asserted (EC held in reset),
 *                          1 = de-asserted (EC allowed to run).
 */
#define R_ESPI_PCR_XMVW_BASE        0x4220u  /* Master-to-Slave VW TX base */
#define R_ESPI_PCR_XSVW_BASE        0x4200u  /* Slave-to-Master VW RX base */
#define ESPI_VW_REG_COUNT            8u       /* Number of VW registers to scan */
#define ESPI_VW_IDX_PLTRST           4u       /* PLT_RST# lives in VW Index 4 */
#define ESPI_VW_PLTRST_WIRE          1u       /* PLT_RST# is wire 1 within index 4 */

/* Slave channel capability register bit definitions (eSPI Base Spec) */
#define B_ESPI_SLV_CH_EN             (1u << 0)   /* Channel Enable (host-writable) */
#define B_ESPI_SLV_CH_READY          (1u << 16)  /* Channel Ready (slave-asserted) */

/* Additional slave register addresses (complement CH0/CH1 already defined) */
#define ESPI_SLAVE_CH2_CAP           0x0030u  /* OOB Channel Cap & Config */
#define ESPI_SLAVE_CH3_CAP           0x0040u  /* Flash Channel Cap & Config */

/* Channel init retry parameters */
#define ESPI_CHINIT_MAX_RETRIES      2u
#define ESPI_CHINIT_BASE_DELAY       (ESPI_CLEAR_SETTLE_LOOPS * 2u)  /* ~4ms */

/*
 * eSPI status masks:
 * - Keep only status bits in W1C clears and "error present" decisions.
 * - Do NOT treat enable/default bits as active faults.
 */
#define B_ESPI_PCR_XERR_XFES             (1u << 4)   /* Fatal error status */
#define B_ESPI_PCR_XERR_XNFES            (1u << 12)  /* Non-fatal error status */
#define B_ESPI_PCR_FCERR_SLV0_SAFBLK     (1u << 17)  /* Flash SAF blocked status */
#define B_ESPI_PCR_PCERR_SLV0_PCURD      (1u << 24)  /* Unsupported request detected */
#define B_ESPI_PCR_LNKERR_SLV0_LFET1S    (1u << 20)  /* Link fatal error type-1 status */
#define B_ESPI_PCR_LNKERR_SLV0_SLCRR     (1u << 31)  /* Link/slave recovery required */
#define ESPI_PCERR_STATUS_MASK  (B_ESPI_PCR_PCERR_SLV0_PCURD | B_ESPI_PCR_XERR_XFES | B_ESPI_PCR_XERR_XNFES)
#define ESPI_VWERR_STATUS_MASK  (B_ESPI_PCR_XERR_XFES | B_ESPI_PCR_XERR_XNFES)
#define ESPI_FCERR_STATUS_MASK  (B_ESPI_PCR_FCERR_SLV0_SAFBLK | B_ESPI_PCR_XERR_XFES | B_ESPI_PCR_XERR_XNFES)
#define ESPI_LNKERR_STATUS_MASK (B_ESPI_PCR_LNKERR_SLV0_LFET1S | B_ESPI_PCR_LNKERR_SLV0_SLCRR)

static uint32_t g_pcr_ioe_before = 0xFFFFFFFFu;
static uint32_t g_pcr_ioe_after  = 0xFFFFFFFFu;
static uint8_t  g_pcr_mirror_done;
static uint8_t  g_p2sb_was_hidden;
static uint8_t  g_ec_early_sts = 0xFFu;
static uint8_t  g_ec_post_lpc_sts = 0xFFu; /* port 0x66 status after LPC decode config */
static uint8_t  g_ec_pre_reg_sts = 0xFFu; /* port 0x66 status right before _REG */
static uint8_t  g_pcr_pid_used = PID_DMI_DEFAULT; /* actual PID used for mirror (diag) */
static uint8_t  g_ec_pre_reg_probe_ok;  /* 1 if pre-_REG probe succeeded */
static uint8_t  g_ec_recovery_method;   /* which recovery method worked (0-6) */
static uint8_t  g_ec_cand_count;        /* total EC candidates found (G446) */

/* fry421: immediate raw probe diagnostics (before ANY init) */
static uint8_t  g_ec_imm_step = 0;        /* 0=not_tried 1=STS_FF 2=IBF_PRE 3=IBF_POST 4=OBF_TMO 5=OK */
static uint8_t  g_ec_imm_val = 0;          /* reg[0] value if imm_step==5 */
static uint8_t  g_ec_imm_post_sts = 0xFFu; /* port 0x66 after immediate probe */
static uint8_t  g_ec_post_setup_sts = 0xFFu; /* port 0x66 after ec_setup_ports */

/* fry422: floating bus detection + force ACPI_ENABLE probe */
static uint8_t  g_ec_ibf_seen = 0;    /* 1 if IBF was observed set after cmd write (real EC) */
static uint8_t  g_ec_pre_data = 0;    /* inb(0x62) before any command (pending data) */
static uint8_t  g_ec_smi_sent = 0;    /* 1 if force ACPI_ENABLE was sent before re-probe */
static uint8_t  g_ec_imm2_step = 0;   /* result of second immediate probe (after SMI) */

/* fry423: Gen3/Gen4 decode + eSPI channel diagnostics */
static uint32_t g_lpc_reg8c = 0xFFFFFFFFu;
static uint32_t g_lpc_reg90 = 0xFFFFFFFFu;
static uint64_t g_sbreg_bar_saved = 0;       /* saved for eSPI probe reuse */
static uint32_t g_espi_raw[8];               /* key eSPI PCR registers */
static uint8_t  g_espi_probed = 0;
static uint8_t  g_espi_pid = 0;              /* PID that worked */
static uint8_t  g_espi_en_strap = 0;         /* D31:F0 offset 0xDC bit 2 */
static uint32_t g_espi_pcbc = 0xFFFFFFFFu;   /* raw D31:F0 offset 0xDC */

/* fry435: eSPI error clear diagnostics */
static uint32_t g_espi_pre_clear[4];   /* PCERR/VWERR/FCERR/LNKERR before first clear */
static uint32_t g_espi_post_clear[4];  /* after most recent clear */
static uint8_t  g_espi_clear_run = 0;  /* bit 0=proactive, bit 1=recovery */
static uint8_t  g_espi_clear_found = 0; /* 1 if errors were non-zero */
static uint8_t  g_espi_clear_ok = 0;   /* 1 if all regs zero after clear */

/* fry444: eSPI slave channel diagnostics */
static uint32_t g_espi_slave_pc_cap = 0xFFFFFFFFu;  /* Peripheral Channel Cap (slave 0x0010) */
static uint32_t g_espi_slave_vw_cap = 0xFFFFFFFFu;  /* Virtual Wire Channel Cap (slave 0x0020) */
static uint8_t  g_espi_slave_pc_en = 0;              /* PC channel enabled on slave */
static uint8_t  g_espi_slave_vw_en = 0;              /* VW channel enabled on slave */
static uint8_t  g_espi_gen_chan_sup = 0;              /* GenCfg channel support bits [15:12] */
static uint8_t  g_espi_slave_read_ok = 0;            /* bitmask: bit0=genCfg bit1=pcCap bit2=vwCap */

/* fry446: eSPI channel initialization diagnostics */
static uint32_t g_espi_mvw[ESPI_VW_REG_COUNT];       /* Master-to-Slave VW TX regs (raw) */
static uint32_t g_espi_svw[ESPI_VW_REG_COUNT];       /* Slave-to-Master VW RX regs (raw) */
static uint8_t  g_espi_pltrst_state = 0xFFu;         /* b0=data b1=valid; 0xFF=not found */
static uint8_t  g_espi_pltrst_sent = 0;              /* 1 if PLT_RST de-assert was attempted */
static uint8_t  g_espi_chinit_result = 0;             /* 0=not_run 1=already_ready 2=toggled_ok
                                                          3=partial 4=failed */
static uint8_t  g_espi_chinit_retries = 0;            /* actual retry count */
static uint32_t g_espi_chinit_pc_cap = 0xFFFFFFFFu;   /* PC_CAP after channel init */
static uint32_t g_espi_chinit_vw_cap = 0xFFFFFFFFu;   /* VW_CAP after channel init */

static struct acpi_node *g_backlight_node;
static uint32_t g_bcl_levels[32];
static uint32_t g_bcl_count;
static uint32_t g_bcl_supported_levels[32];
static uint32_t g_bcl_supported_count;
static uint32_t g_brightness;
static uint32_t g_brightness_raw;

static int eval_path_obj(struct acpi_node *node, const char *suffix, struct acpi_object **out);
static int eval_path_obj_args(struct acpi_node *node, const char *suffix,
                              struct acpi_object **args, uint32_t arg_count,
                              struct acpi_object **out);
static int invoke_path_method(struct acpi_node *node, const char *suffix,
                              struct acpi_object **args, uint32_t arg_count,
                              struct acpi_object **out);
static int lpc_is_intel_300_series(uint16_t lpc_device);

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

/*
 * Legacy PCI config access via I/O ports 0xCF8/0xCFC.
 * This bypasses ECAM and goes through the host bridge directly.
 * Required for accessing hidden devices (P2SB) where ECAM doesn't decode.
 */
#define PCI_CF8_ADDR  0x0CF8u
#define PCI_CFC_DATA  0x0CFCu

static uint32_t pci_cf8_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | ((uint32_t)offset & 0xFCu);
    outl(PCI_CF8_ADDR, addr);
    return inl(PCI_CFC_DATA);
}

static void pci_cf8_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | ((uint32_t)offset & 0xFCu);
    outl(PCI_CF8_ADDR, addr);
    outl(PCI_CFC_DATA, val);
}

static uint8_t pci_cf8_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | ((uint32_t)offset & 0xFCu);
    outl(PCI_CF8_ADDR, addr);
    /* Read the specific byte from port 0xCFC + (offset & 3) */
    return inb((uint16_t)(PCI_CFC_DATA + (offset & 3u)));
}

static void pci_cf8_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t addr = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | ((uint32_t)offset & 0xFCu);
    outl(PCI_CF8_ADDR, addr);
    /* Write the specific byte at port 0xCFC + (offset & 3) */
    outb((uint16_t)(PCI_CFC_DATA + (offset & 3u)), val);
}

#define EC_MAX_CANDIDATES 8
#define EC_MAX_CRS_PORTS  8

struct ec_candidate {
    struct acpi_node *node;
    uint16_t data_port;
    uint16_t cmd_port;
    uint8_t has_crs_ports;
    uint8_t sta_known;
    uint8_t present;
    uint8_t has_reg;
    uint8_t has_ini;
    uint8_t has_gpe;
    uint8_t source;       /* 0=fallback, 1=ECDT, 2=CRS (PNP0C09) */
    uint8_t gpe_num;      /* GPE number from _GPE or ECDT */
    int score;
};

struct ec_scan_ctx {
    struct ec_candidate cands[EC_MAX_CANDIDATES];
    uint32_t count;
};

static uint64_t obj_to_int(struct acpi_object *o) {
    return aml_obj_to_int(o);
}

static int get_sta_flags(struct acpi_node *node, uint64_t *out) {
    struct acpi_object *sta = 0;
    if (!node || !out) return -1;
    if (eval_path_obj(node, "STA", &sta) != 0 || !sta) return -1;
    if (sta->type != AML_OBJ_INTEGER) return -1;
    *out = (uint64_t)sta->v.integer;
    return 0;
}

static int has_method3(struct acpi_node *node, const char *name3) {
    char name[4];
    if (!node || !name3) return 0;
    name[0] = '_';
    name[1] = name3[0];
    name[2] = name3[1];
    name[3] = name3[2];
    return ns_find_child(node, name) != 0;
}

static int ec_node_present(struct acpi_node *node) {
    uint64_t sta = 0;
    if (!node) return 0;
    if (get_sta_flags(node, &sta) != 0) {
        return 1;
    }
    if ((sta & 0x01u) != 0) return 1;
    if ((sta & 0x10u) != 0) return 1;
    return (sta & 0x0Fu) != 0;
}

static int ec_port_is_plausible(uint16_t port) {
    /*
     * Avoid null/placeholder low ports from buggy _CRS templates.
     * Keep this broad so non-Dell systems with non-standard EC ports still work.
     */
    if (port < 0x10u) return 0;
    return 1;
}

static int ec_pair_score(uint16_t data_port, uint16_t cmd_port) {
    int score = 0;
    if (!ec_port_is_plausible(data_port) || !ec_port_is_plausible(cmd_port)) {
        return -1000000;
    }
    if (data_port == cmd_port) return -1000000;

    if (data_port == 0x62u && cmd_port == 0x66u) score += 1000;
    else if (data_port == 0x66u && cmd_port == 0x62u) score += 900;
    else if (data_port == 0x68u && cmd_port == 0x6Cu) score += 850;
    else if (data_port == 0x6Cu && cmd_port == 0x68u) score += 800;

    if ((uint16_t)(data_port + 4u) == cmd_port) score += 300;
    else if ((uint16_t)(cmd_port + 4u) == data_port) score += 250;
    else score += 10;

    return score;
}

static int ec_pick_port_pair(const uint16_t *ports, int nio,
                             uint16_t *data_out, uint16_t *cmd_out) {
    int idx62 = -1;
    int idx66 = -1;
    int best_score = -1000000;
    uint16_t best_data = 0;
    uint16_t best_cmd = 0;
    if (!ports || nio <= 0 || !data_out || !cmd_out) return -1;

    for (int i = 0; i < nio; i++) {
        if (ports[i] == 0x62u && idx62 < 0) idx62 = i;
        if (ports[i] == 0x66u && idx66 < 0) idx66 = i;
    }
    if (idx62 >= 0 && idx66 >= 0) {
        *data_out = 0x62u;
        *cmd_out = 0x66u;
        return 0;
    }

    for (int i = 0; i < nio; i++) {
        for (int j = 0; j < nio; j++) {
            if (i == j) continue;
            int score = ec_pair_score(ports[i], ports[j]);
            if (score > best_score) {
                best_score = score;
                best_data = ports[i];
                best_cmd = ports[j];
            }
        }
    }
    if (best_score > -1000000) {
        *data_out = best_data;
        *cmd_out = best_cmd;
        return 0;
    }

    if (nio >= 2) {
        for (int i = 0; i < nio; i++) {
            if (!ec_port_is_plausible(ports[i])) continue;
            *data_out = ports[i];
            *cmd_out = (i + 1 < nio) ? ports[i + 1] : (uint16_t)(ports[i] + 4u);
            if (!ec_port_is_plausible(*cmd_out) || *cmd_out == *data_out) {
                *cmd_out = (uint16_t)(*data_out + 4u);
            }
            if (ec_port_is_plausible(*cmd_out) && *cmd_out != *data_out) {
                return 0;
            }
        }
    }

    if (ec_port_is_plausible(ports[0])) {
        *data_out = ports[0];
        *cmd_out = (uint16_t)(ports[0] + 4u);
        if (ec_port_is_plausible(*cmd_out) && *cmd_out != *data_out) {
            return 0;
        }
    }
    return -1;
}

static int ec_extract_ports_from_crs(struct acpi_node *ec_node,
                                     uint16_t *data_out,
                                     uint16_t *cmd_out,
                                     int verbose) {
    struct acpi_object *crs = 0;
    uint16_t io_ports[EC_MAX_CRS_PORTS];
    int nio = 0;
    char path[256];

    if (!ec_node || !data_out || !cmd_out) return -1;
    ns_build_path(ec_node, path, sizeof(path));

    if (eval_path_obj(ec_node, "CRS", &crs) != 0 || !crs ||
        crs->type != AML_OBJ_BUFFER || crs->v.buffer.length == 0) {
        if (verbose) {
            kprint("ACPI: EC _CRS unavailable on %s\n", path);
        }
        return -1;
    }

    nio = acpi_get_io_from_crs(crs->v.buffer.data, crs->v.buffer.length,
                               io_ports, EC_MAX_CRS_PORTS);
    if (verbose) {
        kprint("ACPI: EC _CRS on %s has %d IO descriptors\n", path, nio);
        if (nio > 0) {
            kprint("ACPI: EC _CRS IO candidates:");
            for (int i = 0; i < nio; i++) {
                kprint(" 0x%02x", (uint32_t)io_ports[i]);
            }
            kprint("\n");
        }
    }
    if (nio <= 0) return -1;

    if (ec_pick_port_pair(io_ports, nio, data_out, cmd_out) != 0) {
        if (verbose) {
            kprint("ACPI: EC _CRS rejected (no plausible EC port pair)\n");
        }
        return -1;
    }

    if (verbose) {
        kprint("ACPI: EC _CRS selected ports data=0x%02x cmd=0x%02x\n",
               (uint32_t)(*data_out), (uint32_t)(*cmd_out));
    }
    return 0;
}

static int node_looks_like_battery(struct acpi_node *node) {
    if (!node) return 0;
    if (ns_hid_match(node, "PNP0C0A")) return 1;
    if (has_method3(node, "BST") && (has_method3(node, "BIF") || has_method3(node, "BIX"))) {
        return 1;
    }
    return 0;
}

static uint32_t u32_abs_diff(uint32_t a, uint32_t b) {
    return (a >= b) ? (a - b) : (b - a);
}

static void backlight_reset_levels(void) {
    g_bcl_count = 0;
    g_bcl_supported_count = 0;
}

static void backlight_add_supported_level(uint32_t level) {
    for (uint32_t i = 0; i < g_bcl_supported_count; i++) {
        if (g_bcl_supported_levels[i] == level) return;
    }
    if (g_bcl_supported_count < 32) {
        g_bcl_supported_levels[g_bcl_supported_count++] = level;
    }
}

static void backlight_parse_bcl(struct acpi_object *bcl) {
    backlight_reset_levels();
    if (!bcl || bcl->type != AML_OBJ_PACKAGE) return;

    for (uint32_t i = 0; i < bcl->v.package.count && g_bcl_count < 32; i++) {
        g_bcl_levels[g_bcl_count++] = (uint32_t)obj_to_int(bcl->v.package.items[i]);
    }

    uint32_t start = (g_bcl_count > 2) ? 2 : 0;
    for (uint32_t i = start; i < g_bcl_count; i++) {
        backlight_add_supported_level(g_bcl_levels[i]);
    }
    /* Some firmware only exposes one or two values; use them if needed. */
    if (g_bcl_supported_count == 0) {
        for (uint32_t i = 0; i < g_bcl_count; i++) {
            backlight_add_supported_level(g_bcl_levels[i]);
        }
    }
}

static int backlight_minmax(uint32_t *min_out, uint32_t *max_out) {
    if (!min_out || !max_out || g_bcl_supported_count == 0) return -1;
    uint32_t minv = g_bcl_supported_levels[0];
    uint32_t maxv = g_bcl_supported_levels[0];
    for (uint32_t i = 1; i < g_bcl_supported_count; i++) {
        uint32_t v = g_bcl_supported_levels[i];
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
    }
    *min_out = minv;
    *max_out = maxv;
    return 0;
}

static uint32_t backlight_raw_to_percent(uint32_t raw) {
    uint32_t minv = 0, maxv = 0;
    if (backlight_minmax(&minv, &maxv) != 0) {
        return (raw > 100u) ? 100u : raw;
    }
    if (maxv <= 100u && minv <= 100u) {
        return (raw > 100u) ? 100u : raw;
    }
    if (maxv <= minv) return raw ? 100u : 0u;
    if (raw <= minv) return 0u;
    if (raw >= maxv) return 100u;
    return (uint32_t)(((uint64_t)(raw - minv) * 100ULL + ((maxv - minv) / 2u)) /
                      (uint64_t)(maxv - minv));
}

static uint32_t backlight_percent_to_raw(uint32_t percent) {
    if (percent > 100u) percent = 100u;
    uint32_t minv = 0, maxv = 0;
    if (backlight_minmax(&minv, &maxv) != 0) {
        return percent;
    }
    if (maxv <= 100u && minv <= 100u) {
        return percent;
    }
    if (maxv <= minv) return maxv;

    uint32_t span = maxv - minv;
    uint32_t target = minv + (uint32_t)(((uint64_t)span * (uint64_t)percent + 50ULL) / 100ULL);

    uint32_t best = g_bcl_supported_levels[0];
    uint32_t best_diff = u32_abs_diff(best, target);
    for (uint32_t i = 1; i < g_bcl_supported_count; i++) {
        uint32_t v = g_bcl_supported_levels[i];
        uint32_t d = u32_abs_diff(v, target);
        if (d < best_diff) {
            best = v;
            best_diff = d;
        }
    }
    return best;
}

static const char *obj_to_str(struct acpi_object *o) {
    if (!o || o->type != AML_OBJ_STRING) return 0;
    return o->v.string;
}

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static struct acpi_object *make_buf(const uint8_t *data, uint32_t len) {
    if (len == 0) {
        return aml_obj_make_buf(0, 0);
    }
    uint8_t *b = (uint8_t *)kmalloc(len);
    if (!b) return 0;
    for (uint32_t i = 0; i < len; i++) b[i] = data[i];
    return aml_obj_make_buf(b, len);
}

static struct acpi_node *find_pci_root(void) {
    struct acpi_node *n = ns_find_device("PNP0A08");
    if (n) return n;
    n = ns_find_device("PNP0A03");
    if (n) return n;
    n = ns_lookup(ns_root(), "\\_SB_.PCI0");
    if (n) return n;
    return ns_lookup(ns_root(), "\\PCI0");
}

struct prt_find_ctx {
    struct acpi_node *node;
};

static void find_prt_node_cb(struct acpi_node *node, void *ctx) {
    struct prt_find_ctx *c = (struct prt_find_ctx *)ctx;
    if (!c || c->node) return;
    if (!node || node->type != ACPI_NODE_DEVICE) return;
    static const char prt_name[4] = {'_','P','R','T'};
    if (ns_find_child(node, prt_name)) {
        c->node = node;
    }
}

static void collect_ec_candidates_cb(struct acpi_node *node, void *ctx) {
    struct ec_scan_ctx *scan = (struct ec_scan_ctx *)ctx;
    struct ec_candidate *cand;
    if (!scan || scan->count >= EC_MAX_CANDIDATES) return;
    if (!ns_hid_match(node, "PNP0C09")) return;

    cand = &scan->cands[scan->count++];
    cand->node = node;
    cand->data_port = 0;
    cand->cmd_port = 0;
    cand->has_crs_ports = 0;
    cand->sta_known = 0;
    cand->present = 1;
    cand->has_reg = has_method3(node, "REG") ? 1u : 0u;
    cand->has_ini = has_method3(node, "INI") ? 1u : 0u;
    cand->has_gpe = has_method3(node, "GPE") ? 1u : 0u;
    cand->source = 2; /* CRS/PNP0C09 */
    cand->gpe_num = 0xFF;
    cand->score = 0;

    {
        uint64_t sta = 0;
        if (get_sta_flags(node, &sta) == 0) {
            cand->sta_known = 1;
            cand->present = ec_node_present(node) ? 1u : 0u;
        }
    }

    /* fry422: verbose=1 to see why _CRS extraction fails */
    if (ec_extract_ports_from_crs(node, &cand->data_port, &cand->cmd_port, 1) == 0) {
        cand->has_crs_ports = 1;
    }

    cand->score = 0;
    if (cand->present) cand->score += 16;
    if (cand->has_crs_ports) cand->score += 8;
    if (cand->has_reg) cand->score += 4;
    if (cand->has_gpe) cand->score += 2;
    if (cand->has_ini) cand->score += 1;
}

static struct acpi_node *find_any_prt_node(void) {
    struct prt_find_ctx ctx;
    ctx.node = 0;
    ns_walk(find_prt_node_cb, &ctx);
    return ctx.node;
}

static int eval_path_obj(struct acpi_node *node, const char *suffix, struct acpi_object **out) {
    char path[256];
    ns_build_path(node, path, sizeof(path));
    uint32_t i = 0;
    while (path[i] && i < sizeof(path) - 1) i++;
    if (i + 5 >= sizeof(path)) return -1;
    path[i++] = '.';
    path[i++] = '_';
    path[i++] = suffix[0];
    path[i++] = suffix[1];
    path[i++] = suffix[2];
    path[i++] = 0;
    *out = aml_eval(path);
    return (*out) ? 0 : -1;
}

static int eval_path_obj_args(struct acpi_node *node, const char *suffix,
                              struct acpi_object **args, uint32_t arg_count,
                              struct acpi_object **out) {
    char path[256];
    ns_build_path(node, path, sizeof(path));
    uint32_t i = 0;
    while (path[i] && i < sizeof(path) - 1) i++;
    if (i + 5 >= sizeof(path)) return -1;
    path[i++] = '.';
    path[i++] = '_';
    path[i++] = suffix[0];
    path[i++] = suffix[1];
    path[i++] = suffix[2];
    path[i++] = 0;
    *out = aml_eval_with_args(path, args, arg_count);
    return (*out) ? 0 : -1;
}

/*
 * Invoke a control method and treat "no return object" as success.
 * ACPI methods like _REG/_INI are often void and still execute correctly.
 */
static int invoke_path_method(struct acpi_node *node, const char *suffix,
                              struct acpi_object **args, uint32_t arg_count,
                              struct acpi_object **out) {
    char path[256];
    struct acpi_node *target;
    uint32_t i = 0;

    if (!node || !suffix) return -1;
    if (out) *out = 0;

    ns_build_path(node, path, sizeof(path));
    while (path[i] && i < sizeof(path) - 1) i++;
    if (i + 5 >= sizeof(path)) return -1;
    path[i++] = '.';
    path[i++] = '_';
    path[i++] = suffix[0];
    path[i++] = suffix[1];
    path[i++] = suffix[2];
    path[i++] = 0;

    target = ns_lookup(ns_root(), path);
    if (!target) {
        kprint("ACPI: invoke _%c%c%c failed (lookup) path=%s\n",
               suffix[0], suffix[1], suffix[2], path);
        return -1;
    }
    if (target->type != ACPI_NODE_METHOD) {
        kprint("ACPI: invoke _%c%c%c failed (type=%u) path=%s\n",
               suffix[0], suffix[1], suffix[2], (uint32_t)target->type, path);
        return -1;
    }

    if (out) {
        *out = aml_eval_with_args(path, args, arg_count);
    } else {
        (void)aml_eval_with_args(path, args, arg_count);
    }
    return 0;
}

static int dev_present(struct acpi_node *node) {
    uint64_t v = 0;
    if (!node) return 0;
    if (get_sta_flags(node, &v) != 0) {
        /* Missing _STA is treated as present for compatibility. */
        return 1;
    }
    /*
     * Control Method Battery devices may only expose presence via bit 4.
     * Accept that bit as present even when bit 0 is clear.
     */
    if (node_looks_like_battery(node) && (v & 0x10u)) return 1;
    if ((v & 0x01u) == 0) return 0;
    if ((v & 0x10u) != 0) return 1;
    return (v & 0x0Fu) != 0;
}

static int get_gsi_from_link_node(struct acpi_node *link, uint32_t *gsi_out) {
    struct acpi_object *crs = 0;
    if (eval_path_obj(link, "CRS", &crs) != 0) return -1;
    if (!crs || crs->type != AML_OBJ_BUFFER) return -1;
    return acpi_get_gsi_from_crs(crs->v.buffer.data, crs->v.buffer.length, gsi_out);
}

static uint32_t pci_get_base_bus(struct acpi_node *root) {
    struct acpi_object *bbn = 0;
    if (eval_path_obj(root, "BBN", &bbn) == 0) {
        return (uint32_t)obj_to_int(bbn);
    }
    return 0;
}

static void pick_lpc_bridge_candidate(uint8_t preferred_bus,
                                      uint8_t *bus_out,
                                      uint8_t *slot_out,
                                      uint8_t *func_out) {
    uint8_t bus = preferred_bus;
    uint8_t slot = 0x1Fu;
    uint8_t func = 0u;
    uint32_t count = 0;
    const struct pci_device_info *devs = pci_get_devices(&count);
    int found = 0;

    if (devs) {
        for (uint32_t i = 0; i < count; i++) {
            if (devs[i].vendor_id == 0x8086u &&
                devs[i].class_code == 0x06u &&
                devs[i].subclass == 0x01u) {
                if (!found) {
                    bus = devs[i].bus;
                    slot = devs[i].slot;
                    func = devs[i].func;
                    found = 1;
                }
                if (devs[i].bus == preferred_bus &&
                    devs[i].slot == 0x1Fu &&
                    devs[i].func == 0u) {
                    bus = devs[i].bus;
                    slot = devs[i].slot;
                    func = devs[i].func;
                    break;
                }
            }
        }
    }

    *bus_out = bus;
    *slot_out = slot;
    *func_out = func;
}

static uint32_t pcr_get_dmi_pid_candidates(uint8_t out[3]) {
    if (!out) return 0;
    /*
     * Intel 300-series/Coffee/Cannon Lake-class PCH commonly uses PID_DMI=0x88.
     * Keep 0xEF as fallback for platforms that use a different DMI PID.
     */
    if (lpc_is_intel_300_series(g_lpc_device)) {
        out[0] = PID_DMI_CANNONLAKE;     /* 0x88 — preferred for Coffee/Cannon Lake */
        out[1] = PID_DMI_DEFAULT;        /* 0xEF — fallback */
        out[2] = PID_DMI_PANTHERLAKE;
        return 3;
    }
    out[0] = PID_DMI_DEFAULT;
    out[1] = PID_DMI_CANNONLAKE;
    out[2] = PID_DMI_PANTHERLAKE;
    return 3;
}

static int pcr_read_dmi_lpc_regs(uint64_t sbreg_bar, uint8_t pid, uint32_t *ioe_out, uint32_t *iod_out) {
    uint64_t ioe_phys = sbreg_bar + ((uint64_t)pid << 16) + PCR_DMI_LPCIOE;
    uint64_t iod_phys = sbreg_bar + ((uint64_t)pid << 16) + PCR_DMI_LPCIOD;

    vmm_ensure_physmap_uc(ioe_phys + sizeof(uint32_t));
    vmm_ensure_physmap_uc(iod_phys + sizeof(uint32_t));

    volatile uint32_t *ioe_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + ioe_phys);
    volatile uint32_t *iod_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + iod_phys);
    uint32_t ioe = *ioe_reg;
    uint32_t iod = *iod_reg;

    if (ioe_out) *ioe_out = ioe;
    if (iod_out) *iod_out = iod;
    return 0;
}

static int pcr_dmi_regs_score(uint32_t ioe, uint32_t iod,
                              uint16_t ioe_hint,
                              uint16_t iod_hint,
                              uint16_t iod_hint_alt) {
    uint16_t ioe16 = (uint16_t)(ioe & 0xFFFFu);
    uint16_t iod16 = (uint16_t)(iod & 0xFFFFu);
    int score = 0;

    if (ioe == 0xFFFFFFFFu && iod == 0xFFFFFFFFu) return -1000;
    if (ioe == 0x00000000u && iod == 0x00000000u) return -1000;

    if (ioe16 == ioe_hint) score += 8;
    if ((ioe16 & 0x00FFu) == (ioe_hint & 0x00FFu)) score += 4;

    if ((ioe16 & (IOE_EC_62_66 | IOE_KBC_60_64)) != 0) {
        score += 3;
    } else if ((ioe_hint & (IOE_EC_62_66 | IOE_KBC_60_64)) != 0) {
        /*
         * Do not hard-reject decode bits being clear. On failing Dell boots
         * the valid register can read with decode bits currently off and still
         * be the right target for the mirror write.
         */
        score += 1;
    }

    if ((iod_hint & 0x0070u) != 0 && (iod16 & 0x0070u) == (iod_hint & 0x0070u)) score += 4;
    if (iod_hint_alt != 0u && iod16 == iod_hint_alt) score += 4;
    if (iod_hint_alt != 0u && (iod16 & 0x00FFu) == (iod_hint_alt & 0x00FFu)) score += 2;

    if (ioe16 != 0xFFFFu && ioe16 != 0x0000u) score += 1;
    if (iod16 != 0xFFFFu && iod16 != 0x0000u) score += 1;

    return score;
}

static uint32_t pcr_get_fallback_bars(uint64_t out[4], uint16_t lpc_device) {
    uint32_t n = 0;
    out[n++] = SBREG_BAR_DEFAULT;

    /*
     * PCH-aware ordering:
     *  - Intel 300-series PCH-H setups commonly decode with these candidates.
     *  - Keep 0xFE/0xFC/0xFB probes as conservative alternatives when BAR
     *    reads are locked by firmware and we must infer sideband base.
     */
    if (lpc_is_intel_300_series(lpc_device)) {
        out[n++] = SBREG_BAR_ALT0;
        out[n++] = SBREG_BAR_ALT1;
        out[n++] = SBREG_BAR_ALT2;
        return n;
    }

    out[n++] = SBREG_BAR_ALT0;
    out[n++] = SBREG_BAR_ALT1;
    out[n++] = SBREG_BAR_ALT2;
    return n;
}

static uint64_t pcr_probe_fallback_sbreg_bar(uint16_t ioe_hint,
                                             uint16_t iod_hint,
                                             uint16_t iod_hint_alt) {
    uint64_t cands[4];
    uint32_t count = pcr_get_fallback_bars(cands, g_lpc_device);
    uint8_t pid_cands[3];
    uint32_t pid_count = pcr_get_dmi_pid_candidates(pid_cands);
    uint32_t probe_idx = 0;
    uint64_t best_bar = 0;
    uint8_t best_pid = pid_cands[0];
    int best_score = -1000;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t bar = cands[i];
        for (uint32_t p = 0; p < pid_count; p++) {
            uint32_t ioe = 0xFFFFFFFFu;
            uint32_t iod = 0xFFFFFFFFu;
            uint8_t pid = pid_cands[p];
            int score = 0;

            pcr_read_dmi_lpc_regs(bar, pid, &ioe, &iod);
            score = pcr_dmi_regs_score(ioe, iod, ioe_hint, iod_hint, iod_hint_alt);
            kprint("ACPI: SBREG probe[%u] bar=0x%08x PID=0x%02x IOE=0x%08x IOD=0x%08x score=%d\n",
                   probe_idx, (uint32_t)bar, (uint32_t)pid, ioe, iod, score);
            probe_idx++;

            if (score > best_score) {
                best_score = score;
                best_bar = bar;
                best_pid = pid;
            }
        }
    }

    if (best_bar && best_score >= 4) {
        g_pcr_pid_used = best_pid;
        kprint("ACPI: SBREG probe selected bar=0x%08x PID=0x%02x score=%d\n",
               (uint32_t)best_bar, (uint32_t)best_pid, best_score);
        return best_bar;
    }

    if (best_bar && best_score >= 1) {
        g_pcr_pid_used = best_pid;
        kprint("ACPI: SBREG probe weak-select bar=0x%08x PID=0x%02x score=%d\n",
               (uint32_t)best_bar, (uint32_t)best_pid, best_score);
        return best_bar;
    }

    return 0;
}

static uint64_t pcr_get_sbreg_bar(uint8_t bus,
                                  uint16_t ioe_hint,
                                  uint16_t iod_hint,
                                  uint16_t iod_hint_alt) {
    const uint8_t p2sb_slot = 0x1Fu;
    const uint8_t p2sb_func = 1u;
    const uint8_t p2sb_hide_hi_off = (uint8_t)(P2SB_CFG_HIDE_OFF + 1u);
    const uint8_t p2sb_hide_hi_mask = (uint8_t)(P2SB_CFG_HIDE_BIT >> 8);
    int unhid = 0;

    /*
     * Use legacy CF8/CFC I/O port access for P2SB, NOT ECAM.
     *
     * When BIOS hides the P2SB, the ECAM region for BDF 00:1f.1
     * stops decoding entirely — reads return 0xFFFFFFFF and writes
     * are silently dropped. The legacy CF8/CFC mechanism goes through
     * the host bridge directly and always reaches the device.
     * This matches Linux p2sb.c, coreboot, and Intel reference code.
     */
    uint32_t id = pci_cf8_read32(bus, p2sb_slot, p2sb_func, 0x00u);

    kprint("ACPI: P2SB %u:1f.1 VendorID=0x%08x (CF8)\n", (uint32_t)bus, id);

    if (id == 0xFFFFFFFFu) {
        /*
         * P2SB is hidden. The P2SBC register at offset 0xE0 has the
         * HIDE bit at bit 8 (= bit 0 of byte 0xE1). Blind-write 0
         * to byte 0xE1 via CF8/CFC to clear the HIDE bit.
         */
        kprint("ACPI: P2SB hidden, CF8 blind-write 0 to 0xE1 to unhide\n");
        pci_cf8_write8(bus, p2sb_slot, p2sb_func, p2sb_hide_hi_off, 0x00u);

        /* Small delay for hardware to settle */
        for (volatile int i = 0; i < 1000; i++) {}

        /* Read back VendorID via CF8 to confirm unhide worked */
        id = pci_cf8_read32(bus, p2sb_slot, p2sb_func, 0x00u);
        kprint("ACPI: P2SB post-unhide VendorID=0x%08x (CF8)\n", id);

        if (id != 0xFFFFFFFFu) {
            unhid = 1;
            g_p2sb_was_hidden = 1;
        } else {
            /*
             * Do not blindly write the full P2SBC dword. Firmware may lock
             * other control bits in this register; clobbering them can affect
             * unrelated sideband routing. Fall back to read-only BAR probing.
             */
            uint64_t guessed = pcr_probe_fallback_sbreg_bar(ioe_hint, iod_hint, iod_hint_alt);
            if (guessed) {
                kprint("ACPI: P2SB locked via CF8, using probed SBREG_BAR 0x%08x\n",
                       (uint32_t)guessed);
                return guessed;
            }
            kprint("ACPI: P2SB unhide failed via CF8, using SBREG_BAR_DEFAULT 0x%08x\n",
                   (uint32_t)SBREG_BAR_DEFAULT);
            return SBREG_BAR_DEFAULT;
        }
    }

    if ((id & 0xFFFFu) != 0x8086u) {
        kprint("ACPI: P2SB not Intel (0x%04x), using default\n", id & 0xFFFFu);
        if (unhid) {
            uint8_t p2sbc_hi = pci_cf8_read8(bus, p2sb_slot, p2sb_func, p2sb_hide_hi_off);
            pci_cf8_write8(bus, p2sb_slot, p2sb_func, p2sb_hide_hi_off,
                           (uint8_t)(p2sbc_hi | p2sb_hide_hi_mask));
        }
        return SBREG_BAR_DEFAULT;
    }

    /* Read BARs via CF8 while device is visible */
    uint32_t bar_lo = pci_cf8_read32(bus, p2sb_slot, p2sb_func, 0x10u);
    uint32_t bar_hi = pci_cf8_read32(bus, p2sb_slot, p2sb_func, 0x14u);
    kprint("ACPI: P2SB BAR0=0x%08x BAR1=0x%08x (CF8)\n", bar_lo, bar_hi);

    /* Re-hide P2SB if we unhid it */
    if (unhid) {
        uint8_t p2sbc_hi = pci_cf8_read8(bus, p2sb_slot, p2sb_func, p2sb_hide_hi_off);
        pci_cf8_write8(bus, p2sb_slot, p2sb_func, p2sb_hide_hi_off,
                       (uint8_t)(p2sbc_hi | p2sb_hide_hi_mask));
        kprint("ACPI: P2SB re-hidden (CF8)\n");
    }

    uint64_t bar = ((uint64_t)bar_hi << 32) | (uint64_t)(bar_lo & ~0xFULL);
    if (bar == 0 || bar == 0xFFFFFFFFFFFFFFF0ULL) {
        uint64_t guessed = pcr_probe_fallback_sbreg_bar(ioe_hint, iod_hint, iod_hint_alt);
        if (guessed) {
            kprint("ACPI: P2SB BAR invalid, using probed SBREG_BAR 0x%08x\n",
                   (uint32_t)guessed);
            return guessed;
        }
        kprint("ACPI: P2SB BAR invalid, using default\n");
        return SBREG_BAR_DEFAULT;
    }
    kprint("ACPI: P2SB SBREG_BAR=0x%llx\n", (unsigned long long)bar);
    return bar;
}

static void ec_mirror_pcr_ioe(uint8_t bus,
                              uint16_t ioe_val,
                              uint16_t iod_val,
                              uint16_t iod_alt_val) {
    uint64_t sbreg_bar = pcr_get_sbreg_bar(bus, ioe_val, iod_val, iod_alt_val);
    g_sbreg_bar_saved = sbreg_bar;
    uint8_t pid_cands[3];
    uint32_t pid_count = pcr_get_dmi_pid_candidates(pid_cands);
    uint8_t preferred_pid = pid_cands[0];
    uint8_t chosen_pid = pid_cands[0];
    uint32_t ioe_before = 0xFFFFFFFFu;
    uint32_t iod_before = 0xFFFFFFFFu;
    int best_score = -1000;

    for (uint32_t p = 0; p < pid_count; p++) {
        uint32_t ioe_try = 0xFFFFFFFFu;
        uint32_t iod_try = 0xFFFFFFFFu;
        uint8_t pid_try = pid_cands[p];
        int score = 0;
        pcr_read_dmi_lpc_regs(sbreg_bar, pid_try, &ioe_try, &iod_try);
        score = pcr_dmi_regs_score(ioe_try, iod_try, ioe_val, iod_val, iod_alt_val);
        kprint("ACPI: PCR PID probe[%u] pid=0x%02x IOE=0x%08x IOD=0x%08x score=%d\n",
               p, (uint32_t)pid_try, ioe_try, iod_try, score);
        if (score > best_score ||
            (score == best_score && pid_try == preferred_pid)) {
            /* Tie-break: prefer candidate[0] for this detected platform. */
            best_score = score;
            chosen_pid = pid_try;
            ioe_before = ioe_try;
            iod_before = iod_try;
        }
    }

    if (best_score < 0) {
        pcr_read_dmi_lpc_regs(sbreg_bar, chosen_pid, &ioe_before, &iod_before);
        kprint("ACPI: PCR PID fallback pid=0x%02x\n", (uint32_t)chosen_pid);
    } else {
        kprint("ACPI: PCR PID selected pid=0x%02x score=%d\n",
               (uint32_t)chosen_pid, best_score);
    }

    g_pcr_pid_used = chosen_pid;

    uint64_t ioe_phys = sbreg_bar + ((uint64_t)chosen_pid << 16) + PCR_DMI_LPCIOE;
    uint64_t iod_phys = sbreg_bar + ((uint64_t)chosen_pid << 16) + PCR_DMI_LPCIOD;

    kprint("ACPI: PCR target SBREG=0x%llx PID=0x%02x IOE_phys=0x%llx\n",
           (unsigned long long)sbreg_bar, (uint32_t)chosen_pid,
           (unsigned long long)ioe_phys);

    vmm_ensure_physmap_uc(ioe_phys + sizeof(uint32_t));
    vmm_ensure_physmap_uc(iod_phys + sizeof(uint32_t));

    volatile uint32_t *ioe_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + ioe_phys);
    volatile uint32_t *iod_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + iod_phys);

    uint32_t ioe_new = ioe_before | (uint32_t)ioe_val | IOE_KBC_60_64 | IOE_EC_62_66;

    /* Write IOE with memory fence to ensure commit */
    *ioe_reg = ioe_new;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t ioe_after = *ioe_reg;

    uint32_t iod_new = iod_before | (uint32_t)iod_val;
    *iod_reg = iod_new;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t iod_after = *iod_reg;

    g_pcr_ioe_before = ioe_before;
    g_pcr_ioe_after = ioe_after;
    g_pcr_mirror_done = 1;

    kprint("ACPI: PCR[DMI] IOE 0x%08x -write-> 0x%08x -readback-> 0x%08x %s\n",
           ioe_before, ioe_new, ioe_after,
           (ioe_after != ioe_before) ? "CHANGED" : "unchanged");
    kprint("ACPI: PCR[DMI] IOD 0x%08x -> 0x%08x\n", iod_before, iod_after);

    /* Verify EC port status immediately after mirror */
    uint8_t post_sts = inb(0x66u);
    kprint("ACPI: EC sts after PCR mirror: 0x%02x\n", (uint32_t)post_sts);
}

static void ec_enable_lpc_decode(void) {
    uint8_t bus = 0;
    uint8_t slot = 0x1Fu;
    uint8_t func = 0u;
    struct acpi_node *root = find_pci_root();
    if (root) {
        bus = (uint8_t)pci_get_base_bus(root);
    }

    pick_lpc_bridge_candidate(bus, &bus, &slot, &func);
    g_lpc_bus = bus;
    g_lpc_slot = slot;
    g_lpc_func = func;
    g_lpc_write_attempted = 0;
    g_lpc_write_performed = 0;

    uint32_t id = pci_ecam_read32(0, bus, slot, func, 0x00u);
    uint16_t vendor = (uint16_t)(id & 0xFFFFu);
    uint16_t device = (uint16_t)((id >> 16) & 0xFFFFu);
    g_lpc_vendor = vendor;
    g_lpc_device = device;
    if (id == 0xFFFFFFFFu || vendor == 0xFFFFu) {
        g_lpc_ioe_before = 0xFFFFu;
        g_lpc_ioe_after = 0xFFFFu;
        g_lpc_class_code = 0xFFu;
        g_lpc_subclass = 0xFFu;
        g_lpc_prog_if = 0xFFu;
        g_lpc_cmd = 0xFFFFu;
        g_lpc_reg80_before = 0xFFFFFFFFu;
        g_lpc_reg80_after = 0xFFFFFFFFu;
        g_lpc_reg84 = 0xFFFFFFFFu;
        g_lpc_reg88 = 0xFFFFFFFFu;
        g_lpc_reg8c = 0xFFFFFFFFu;
        g_lpc_reg90 = 0xFFFFFFFFu;
        kprint("ACPI: LPC/eSPI bridge absent at %u:%u.%u\n",
               (uint32_t)bus, (uint32_t)slot, (uint32_t)func);
        return;
    }

    uint32_t class_reg = pci_ecam_read32(0, bus, slot, func, 0x08u);
    uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
    uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
    uint8_t prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);
    g_lpc_class_code = class_code;
    g_lpc_subclass = subclass;
    g_lpc_prog_if = prog_if;

    uint32_t cmd_reg = pci_ecam_read32(0, bus, slot, func, 0x04u);
    g_lpc_cmd = (uint16_t)(cmd_reg & 0xFFFFu);

    uint32_t reg80 = pci_ecam_read32(0, bus, slot, func, 0x80u);
    uint32_t reg84 = pci_ecam_read32(0, bus, slot, func, 0x84u);
    uint32_t reg88 = pci_ecam_read32(0, bus, slot, func, 0x88u);
    uint32_t reg8c = pci_ecam_read32(0, bus, slot, func, 0x8Cu);
    uint32_t reg90 = pci_ecam_read32(0, bus, slot, func, 0x90u);
    g_lpc_reg80_before = reg80;
    g_lpc_reg80_after = reg80;
    g_lpc_reg84 = reg84;
    g_lpc_reg88 = reg88;
    g_lpc_reg8c = reg8c;
    g_lpc_reg90 = reg90;

    if (reg80 == 0xFFFFFFFFu) {
        g_lpc_ioe_before = 0xFFFFu;
        g_lpc_ioe_after = 0xFFFFu;
        kprint("ACPI: LPC IOE read failed at %u:%u.%u+0x82\n",
               (uint32_t)bus, (uint32_t)slot, (uint32_t)func);
        return;
    }

    uint16_t iod_val = (uint16_t)(reg80 & 0xFFFFu);
    uint16_t iod_alt = (reg88 == 0xFFFFFFFFu) ? 0u : (uint16_t)(reg88 & 0xFFFFu);
    uint16_t ioe_before = (uint16_t)((reg80 >> 16) & 0xFFFFu);
    uint16_t ioe_after = (uint16_t)(ioe_before | IOE_EC_62_66 | IOE_KBC_60_64);
    /* Ensure decode for EC 0x62/0x66 and KBC 0x60/0x64. */
    g_lpc_ioe_before = ioe_before;
    g_lpc_ioe_after = ioe_before;

    if (vendor != 0x8086u || class_code != 0x06u || subclass != 0x01u) {
        kprint("ACPI: skip LPC IOE write at %u:%u.%u ven=%04x dev=%04x cls=%02x/%02x/%02x\n",
               (uint32_t)bus, (uint32_t)slot, (uint32_t)func,
               (uint32_t)vendor, (uint32_t)device,
               (uint32_t)class_code, (uint32_t)subclass, (uint32_t)prog_if);
        kprint("ACPI: LPC cfg cmd=0x%04x 80=0x%08x 84=0x%08x 88=0x%08x\n",
               (uint32_t)g_lpc_cmd, g_lpc_reg80_before, g_lpc_reg84, g_lpc_reg88);
        return;
    }

    g_lpc_write_attempted = 1;
    if (ioe_after != ioe_before) {
        uint32_t new_reg80 = (reg80 & 0x0000FFFFu) | ((uint32_t)ioe_after << 16);
        pci_ecam_write32(0, bus, slot, func, 0x80u, new_reg80);
        g_lpc_write_performed = 1;
        reg80 = pci_ecam_read32(0, bus, slot, func, 0x80u);
        if (reg80 != 0xFFFFFFFFu) {
            g_lpc_reg80_after = reg80;
        } else {
            g_lpc_reg80_after = new_reg80;
        }
    }
    g_lpc_ioe_after = (uint16_t)((g_lpc_reg80_after >> 16) & 0xFFFFu);

    /*
     * Skip PCR sideband mirror when BIOS already has EC+KBC decode bits set.
     * On Intel 300-series PCH, PCI config IOE writes are auto-mirrored to
     * PCR[DMI] by hardware. Only mirror when we actually changed IOE in PCI
     * config space to avoid speculative sideband writes on uncertain PID paths.
     */
    if (g_lpc_write_performed) {
        ec_mirror_pcr_ioe(bus, g_lpc_ioe_after, iod_val, iod_alt);
    } else {
        kprint("ACPI: LPC IOE bits 10+11 already set by BIOS, skipping PCR mirror\n");
        g_pcr_mirror_done = 0;
        /* fry423: actually read PCR for diagnostics + get SBREG_BAR for eSPI probe */
        {
            uint64_t sbreg_bar = pcr_get_sbreg_bar(bus, ioe_before, iod_val, iod_alt);
            g_sbreg_bar_saved = sbreg_bar;
            uint8_t pid_cands[3];
            uint32_t pid_count = pcr_get_dmi_pid_candidates(pid_cands);
            uint8_t diag_pid = pid_cands[0];
            uint32_t pcr_ioe_rd = 0xFFFFFFFFu;
            uint32_t pcr_iod_rd = 0xFFFFFFFFu;
            int best_score = -1000;
            for (uint32_t p = 0; p < pid_count; p++) {
                uint32_t ioe_try = 0xFFFFFFFFu;
                uint32_t iod_try = 0xFFFFFFFFu;
                uint8_t pid_try = pid_cands[p];
                int score = 0;
                pcr_read_dmi_lpc_regs(sbreg_bar, pid_try, &ioe_try, &iod_try);
                score = pcr_dmi_regs_score(ioe_try, iod_try, ioe_before, iod_val, iod_alt);
                kprint("ACPI: PCR[DMI] diag PID probe[%u] pid=0x%02x IOE=0x%08x IOD=0x%08x score=%d\n",
                       p, (uint32_t)pid_try, ioe_try, iod_try, score);
                if (score > best_score ||
                    (score == best_score && pid_try == pid_cands[0])) {
                    best_score = score;
                    diag_pid = pid_try;
                    pcr_ioe_rd = ioe_try;
                    pcr_iod_rd = iod_try;
                }
            }
            g_pcr_ioe_before = pcr_ioe_rd;
            g_pcr_ioe_after = pcr_ioe_rd; /* same — no write */
            g_pcr_pid_used = diag_pid;
            kprint("ACPI: PCR[DMI] diag-read pid=0x%02x IOE=0x%08x IOD=0x%08x (no write)\n",
                   (uint32_t)diag_pid, pcr_ioe_rd, pcr_iod_rd);

            /* BIOS decode is already enabled; avoid speculative PCR writes. */
            if (pcr_ioe_rd == 0xFFFFFFFFu || pcr_ioe_rd == 0x00000000u) {
                kprint("ACPI: PCR[DMI] invalid read, skip mirror (BIOS decode already set)\n");
            }
        }
    }

    kprint("ACPI: LPC %u:%u.%u ven=%04x dev=%04x cls=%02x/%02x/%02x cmd=0x%04x\n",
           (uint32_t)bus, (uint32_t)slot, (uint32_t)func,
           (uint32_t)vendor, (uint32_t)device,
           (uint32_t)class_code, (uint32_t)subclass, (uint32_t)prog_if,
           (uint32_t)g_lpc_cmd);
    kprint("ACPI: LPC cfg 80 0x%08x -> 0x%08x (IOE 0x%04x -> 0x%04x) 84=0x%08x 88=0x%08x wr=%u/%u\n",
           g_lpc_reg80_before, g_lpc_reg80_after,
           (uint32_t)g_lpc_ioe_before, (uint32_t)g_lpc_ioe_after,
           g_lpc_reg84, g_lpc_reg88,
           (uint32_t)g_lpc_write_attempted, (uint32_t)g_lpc_write_performed);
    kprint("ACPI: LPC Gen3_DEC(8C)=0x%08x Gen4_DEC(90)=0x%08x\n",
           g_lpc_reg8c, g_lpc_reg90);
}

/*
 * fry423: Probe eSPI configuration for Coffee Lake-H PCH (HM370).
 *
 * The i5-8300H Dell Precision 7530 uses Coffee Lake-H with eSPI (not LPC)
 * for EC communication.  Even though LPC IOE has MC_EN set, I/O port
 * 0x62/0x66 cycles are forwarded over the eSPI bus.  If the eSPI bus is
 * misconfigured (wrong Gen 3/4 frequency, I/O mode mismatch, channel not
 * enabled), the EC won't respond — causing our floating bus symptom.
 *
 * Register offsets from Intel EDK2 CoffeelakeSiliconPkg/PchRegsLpc.h:
 *   0xDC (PCI cfg)  — PCBC, bit 2 = ESPI_EN strap
 *   0x4000 (PCR)    — SLV_CFG_REG_CTL (indirect slave register access)
 *   0x4004 (PCR)    — SLV_CFG_REG_DATA (slave register readback)
 *   0x4020 (PCR)    — PCERR_SLV0 (peripheral channel errors)
 *   0x4030 (PCR)    — VWERR_SLV0 (virtual wire errors)
 *   0x4040 (PCR)    — FCERR_SLV0 (flash channel errors)
 *   0x4050 (PCR)    — LNKERR_SLV0 (link errors, fatal/recovery)
 *   0xC00C (PCR)    — CFG_VAL (bit 0 = eSPI enabled strap)
 *
 * PID for eSPI PCR on Coffee Lake-H is uncertain (not Cannon Lake 0x72).
 * We try three candidates and pick the one that returns non-FF at 0xC00C.
 */
static uint32_t espi_pcr_read32(uint64_t sbreg, uint8_t pid, uint16_t offset) {
    uint64_t phys = sbreg + ((uint64_t)pid << 16) + offset;
    vmm_ensure_physmap_uc(phys + 4);
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + phys);
    return *reg;
}

static void espi_pcr_write32(uint64_t sbreg, uint8_t pid, uint16_t offset, uint32_t val) {
    uint64_t phys = sbreg + ((uint64_t)pid << 16) + offset;
    vmm_ensure_physmap_uc(phys + 4);
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + phys);
    *reg = val;
    __asm__ volatile("mfence" ::: "memory");
}

static int lpc_is_intel_300_series(uint16_t lpc_device) {
    /* 300-series LPC/eSPI IDs commonly live in the 0xA3xx range. */
    return (lpc_device & 0xFF00u) == 0xA300u;
}

static void espi_settle_delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) {}
}

static uint32_t espi_status_mask_for_off(uint16_t off) {
    switch (off) {
        case R_ESPI_PCR_PCERR_SLV0:  return ESPI_PCERR_STATUS_MASK;
        case R_ESPI_PCR_VWERR_SLV0:  return ESPI_VWERR_STATUS_MASK;
        case R_ESPI_PCR_FCERR_SLV0:  return ESPI_FCERR_STATUS_MASK;
        case R_ESPI_PCR_LNKERR_SLV0: return ESPI_LNKERR_STATUS_MASK;
        default:                     return 0;
    }
}

static uint32_t espi_status_bits(uint16_t off, uint32_t raw) {
    return raw & espi_status_mask_for_off(off);
}

static uint32_t espi_get_pid_candidates(uint8_t out[3]) {
    if (!out) return 0;
    /*
     * Coffee Lake / 300-series platforms usually use PID 0xC7 for eSPI.
     * Keep all three candidates, but bias ordering by detected PCH family.
     */
    if (lpc_is_intel_300_series(g_lpc_device)) {
        out[0] = PID_ESPI_B;
        out[1] = PID_ESPI_A;
        out[2] = PID_ESPI_C;
    } else {
        out[0] = PID_ESPI_A;
        out[1] = PID_ESPI_B;
        out[2] = PID_ESPI_C;
    }
    return 3;
}

static int espi_select_pid(uint8_t *pid_out, uint32_t *cfgval_out, int call_site_tag) {
    uint8_t pid_cands[3];
    uint32_t cand_count = espi_get_pid_candidates(pid_cands);
    uint32_t pcbc = pci_ecam_read32(0, g_lpc_bus, g_lpc_slot, g_lpc_func, R_ESPI_CFG_PCBC);
    uint32_t strap = (pcbc & B_ESPI_CFG_PCBC_ESPI_EN) ? 1u : 0u;
    int best_score = -1000000;
    uint8_t best_pid = pid_cands[0];
    uint32_t best_cfgval = 0xFFFFFFFFu;
    int found = 0;

    for (uint32_t i = 0; i < cand_count; i++) {
        uint8_t pid = pid_cands[i];
        uint32_t cfgval = espi_pcr_read32(g_sbreg_bar_saved, pid, R_ESPI_PCR_CFG_VAL);
        int score = 0;

        if (cfgval == 0xFFFFFFFFu || cfgval == 0x00000000u) {
            if (call_site_tag >= 0) {
                kprint("ACPI: espi_clear_errors[%d]: PID cand=0x%02x CFG_VAL=%08x invalid\n",
                       call_site_tag, (uint32_t)pid, cfgval);
            } else {
                kprint("ACPI: eSPI PID cand=0x%02x CFG_VAL=%08x invalid\n",
                       (uint32_t)pid, cfgval);
            }
            continue;
        }

        found = 1;
        score += 4;
        if ((cfgval & 1u) == strap) score += 8;
        else score -= 6;
        if ((cfgval & 1u) != 0) score += 1;
        if (lpc_is_intel_300_series(g_lpc_device) && pid == PID_ESPI_B) score += 6;
        if (!lpc_is_intel_300_series(g_lpc_device) && pid == PID_ESPI_A) score += 2;

        if (call_site_tag >= 0) {
            kprint("ACPI: espi_clear_errors[%d]: PID cand=0x%02x CFG_VAL=%08x score=%d\n",
                   call_site_tag, (uint32_t)pid, cfgval, score);
        } else {
            kprint("ACPI: eSPI PID cand=0x%02x CFG_VAL=%08x score=%d\n",
                   (uint32_t)pid, cfgval, score);
        }

        if (score > best_score) {
            best_score = score;
            best_pid = pid;
            best_cfgval = cfgval;
        }
    }

    if (!found) return 0;
    if (pid_out) *pid_out = best_pid;
    if (cfgval_out) *cfgval_out = best_cfgval;
    return 1;
}

/* fry435: Clear eSPI W1C error registers to unblock PC channel I/O cycles.
 * call_site: 0=proactive (before EC probe), 1=recovery (after all methods failed).
 * Runtime-gated: only proceeds if eSPI mode confirmed and SBREG_BAR available. */
static void espi_clear_errors(int call_site) {
    /* Guard: check PCBC bit 2 for eSPI mode */
    uint32_t pcbc = pci_ecam_read32(0, g_lpc_bus, g_lpc_slot, g_lpc_func, R_ESPI_CFG_PCBC);
    if (!(pcbc & B_ESPI_CFG_PCBC_ESPI_EN)) {
        kprint("ACPI: espi_clear_errors[%d]: LPC mode (not eSPI), skip\n", call_site);
        return;
    }

    /* Guard: need SBREG_BAR for PCR access */
    if (!g_sbreg_bar_saved) {
        kprint("ACPI: espi_clear_errors[%d]: no SBREG_BAR, skip\n", call_site);
        return;
    }

    uint8_t best_pid = 0;
    if (!espi_select_pid(&best_pid, 0, call_site)) {
        kprint("ACPI: espi_clear_errors[%d]: no valid PID found, skip\n", call_site);
        return;
    }

    /* Read current error registers */
    uint32_t pcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_PCERR_SLV0);
    uint32_t vwerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_VWERR_SLV0);
    uint32_t fcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_FCERR_SLV0);
    uint32_t lnkerr = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_LNKERR_SLV0);
    uint32_t pc_sts  = espi_status_bits(R_ESPI_PCR_PCERR_SLV0, pcerr);
    uint32_t vw_sts  = espi_status_bits(R_ESPI_PCR_VWERR_SLV0, vwerr);
    uint32_t fc_sts  = espi_status_bits(R_ESPI_PCR_FCERR_SLV0, fcerr);
    uint32_t ln_sts  = espi_status_bits(R_ESPI_PCR_LNKERR_SLV0, lnkerr);

    kprint("ACPI: espi_clear_errors[%d]: PID=0x%02x before: PC=%08x VW=%08x FC=%08x LN=%08x\n",
           call_site, (uint32_t)best_pid, pcerr, vwerr, fcerr, lnkerr);
    kprint("ACPI: espi_clear_errors[%d]: status bits: PC=%08x VW=%08x FC=%08x LN=%08x\n",
           call_site, pc_sts, vw_sts, fc_sts, ln_sts);

    /* Save pre-clear values on first call (preserve original baseline) */
    if (call_site == 0) {
        g_espi_pre_clear[0] = pcerr;
        g_espi_pre_clear[1] = vwerr;
        g_espi_pre_clear[2] = fcerr;
        g_espi_pre_clear[3] = lnkerr;
    }

    g_espi_clear_found = 0;
    if (pc_sts || vw_sts || fc_sts || ln_sts) {
        g_espi_clear_found = 1;

        for (int pass = 0; pass < ESPI_CLEAR_MAX_PASSES; pass++) {
            if (!(pc_sts || vw_sts || fc_sts || ln_sts)) break;

            /* W1C: write only documented status bits, never enable/default bits. */
            if (pc_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_PCERR_SLV0, pc_sts);
            if (vw_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_VWERR_SLV0, vw_sts);
            if (fc_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_FCERR_SLV0, fc_sts);
            if (ln_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_LNKERR_SLV0, ln_sts);

            /* Allow the eSPI slave/link state machine to settle before re-read. */
            espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS);

            /* Re-read to verify clear */
            pcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_PCERR_SLV0);
            vwerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_VWERR_SLV0);
            fcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_FCERR_SLV0);
            lnkerr = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_LNKERR_SLV0);
            pc_sts = espi_status_bits(R_ESPI_PCR_PCERR_SLV0, pcerr);
            vw_sts = espi_status_bits(R_ESPI_PCR_VWERR_SLV0, vwerr);
            fc_sts = espi_status_bits(R_ESPI_PCR_FCERR_SLV0, fcerr);
            ln_sts = espi_status_bits(R_ESPI_PCR_LNKERR_SLV0, lnkerr);
            kprint("ACPI: espi_clear_errors[%d]: pass=%u after: PC=%08x VW=%08x FC=%08x LN=%08x\n",
                   call_site, (uint32_t)(pass + 1), pcerr, vwerr, fcerr, lnkerr);
            kprint("ACPI: espi_clear_errors[%d]: pass=%u status: PC=%08x VW=%08x FC=%08x LN=%08x\n",
                   call_site, (uint32_t)(pass + 1), pc_sts, vw_sts, fc_sts, ln_sts);
        }

        kprint("ACPI: espi_clear_errors[%d]: final:  PC=%08x VW=%08x FC=%08x LN=%08x\n",
               call_site, pcerr, vwerr, fcerr, lnkerr);
        kprint("ACPI: espi_clear_errors[%d]: final status: PC=%08x VW=%08x FC=%08x LN=%08x\n",
               call_site, pc_sts, vw_sts, fc_sts, ln_sts);
        g_espi_clear_ok = (pc_sts == 0 && vw_sts == 0 && fc_sts == 0 && ln_sts == 0) ? 1 : 0;
    } else {
        kprint("ACPI: espi_clear_errors[%d]: no status-bit errors to clear\n", call_site);
        g_espi_clear_ok = 1;  /* no errors = clean */
    }

    /* Save post-clear values */
    g_espi_post_clear[0] = pcerr;
    g_espi_post_clear[1] = vwerr;
    g_espi_post_clear[2] = fcerr;
    g_espi_post_clear[3] = lnkerr;

    /* Set run bit for this call site */
    g_espi_clear_run |= (call_site == 0) ? 0x01 : 0x02;
}

/*
 * espi_slave_read_indirect — read a slave register via SLV_CFG_CTL/DATA.
 * Returns 1 on success (SCRS != 0), 0 on timeout.
 * On success *val_out receives the slave register data.
 */
static int espi_slave_read_indirect(uint64_t sbreg, uint8_t pid,
                                    uint16_t slave_addr, uint32_t *val_out) {
    uint64_t ctl_phys = sbreg + ((uint64_t)pid << 16) + R_ESPI_PCR_SLV_CFG_CTL;
    uint64_t dat_phys = sbreg + ((uint64_t)pid << 16) + R_ESPI_PCR_SLV_CFG_DATA;
    vmm_ensure_physmap_uc(ctl_phys + 4);
    vmm_ensure_physmap_uc(dat_phys + 4);
    volatile uint32_t *ctl_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + ctl_phys);
    volatile uint32_t *dat_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + dat_phys);

    /* Issue read: SCRE=1 (bit 31), SCRT=00 read (bits 17:16), SID=00 slave 0, addr [11:0] */
    uint32_t cmd = (1u << 31) | (0u << 16) | (slave_addr & 0xFFFu);
    *ctl_reg = cmd;
    __asm__ volatile("mfence" ::: "memory");

    /* Poll for completion: SCRS (bits 30:28) nonzero = done */
    uint32_t ctl_rb = 0;
    for (int i = 0; i < 5000; i++) {
        ctl_rb = *ctl_reg;
        if (((ctl_rb >> 28) & 0x7u) != 0) {
            if (val_out) *val_out = *dat_reg;
            return 1;
        }
        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
    }
    if (val_out) *val_out = 0xFFFFFFFFu;
    return 0;
}

/*
 * espi_slave_write_indirect — write a slave register via SLV_CFG_CTL/DATA.
 *
 * Issues a SET_CONFIGURATION command on the eSPI bus.
 * The PCH master sends an in-band SET_CONFIGURATION transaction with the
 * provided register address and data value. The slave processes it and
 * updates its internal register.
 *
 * SLV_CFG_CTL bit layout (Intel PCH datasheet):
 *   [11:0]  Slave register address (12-bit)
 *   [15:12] Reserved (write 0)
 *   [17:16] SCRT: 00=Read, 01=Write, 10/11=Reserved
 *   [23:18] Reserved
 *   [27:24] SID: Slave ID (0 for slave 0)
 *   [30:28] SCRS: Completion status (read-back):
 *           000 = Not started / not complete
 *           001 = Complete with ACCEPT (success)
 *           010 = Complete with DEFER/WAIT
 *           011 = Complete with NON-FATAL ERROR
 *           1xx = Complete with FATAL ERROR
 *   [31]    SCRE: 1 = Start the transaction
 *
 * Returns: 1 on success (SCRS==ACCEPT), 0 on timeout, -1 on error response.
 * On error, *scrs_out (if non-NULL) receives the SCRS code.
 */
static int espi_slave_write_indirect(uint64_t sbreg, uint8_t pid,
                                     uint16_t slave_addr, uint32_t val,
                                     uint32_t *scrs_out) {
    uint64_t ctl_phys = sbreg + ((uint64_t)pid << 16) + R_ESPI_PCR_SLV_CFG_CTL;
    uint64_t dat_phys = sbreg + ((uint64_t)pid << 16) + R_ESPI_PCR_SLV_CFG_DATA;
    vmm_ensure_physmap_uc(ctl_phys + 4);
    vmm_ensure_physmap_uc(dat_phys + 4);
    volatile uint32_t *ctl_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + ctl_phys);
    volatile uint32_t *dat_reg = (volatile uint32_t *)(uintptr_t)(VMM_PHYSMAP_BASE + dat_phys);

    /* Step 1: Write data to SLV_CFG_DATA before issuing the command */
    *dat_reg = val;
    __asm__ volatile("mfence" ::: "memory");

    /* Step 2: Issue write command: SCRE=1, SCRT=01 (write), SID=00, addr */
    uint32_t cmd = (1u << 31) | (1u << 16) | (slave_addr & 0xFFFu);
    *ctl_reg = cmd;
    __asm__ volatile("mfence" ::: "memory");

    /* Step 3: Poll for completion — SCRS (bits 30:28) becomes nonzero */
    for (int i = 0; i < 5000; i++) {
        uint32_t ctl_rb = *ctl_reg;
        uint32_t scrs = (ctl_rb >> 28) & 0x7u;
        if (scrs != 0) {
            if (scrs_out) *scrs_out = scrs;
            /* SCRS=1 is ACCEPT (success), anything else is an error */
            return (scrs == 1) ? 1 : -1;
        }
        /* I/O port 0x80 delay (~1us per iteration) */
        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
    }

    if (scrs_out) *scrs_out = 0;
    return 0; /* timeout */
}

/*
 * espi_channel_init — bring up eSPI Peripheral Channel and Virtual Wire
 * Channel so that EC I/O cycles on 0x62/0x66 are actually tunneled to the
 * Embedded Controller.
 *
 * ROOT CAUSE (fry445 research): The Dell Precision 7530's EC has rdy=0 on
 * both PC and VW channels. Without PCRDY, the PCH does not tunnel I/O cycles
 * to the EC via eSPI, so reads from 0x62/0x66 return 0xFF (floating bus).
 *
 * Strategy based on Zephyr espi_mchp_xec_v2.c analysis of the EC firmware's
 * channel ready sequence:
 *   1. EC firmware waits for PLT_RST# de-assertion (Virtual Wire)
 *   2. EC firmware configures I/O BARs (maps 0x62/0x66 to internal regs)
 *   3. Host sends SET_CONFIGURATION enabling PC channel (channel enable bit)
 *   4. EC firmware detects EN_CHG (enable change) interrupt
 *   5. EC firmware asserts PCRDY=1 in response
 *   6. Similarly for VW channel: enable → VWRDY=1
 *
 * This function:
 *   - Dumps all master-to-slave and slave-to-master VW registers (diagnostic)
 *   - Checks PLT_RST# state; if asserted, attempts de-assertion
 *   - Reads all slave capability registers (GenCfg, PC, VW, OOB, FC)
 *   - If channels not ready: toggles channel enable (disable→wait→enable)
 *     to re-trigger the EC's EN_CHG interrupt handler
 *   - Retries up to ESPI_CHINIT_MAX_RETRIES times with increasing delays
 *   - Logs everything to serial for bare-metal debug
 *
 * Hardware-agnostic per Rule 14: uses standard eSPI slave register addresses
 * per the eSPI Base Spec. PCR access is Intel-specific but gated behind
 * the existing Intel PCH detection in ec_enable_lpc_decode().
 *
 * Must be called AFTER ec_enable_lpc_decode() sets up g_sbreg_bar_saved
 * and BEFORE espi_clear_errors() / EC probe attempts.
 */
static void espi_channel_init(void) {
    /* ---- Prerequisites ---- */
    uint32_t pcbc = pci_ecam_read32(0, g_lpc_bus, g_lpc_slot, g_lpc_func,
                                    R_ESPI_CFG_PCBC);
    if (!(pcbc & B_ESPI_CFG_PCBC_ESPI_EN)) {
        kprint("ACPI: espi_chinit: LPC mode (not eSPI), skip\n");
        return;
    }
    if (!g_sbreg_bar_saved) {
        kprint("ACPI: espi_chinit: no SBREG_BAR available, skip\n");
        return;
    }

    uint8_t pid = 0;
    if (!espi_select_pid(&pid, 0, -1)) {
        kprint("ACPI: espi_chinit: no valid eSPI PID found, skip\n");
        return;
    }

    kprint("ACPI: espi_chinit: PID=0x%02x — starting eSPI channel bring-up\n",
           (uint32_t)pid);

    /* ---- Step 1: Scan Master-to-Slave VW transmit registers ---- */
    int pltrst_reg_idx = -1;
    uint32_t pltrst_reg_val = 0;
    int all_mvw_zero = 1;

    for (uint32_t i = 0; i < ESPI_VW_REG_COUNT; i++) {
        uint16_t off = R_ESPI_PCR_XMVW_BASE + (uint16_t)(i * 4);
        uint32_t vw = espi_pcr_read32(g_sbreg_bar_saved, pid, off);
        g_espi_mvw[i] = vw;
        if (vw) all_mvw_zero = 0;

        if ((vw & 0xFFu) == ESPI_VW_IDX_PLTRST) {
            pltrst_reg_idx = (int)i;
            pltrst_reg_val = vw;
        }
    }

    kprint("ACPI: espi_chinit: XMVW scan: all_zero=%d pltrst_reg=%d\n",
           all_mvw_zero, pltrst_reg_idx);

    /* ---- Step 1b: Program XMVW if firmware left them empty ---- */
    /*
     * If UEFI handed off without programming any VW indices, the EC never
     * received PLT_RST# deassert or SLP_Sx# state. Program the standard
     * eSPI system VW indices so the EC knows the platform is awake.
     *
     * This is hardware-agnostic: VW index assignments are per the eSPI
     * Base Spec Rev 1.6 Table 5, not vendor-specific. PCR access is
     * already gated behind Intel PCH detection.
     *
     * XMVW register format (Intel 300-series PCH):
     *   [7:0]   VW Index number
     *   [11:8]  Data bits (wire 3..0)
     *   [15:12] Valid/enable bits (wire 3..0)
     *
     * Active-low signals: data=1 = deasserted (normal running state).
     */
    if (all_mvw_zero) {
        kprint("ACPI: espi_chinit: XMVW all zero — programming standard VW indices\n");

        /* XMVW0: Index 2 — SLP_S3#[w0]=1, SLP_S4#[w1]=1, SLP_S5#[w2]=1 */
        uint32_t mvw_slp = 0x02u          /* idx = 2 */
                         | (0x7u << 8)    /* data: w0=1 w1=1 w2=1 (deasserted) */
                         | (0x7u << 12);  /* valid: w0=1 w1=1 w2=1 */
        espi_pcr_write32(g_sbreg_bar_saved, pid,
                         R_ESPI_PCR_XMVW_BASE + 0, mvw_slp);

        /* XMVW1: Index 4 — OOB_RST_WARN[w0]=1, PLT_RST#[w1]=1 */
        uint32_t mvw_rst = ESPI_VW_IDX_PLTRST  /* idx = 4 */
                         | (0x3u << 8)          /* data: w0=1 w1=1 (deasserted) */
                         | (0x3u << 12);        /* valid: w0=1 w1=1 */
        espi_pcr_write32(g_sbreg_bar_saved, pid,
                         R_ESPI_PCR_XMVW_BASE + 4, mvw_rst);

        /* XMVW2: Index 7 — HOST_C10[w0]=0 (not in C10 state) */
        uint32_t mvw_host = 0x07u         /* idx = 7 */
                          | (0x0u << 8)   /* data: all wires low */
                          | (0x1u << 12); /* valid: w0 only */
        espi_pcr_write32(g_sbreg_bar_saved, pid,
                         R_ESPI_PCR_XMVW_BASE + 8, mvw_host);

        /* Allow PCH to transmit VWs and EC firmware to process PLT_RST# */
        espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS * 5u); /* ~10ms */

        /* Re-read all XMVW to verify and update tracking */
        for (uint32_t i = 0; i < ESPI_VW_REG_COUNT; i++) {
            uint16_t off = R_ESPI_PCR_XMVW_BASE + (uint16_t)(i * 4);
            g_espi_mvw[i] = espi_pcr_read32(g_sbreg_bar_saved, pid, off);
        }
        pltrst_reg_idx = 1; /* we put PLT_RST# in XMVW1 */
        pltrst_reg_val = g_espi_mvw[1];
        g_espi_pltrst_sent = 1;

        kprint("ACPI: espi_chinit: VW programmed: XMVW0=0x%08x XMVW1=0x%08x XMVW2=0x%08x\n",
               g_espi_mvw[0], g_espi_mvw[1], g_espi_mvw[2]);
    }

    /* ---- Step 2: Scan Slave-to-Master VW receive registers ---- */
    int any_svw_nonzero = 0;
    for (uint32_t i = 0; i < ESPI_VW_REG_COUNT; i++) {
        uint16_t off = R_ESPI_PCR_XSVW_BASE + (uint16_t)(i * 4);
        uint32_t vw = espi_pcr_read32(g_sbreg_bar_saved, pid, off);
        g_espi_svw[i] = vw;
        if (vw) any_svw_nonzero = 1;
    }
    kprint("ACPI: espi_chinit: XSVW scan: any_nonzero=%d\n", any_svw_nonzero);

    /* ---- Step 3: Check PLT_RST# state ---- */
    if (pltrst_reg_idx >= 0) {
        /*
         * PLT_RST# is wire 1 in VW Index 4.
         * Data = bit 9, Valid = bit 13.
         * Active-low: data=0 means asserted (EC in reset).
         */
        uint32_t pltrst_data  = (pltrst_reg_val >> (8 + ESPI_VW_PLTRST_WIRE)) & 1u;
        uint32_t pltrst_valid = (pltrst_reg_val >> (12 + ESPI_VW_PLTRST_WIRE)) & 1u;
        g_espi_pltrst_state = (uint8_t)((pltrst_valid << 1) | pltrst_data);

        kprint("ACPI: espi_chinit: PLT_RST# found in XMVW%d: data=%u valid=%u → %s\n",
               pltrst_reg_idx, pltrst_data, pltrst_valid,
               pltrst_data ? "de-asserted (normal)" : "ASSERTED (EC held in reset!)");

        if (!pltrst_data) {
            /*
             * PLT_RST# is asserted — the EC is being held in reset state.
             * Attempt to de-assert by setting data bit high and marking valid.
             * Also de-assert OOB_RST_WARN (wire 0) which the EC expects first.
             *
             * NOTE: On Intel PCH, these VW registers may be hardware-controlled
             * and read-only from software. The write may have no effect, but it's
             * safe to try. We'll verify via readback.
             */
            kprint("ACPI: espi_chinit: attempting PLT_RST# de-assertion...\n");

            /* Build new value with all 4 wires de-asserted and valid */
            uint32_t new_val = pltrst_reg_val;
            /* Set data bits for wire 0 (OOB_RST_WARN) and wire 1 (PLT_RST#) */
            new_val |= (1u << 8) | (1u << 9);
            /* Set valid bits for wire 0 and wire 1 */
            new_val |= (1u << 12) | (1u << 13);

            espi_pcr_write32(g_sbreg_bar_saved, pid,
                            R_ESPI_PCR_XMVW_BASE + (uint16_t)(pltrst_reg_idx * 4),
                            new_val);

            /* Give the EC firmware time to process PLT_RST de-assertion. */
            espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS * 3u); /* ~6ms */

            /* Readback to verify */
            uint32_t verify = espi_pcr_read32(g_sbreg_bar_saved, pid,
                                              R_ESPI_PCR_XMVW_BASE +
                                              (uint16_t)(pltrst_reg_idx * 4));
            uint32_t vfy_data = (verify >> (8 + ESPI_VW_PLTRST_WIRE)) & 1u;

            kprint("ACPI: espi_chinit: PLT_RST# after write: XMVW%d=0x%08x data=%u %s\n",
                   pltrst_reg_idx, verify, vfy_data,
                   vfy_data ? "(de-asserted OK)" : "(still asserted — HW-controlled)");

            g_espi_pltrst_sent = 1;
            g_espi_pltrst_state = (uint8_t)(((verify >> (12 + ESPI_VW_PLTRST_WIRE)) & 1u) << 1)
                                  | (uint8_t)vfy_data;
        }
    } else {
        kprint("ACPI: espi_chinit: PLT_RST# VW Index %u not found in any XMVW register\n",
               ESPI_VW_IDX_PLTRST);
        g_espi_pltrst_state = 0xFFu;
    }

    /* ---- Step 4: Read all slave capability registers ---- */
    uint32_t gen_cfg = 0, pc_cap = 0, vw_cap = 0, oob_cap = 0, fc_cap = 0;
    int gc_ok  = espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_GEN_CFG,  &gen_cfg);
    int pc_ok  = espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_CH0_CAP,  &pc_cap);
    int vw_ok  = espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_CH1_CAP,  &vw_cap);
    int oob_ok = espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_CH2_CAP,  &oob_cap);
    int fc_ok  = espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_CH3_CAP,  &fc_cap);
    (void)oob_ok; (void)fc_ok; (void)oob_cap; (void)fc_cap;

    kprint("ACPI: espi_chinit: slave: GenCfg=0x%08x PC=0x%08x(rdy=%u) VW=0x%08x(rdy=%u)\n",
           gen_cfg, pc_cap, (pc_cap >> 16) & 1u, vw_cap, (vw_cap >> 16) & 1u);

    /* ---- Step 4b: Try enabling VW in slave GenCfg ---- */
    /*
     * Some ECs (e.g. Microchip MEC on Dell) don't report VW support in
     * GenCfg until after PLT_RST# deassert, but have real VW_CAP hardware.
     * Attempt to set the VW support bit in GenCfg via SET_CONFIGURATION.
     * This is safe: if the slave rejects it, we just proceed with PC-only.
     */
    if (gc_ok && !(gen_cfg & (1u << 13)) && vw_ok && vw_cap != 0xFFFFFFFFu) {
        uint32_t new_gencfg = gen_cfg | (1u << 13);
        uint32_t scrs = 0;
        espi_slave_write_indirect(g_sbreg_bar_saved, pid,
                                  ESPI_SLAVE_GEN_CFG, new_gencfg, &scrs);
        espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS * 2u);
        espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_GEN_CFG, &gen_cfg);
        kprint("ACPI: espi_chinit: GenCfg VW enable attempt: now=0x%08x VW=%u\n",
               gen_cfg, (gen_cfg >> 13) & 1u);
    }

    /* ---- Step 5: Check if channels are already ready ---- */
    if ((pc_cap & B_ESPI_SLV_CH_READY) && (vw_cap & B_ESPI_SLV_CH_READY)) {
        kprint("ACPI: espi_chinit: both channels already READY\n");
        g_espi_chinit_result = 1;
        g_espi_chinit_pc_cap = pc_cap;
        g_espi_chinit_vw_cap = vw_cap;
        return;
    }

    /* ---- Step 6: Toggle channel enables with retries ---- */
    /*
     * Generate EN_CHG events by toggling enable bits. This should trigger
     * the EC firmware's channel ready path. We also try VW toggle even if
     * GenCfg doesn't report VW support — some ECs have VW_CAP hardware
     * but don't advertise it in GenCfg until fully initialized.
     */
    for (uint32_t attempt = 0; attempt < ESPI_CHINIT_MAX_RETRIES; attempt++) {
        uint32_t delay = ESPI_CHINIT_BASE_DELAY * (attempt + 1u);
        g_espi_chinit_retries = (uint8_t)(attempt + 1);

        /* Toggle PC channel if capability read succeeded and not yet ready */
        if (pc_ok && (gen_cfg & (1u << 12)) && !(pc_cap & B_ESPI_SLV_CH_READY)) {
            uint32_t scrs_d = 0, scrs_e = 0;
            espi_slave_write_indirect(g_sbreg_bar_saved, pid,
                                      ESPI_SLAVE_CH0_CAP, pc_cap & ~B_ESPI_SLV_CH_EN, &scrs_d);
            espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS * 2u);
            espi_slave_write_indirect(g_sbreg_bar_saved, pid,
                                      ESPI_SLAVE_CH0_CAP, pc_cap | B_ESPI_SLV_CH_EN, &scrs_e);
        }

        /* Toggle VW channel — try regardless of GenCfg VW support bit */
        if (vw_ok && !(vw_cap & B_ESPI_SLV_CH_READY)) {
            uint32_t scrs_d = 0, scrs_e = 0;
            espi_slave_write_indirect(g_sbreg_bar_saved, pid,
                                      ESPI_SLAVE_CH1_CAP, vw_cap & ~B_ESPI_SLV_CH_EN, &scrs_d);
            espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS * 2u);
            espi_slave_write_indirect(g_sbreg_bar_saved, pid,
                                      ESPI_SLAVE_CH1_CAP, vw_cap | B_ESPI_SLV_CH_EN, &scrs_e);
        }

        espi_settle_delay(delay);

        /* Readback channel status */
        espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_CH0_CAP, &pc_cap);
        espi_slave_read_indirect(g_sbreg_bar_saved, pid, ESPI_SLAVE_CH1_CAP, &vw_cap);

        uint32_t pc_rdy = (pc_cap & B_ESPI_SLV_CH_READY) ? 1u : 0u;
        uint32_t vw_rdy = (vw_cap & B_ESPI_SLV_CH_READY) ? 1u : 0u;

        kprint("ACPI: espi_chinit: toggle %u/%u PC_rdy=%u VW_rdy=%u\n",
               attempt + 1, ESPI_CHINIT_MAX_RETRIES, pc_rdy, vw_rdy);

        if (pc_rdy && vw_rdy) {
            kprint("ACPI: espi_chinit: SUCCESS after %u toggle(s)\n", attempt + 1);
            g_espi_chinit_result = 2;
            g_espi_chinit_pc_cap = pc_cap;
            g_espi_chinit_vw_cap = vw_cap;
            return;
        }
    }

    /* ---- Step 7: All retries exhausted — log final state ---- */
    uint32_t final_pc_rdy = (pc_cap & B_ESPI_SLV_CH_READY) ? 1u : 0u;
    uint32_t final_vw_rdy = (vw_cap & B_ESPI_SLV_CH_READY) ? 1u : 0u;

    g_espi_chinit_pc_cap = pc_cap;
    g_espi_chinit_vw_cap = vw_cap;

    if (final_pc_rdy || final_vw_rdy) {
        g_espi_chinit_result = 3; /* partial */
        kprint("ACPI: espi_chinit: PARTIAL — PC_rdy=%u VW_rdy=%u after %u retries\n",
               final_pc_rdy, final_vw_rdy, ESPI_CHINIT_MAX_RETRIES);
    } else {
        g_espi_chinit_result = 4; /* failed */
        kprint("ACPI: espi_chinit: FAILED — both channels still not ready after %u retries\n",
               ESPI_CHINIT_MAX_RETRIES);
        kprint("ACPI: espi_chinit: EC firmware may require proprietary initialization\n");
        kprint("ACPI: espi_chinit: (encrypted Dell EC firmware, PLT_RST may be HW-controlled)\n");
    }

    /* Re-read slave-to-master VWs post-init — only log changes */
    int svw_changes = 0;
    for (uint32_t i = 0; i < ESPI_VW_REG_COUNT; i++) {
        uint16_t off = R_ESPI_PCR_XSVW_BASE + (uint16_t)(i * 4);
        uint32_t vw = espi_pcr_read32(g_sbreg_bar_saved, pid, off);
        if (vw != g_espi_svw[i]) {
            kprint("ACPI: espi_chinit: XSVW%u changed 0x%08x->0x%08x\n",
                   i, g_espi_svw[i], vw);
            svw_changes++;
        }
        g_espi_svw[i] = vw;
    }

    uint8_t final_sts = inb(0x66u);
    kprint("ACPI: espi_chinit: done: port66=0x%02x svw_changes=%d\n",
           (uint32_t)final_sts, svw_changes);
}

static void espi_diag_probe(void) {
    /*
     * Step 1: Check ESPI_EN strap in PCI config space (no PCR needed).
     * D31:F0 offset 0xDC = PCBC (Peripheral Channel BIOS Control).
     * Bit 2 = ESPI_EN pin strap: 1 = eSPI mode, 0 = LPC mode.
     */
    g_espi_pcbc = pci_ecam_read32(0, g_lpc_bus, g_lpc_slot, g_lpc_func, R_ESPI_CFG_PCBC);
    g_espi_en_strap = (g_espi_pcbc & B_ESPI_CFG_PCBC_ESPI_EN) ? 1 : 0;
    kprint("ACPI: eSPI PCBC(0xDC)=0x%08x ESPI_EN=%u\n",
           g_espi_pcbc, (uint32_t)g_espi_en_strap);

    if (!g_sbreg_bar_saved) {
        kprint("ACPI: eSPI PCR probe skipped — no SBREG_BAR\n");
        return;
    }

    /*
     * Keep eSPI diagnostics read-only by default. Writing uncertain sideband
     * registers before EC bring-up can destabilize firmware-controlled flows.
     */
    const int espi_safe_read_only = 0;  /* fry435: safe now — runs after all EC work */

    /*
     * Step 2: Find correct PID for eSPI PCR registers.
     * Read R_ESPI_PCR_CFG_VAL (0xC00C) at each candidate PID.
     * The correct PID returns a non-0xFFFFFFFF value with bit 0 = eSPI strap.
     */
    uint8_t best_pid = 0;
    uint32_t best_cfgval = 0xFFFFFFFFu;
    g_espi_pid = 0;

    if (!espi_select_pid(&best_pid, &best_cfgval, -1)) {
        kprint("ACPI: eSPI PCR — no valid PID found (all CFG_VAL=FF or 00)\n");
        g_espi_probed = 1;
        /* Store zeros to indicate "probed but nothing found" */
        for (int i = 0; i < 8; i++) g_espi_raw[i] = 0xFFFFFFFFu;
        return;
    }
    g_espi_pid = best_pid;
    kprint("ACPI: eSPI using PID=0x%02x CFG_VAL=0x%08x (strap=%u)\n",
           (uint32_t)best_pid, best_cfgval,
           (best_cfgval & 1u) ? 1u : 0u);

    /*
     * Step 3: Read error registers and link status.
     * These tell us if the eSPI bus has communication problems.
     */
    uint32_t pcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_PCERR_SLV0);
    uint32_t vwerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_VWERR_SLV0);
    uint32_t fcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_FCERR_SLV0);
    uint32_t lnkerr = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_LNKERR_SLV0);
    uint32_t slvcfg_ctl = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_SLV_CFG_CTL);

    kprint("ACPI: eSPI PCERR=0x%08x VWERR=0x%08x FCERR=0x%08x\n",
           pcerr, vwerr, fcerr);
    kprint("ACPI: eSPI LNKERR=0x%08x SLV_CFG_CTL=0x%08x\n",
           lnkerr, slvcfg_ctl);

    /* Do not clear sticky errors while running in read-only diagnostic mode. */
    uint32_t pc_sts = espi_status_bits(R_ESPI_PCR_PCERR_SLV0, pcerr);
    uint32_t vw_sts = espi_status_bits(R_ESPI_PCR_VWERR_SLV0, vwerr);
    uint32_t fc_sts = espi_status_bits(R_ESPI_PCR_FCERR_SLV0, fcerr);
    uint32_t ln_sts = espi_status_bits(R_ESPI_PCR_LNKERR_SLV0, lnkerr);

    if (pc_sts || vw_sts || fc_sts || ln_sts) {
        if (espi_safe_read_only) {
            kprint("ACPI: eSPI safe mode: skip error-clear writes\n");
        } else {
            if (pc_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_PCERR_SLV0, pc_sts);
            if (vw_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_VWERR_SLV0, vw_sts);
            if (fc_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_FCERR_SLV0, fc_sts);
            if (ln_sts) espi_pcr_write32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_LNKERR_SLV0, ln_sts);

            pcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_PCERR_SLV0);
            vwerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_VWERR_SLV0);
            fcerr  = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_FCERR_SLV0);
            lnkerr = espi_pcr_read32(g_sbreg_bar_saved, best_pid, R_ESPI_PCR_LNKERR_SLV0);
            kprint("ACPI: eSPI status-bit clear -> PCERR=0x%08x VWERR=0x%08x FCERR=0x%08x LNKERR=0x%08x\n",
                   pcerr, vwerr, fcerr, lnkerr);
        }
    }

    /* Store in diagnostic array:
     * [0] = CFG_VAL, [1] = PCBC, [2] = PCERR, [3] = VWERR,
     * [4] = FCERR, [5] = LNKERR, [6] = SLV_CFG_CTL, [7] = SLV_CFG_DATA
     */
    g_espi_raw[0] = best_cfgval;
    g_espi_raw[1] = g_espi_pcbc;
    g_espi_raw[2] = pcerr;
    g_espi_raw[3] = vwerr;
    g_espi_raw[4] = fcerr;
    g_espi_raw[5] = lnkerr;
    g_espi_raw[6] = slvcfg_ctl;

    /*
     * Step 4: Indirect slave register reads via SLV_CFG_CTL/DATA.
     *
     * eSPI slave register layout (eSPI spec):
     *   0x0008 General/Device Config:
     *     [2:0]   Operating Frequency (000=20M 001=25M 010=33M 011=50M 100=66M)
     *     [4:3]   I/O Mode (00=Single 01=S+Dual 10=S+Quad 11=S+D+Q)
     *     [12]    Peripheral Channel Supported
     *     [13]    Virtual Wire Channel Supported
     *     [14]    OOB Channel Supported
     *     [15]    Flash Access Channel Supported
     *   0x0010 Peripheral Channel Cap & Config:
     *     [2:0]   Max Payload Size Supported
     *     [3]     Max Payload Size Selected
     *     [6:4]   Max Read Request Size
     *     [16]    Peripheral Channel Ready (slave asserts when PC path is up)
     *   0x0020 Virtual Wire Cap & Config:
     *     [5:0]   Max VW Count Supported
     *     [16]    Virtual Wire Channel Ready
     */
    if (!espi_safe_read_only) {
        /* Read General Config (0x0008) */
        uint32_t gen_cfg = 0xFFFFFFFFu;
        int gc_ok = espi_slave_read_indirect(g_sbreg_bar_saved, best_pid,
                                             ESPI_SLAVE_GEN_CFG, &gen_cfg);
        g_espi_raw[7] = gen_cfg;
        if (gc_ok) {
            g_espi_slave_read_ok |= 0x01;
            uint32_t freq = gen_cfg & 0x7u;
            uint32_t io_mode = (gen_cfg >> 3) & 0x3u;
            g_espi_gen_chan_sup = (uint8_t)((gen_cfg >> 12) & 0xFu);
            const char *freq_str = "?";
            switch (freq) {
                case 0: freq_str = "20MHz"; break;
                case 1: freq_str = "25MHz"; break;
                case 2: freq_str = "33MHz(G3)"; break;
                case 3: freq_str = "50MHz"; break;
                case 4: freq_str = "66MHz(G4)"; break;
            }
            const char *io_str = "?";
            switch (io_mode) {
                case 0: io_str = "Single"; break;
                case 1: io_str = "S+Dual"; break;
                case 2: io_str = "S+Quad"; break;
                case 3: io_str = "S+D+Q"; break;
            }
            kprint("ACPI: eSPI slave GenCfg=0x%08x freq=%s io=%s ch_sup=PC%u VW%u OOB%u FC%u\n",
                   gen_cfg, freq_str, io_str,
                   (gen_cfg >> 12) & 1u, (gen_cfg >> 13) & 1u,
                   (gen_cfg >> 14) & 1u, (gen_cfg >> 15) & 1u);
        } else {
            kprint("ACPI: eSPI slave GenCfg read timeout\n");
        }

        /* Read Peripheral Channel Cap & Config (0x0010) */
        uint32_t pc_cap = 0xFFFFFFFFu;
        int pc_ok = espi_slave_read_indirect(g_sbreg_bar_saved, best_pid,
                                             ESPI_SLAVE_CH0_CAP, &pc_cap);
        g_espi_slave_pc_cap = pc_cap;
        if (pc_ok) {
            g_espi_slave_read_ok |= 0x02;
            g_espi_slave_pc_en = (pc_cap >> 16) & 1u;  /* PC Ready bit */
            kprint("ACPI: eSPI slave PC_CAP=0x%08x ready=%u maxPL=%u maxRR=%u\n",
                   pc_cap, g_espi_slave_pc_en,
                   pc_cap & 0x7u, (pc_cap >> 4) & 0x7u);
        } else {
            kprint("ACPI: eSPI slave PC_CAP read timeout\n");
        }

        /* Read Virtual Wire Channel Cap & Config (0x0020) */
        uint32_t vw_cap = 0xFFFFFFFFu;
        int vw_ok = espi_slave_read_indirect(g_sbreg_bar_saved, best_pid,
                                             ESPI_SLAVE_CH1_CAP, &vw_cap);
        g_espi_slave_vw_cap = vw_cap;
        if (vw_ok) {
            g_espi_slave_read_ok |= 0x04;
            g_espi_slave_vw_en = (vw_cap >> 16) & 1u;  /* VW Ready bit */
            kprint("ACPI: eSPI slave VW_CAP=0x%08x ready=%u maxVW=%u\n",
                   vw_cap, g_espi_slave_vw_en,
                   vw_cap & 0x3Fu);
        } else {
            kprint("ACPI: eSPI slave VW_CAP read timeout\n");
        }
    } else {
        g_espi_raw[7] = 0xFFFFFFFFu;
        kprint("ACPI: eSPI safe mode: skip SLV_CFG command writes\n");
    }

    g_espi_probed = 1;

    /* Summary: check for errors */
    pc_sts = espi_status_bits(R_ESPI_PCR_PCERR_SLV0, pcerr);
    vw_sts = espi_status_bits(R_ESPI_PCR_VWERR_SLV0, vwerr);
    fc_sts = espi_status_bits(R_ESPI_PCR_FCERR_SLV0, fcerr);
    ln_sts = espi_status_bits(R_ESPI_PCR_LNKERR_SLV0, lnkerr);

    if (ln_sts & B_ESPI_PCR_LNKERR_SLV0_SLCRR) {
        kprint("ACPI: eSPI LINK RECOVERY ACTIVE (SLCRR bit 31 set)\n");
    }
    if (ln_sts & B_ESPI_PCR_LNKERR_SLV0_LFET1S) {
        kprint("ACPI: eSPI FATAL ERROR TYPE 1 (LFET1S bit 20 set)\n");
    }
    if (pc_sts & B_ESPI_PCR_PCERR_SLV0_PCURD) {
        kprint("ACPI: eSPI PERIPHERAL UNSUPPORTED REQUEST (PCURD bit 24)\n");
    }
    if (pc_sts || vw_sts || fc_sts || ln_sts) {
        kprint("ACPI: eSPI has active status-bit errors — EC communication may be impaired\n");
    }
}

static void acpi_pci_prt_init(void) {
    char path[256];
    struct acpi_node *root = find_pci_root();
    struct acpi_object *prt = 0;

    if (root) {
        ns_build_path(root, path, sizeof(path));
        kprint("ACPI: found PCI root at %s, searching for _PRT\n", path);
    }

    if (!root || eval_path_obj(root, "PRT", &prt) != 0 || !prt || prt->type != AML_OBJ_PACKAGE) {
        struct acpi_node *alt = find_any_prt_node();
        if (!alt || eval_path_obj(alt, "PRT", &prt) != 0 || !prt || prt->type != AML_OBJ_PACKAGE) {
            return;
        }
        ns_build_path(alt, path, sizeof(path));
        kprint("ACPI: found _PRT on alternate node %s\n", path);
        root = alt;
    }

    ns_build_path(root, path, sizeof(path));
    kprint("ACPI: found _PRT on node %s, parsing...\n", path);

    uint32_t base_bus = pci_get_base_bus(root);
    uint32_t pci_count = 0;
    const struct pci_device_info *devs = pci_get_devices(&pci_count);
    if (!devs) return;

    for (uint32_t i = 0; i < prt->v.package.count; i++) {
        struct acpi_object *ent = prt->v.package.items[i];
        if (!ent || ent->type != AML_OBJ_PACKAGE || ent->v.package.count < 4) {
            continue;
        }
        uint64_t addr = obj_to_int(ent->v.package.items[0]);
        uint32_t pin = (uint32_t)obj_to_int(ent->v.package.items[1]);
        struct acpi_object *src = ent->v.package.items[2];
        uint32_t src_idx = (uint32_t)obj_to_int(ent->v.package.items[3]);

        uint32_t dev = (uint32_t)((addr >> 16) & 0xFFFF);
        uint32_t gsi = 0xFFFFFFFFu;

        if (src && src->type == AML_OBJ_REFERENCE) {
            struct acpi_node *lnk = (struct acpi_node *)src->v.ref;
            if (lnk && get_gsi_from_link_node(lnk, &gsi) == 0) {
                // ok
            }
        } else if (src && src->type == AML_OBJ_STRING) {
            struct acpi_node *lnk = ns_lookup(ns_root(), src->v.string);
            if (lnk && get_gsi_from_link_node(lnk, &gsi) == 0) {
                // ok
            }
        } else if (src_idx) {
            gsi = src_idx;
        }

        if (gsi == 0xFFFFFFFFu) continue;

        for (uint32_t d = 0; d < pci_count; d++) {
            if (devs[d].bus == base_bus && devs[d].slot == dev) {
                pci_set_irq(devs[d].bus, devs[d].slot, devs[d].func, (uint8_t)pin, gsi);
                kprint("ACPI: _PRT bus=%u dev=%u pin=%u -> GSI %u\n",
                       devs[d].bus, devs[d].slot, pin, gsi);
            }
        }
    }
}

static void acpi_pci_osc_init(void) {
    struct acpi_node *root = find_pci_root();
    if (!root) return;

    uint8_t uuid[16] = {
        0x5B,0x4D,0xDB,0x33,
        0xF7,0x1F,
        0x1C,0x40,
        0x96,0x57,0x74,0x41,0xC0,0x3D,0xD7,0x66
    };
    uint32_t caps = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
    uint8_t capbuf[4] = {
        (uint8_t)(caps & 0xFF),
        (uint8_t)((caps >> 8) & 0xFF),
        (uint8_t)((caps >> 16) & 0xFF),
        (uint8_t)((caps >> 24) & 0xFF)
    };

    struct acpi_object *args[4];
    args[0] = make_buf(uuid, sizeof(uuid));
    args[1] = aml_obj_make_int(1);
    args[2] = aml_obj_make_int(1);
    args[3] = make_buf(capbuf, sizeof(capbuf));

    struct acpi_object *ret = 0;
    if (eval_path_obj_args(root, "OSC", args, 4, &ret) == 0 && ret && ret->type == AML_OBJ_BUFFER) {
        uint32_t status = 0;
        if (ret->v.buffer.length >= 4) {
            status = (uint32_t)(ret->v.buffer.data[0] |
                                (ret->v.buffer.data[1] << 8) |
                                (ret->v.buffer.data[2] << 16) |
                                (ret->v.buffer.data[3] << 24));
        }
        kprint("ACPI: _OSC status=0x%08x len=%u\n", status, ret->v.buffer.length);
    }
}

static struct acpi_object *acpi_dsm_eval(const char *path, const uint8_t uuid[16],
                                        uint32_t rev, uint32_t func, struct acpi_object *params) {
    struct acpi_object *args[4];
    args[0] = make_buf(uuid, 16);
    args[1] = aml_obj_make_int(rev);
    args[2] = aml_obj_make_int(func);
    args[3] = params ? params : make_buf(0, 0);
    return aml_eval_with_args(path, args, 4);
}

void acpi_nvme_dsm_for_pci(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t adr = ((uint32_t)slot << 16) | (uint32_t)func;
    struct acpi_node *dev = ns_find_device_by_adr(adr);
    if (!dev) {
        kprint("ACPI: NVMe _DSM device not found for ADR 0x%08x\n", adr);
        return;
    }
    char path[256];
    ns_build_path(dev, path, sizeof(path));
    // Append ._DSM
    uint32_t i = 0;
    while (path[i] && i < sizeof(path) - 6) i++;
    path[i++] = '.';
    path[i++] = '_';
    path[i++] = 'D';
    path[i++] = 'S';
    path[i++] = 'M';
    path[i++] = 0;

    uint8_t nvme_uuid[16] = {
        0xD0,0x37,0xC9,0xE5,
        0x53,0x35,
        0x7A,0x4D,
        0x91,0x17,0xEA,0x4D,0x19,0xC3,0x43,0x4D
    };

    struct acpi_object *r0 = acpi_dsm_eval(path, nvme_uuid, 1, 0, 0);
    if (r0) {
        kprint("ACPI: NVMe _DSM funcs=0x%llx\n", aml_obj_to_int(r0));
    }
    struct acpi_object *r9 = acpi_dsm_eval(path, nvme_uuid, 1, 9, 0);
    if (r9) {
        kprint("ACPI: NVMe _DSM queue depth=0x%llx\n", aml_obj_to_int(r9));
    }
    (void)bus;
}

void acpi_thermal_poll_once(void) {
    struct acpi_node *root = ns_root();
    if (!root) return;
    struct acpi_node *stack[64];
    uint32_t sp = 0;
    stack[sp++] = root;
    while (sp) {
        struct acpi_node *tz = stack[--sp];
        if (tz->type == ACPI_NODE_THERMAL) {
            struct acpi_object *tmp = 0;
            if (eval_path_obj(tz, "TMP", &tmp) == 0 && tmp) {
                uint64_t t = obj_to_int(tmp);
                int32_t c = (int32_t)(t / 10) - 273;
                struct acpi_object *crt = 0;
                if (eval_path_obj(tz, "CRT", &crt) == 0 && crt) {
                    uint64_t tc = obj_to_int(crt);
                    int32_t cc = (int32_t)(tc / 10) - 273;
                    if (c > cc) {
                        kprint("ACPI: thermal critical %dC > %dC\n", c, cc);
                        acpi_shutdown();
                    }
                }
            }
        }
        for (struct acpi_node *c = tz->first_child; c; c = c->next_sibling) {
            if (sp < 64) stack[sp++] = c;
        }
    }
}

static void thermal_thread(void *arg) {
    (void)arg;
    for (;;) {
        acpi_thermal_poll_once();
        struct fry_process *cur = proc_current();
        if (cur) {
            sched_sleep(cur->pid, 5000);
            sched_yield();
        }
    }
}

static void find_battery_cb(struct acpi_node *node, void *ctx) {
    char path[256];
    uint64_t sta = 0;
    int present = 0;
    int id_match = 0;
    int has_bst = 0;
    int has_bif = 0;
    int has_bix = 0;
    (void)ctx;
    if (g_batt_count >= 4) return;
    id_match = ns_hid_match(node, "PNP0C0A");
    has_bst = has_method3(node, "BST");
    has_bif = has_method3(node, "BIF");
    has_bix = has_method3(node, "BIX");
    if (!id_match && !(has_bst && (has_bif || has_bix))) return;
    present = dev_present(node);
    if (!present && !has_bst) return;
    g_batt_nodes[g_batt_count++] = node;
    ns_build_path(node, path, sizeof(path));
    if (get_sta_flags(node, &sta) == 0) {
        kprint("ACPI: battery node %s _STA=0x%llx present=%u hid=%u bst=%u bif=%u bix=%u\n",
               path, sta, (uint32_t)present, (uint32_t)id_match, (uint32_t)has_bst,
               (uint32_t)has_bif, (uint32_t)has_bix);
    } else {
        kprint("ACPI: battery node %s _STA=missing present=%u hid=%u bst=%u bif=%u bix=%u\n",
               path, (uint32_t)present, (uint32_t)id_match, (uint32_t)has_bst,
               (uint32_t)has_bif, (uint32_t)has_bix);
    }
}

static void battery_thread(void *arg) {
    (void)arg;
    for (;;) {
        acpi_battery_refresh();
        struct fry_process *cur = proc_current();
        if (cur) {
            sched_sleep(cur->pid, 5000);
            sched_yield();
        }
    }
}

void acpi_battery_refresh(void) {
    struct fry_battery_status best = g_batt;
    int best_set = 0;
    for (uint32_t i = 0; i < g_batt_count; i++) {
        struct acpi_node *bat = g_batt_nodes[i];
        if (!dev_present(bat) && !has_method3(bat, "BST")) continue;
        struct acpi_object *bst = 0;
        if (eval_path_obj(bat, "BST", &bst) == 0 && bst && bst->type == AML_OBJ_PACKAGE) {
            if (bst->v.package.count >= 4) {
                struct fry_battery_status cur;
                cur.state = (uint32_t)obj_to_int(bst->v.package.items[0]);
                cur.present_rate = (uint32_t)obj_to_int(bst->v.package.items[1]);
                cur.remaining_capacity = (uint32_t)obj_to_int(bst->v.package.items[2]);
                cur.present_voltage = (uint32_t)obj_to_int(bst->v.package.items[3]);

                if (cur.present_voltage || cur.present_rate || cur.remaining_capacity || cur.state) {
                    g_batt = cur;
                    return;
                }
                if (!best_set) {
                    best = cur;
                    best_set = 1;
                }
            }
        }
    }
    if (best_set) {
        g_batt = best;
    }
}

static void acpi_battery_init(void) {
    g_batt_count = 0;
    g_batt.state = 0;
    g_batt.present_rate = 0;
    g_batt.remaining_capacity = 0;
    g_batt.present_voltage = 0;
    for (uint32_t i = 0; i < 4; i++) g_batt_nodes[i] = 0;
    ns_walk(find_battery_cb, 0);
    if (g_batt_count == 0) {
        kprint("ACPI: no battery\n");
        return;
    }
    kprint("ACPI: battery candidates=%u\n", g_batt_count);
    struct acpi_object *info = 0;
    if (eval_path_obj(g_batt_nodes[0], "BIX", &info) == 0 && info && info->type == AML_OBJ_PACKAGE) {
        kprint("ACPI: battery _BIX count=%u\n", info->v.package.count);
    } else if (eval_path_obj(g_batt_nodes[0], "BIF", &info) == 0 && info && info->type == AML_OBJ_PACKAGE) {
        kprint("ACPI: battery _BIF count=%u\n", info->v.package.count);
    }
    acpi_battery_refresh();
    struct fry_process *p = process_create_kernel(battery_thread, 0, "battery");
    if (p) {
        sched_add(p->pid);
    }
}

struct backlight_find_ctx {
    struct acpi_node *node;
    int score;
};

static void find_backlight_cb(struct acpi_node *node, void *ctx) {
    struct backlight_find_ctx *best = (struct backlight_find_ctx *)ctx;
    int score = 0;
    int present = 0;
    int hid = 0;
    int has_bcm = 0;
    int has_bqc = 0;
    int has_bcl = 0;
    if (!best) return;
    hid = ns_hid_match(node, "ACPI0008");
    has_bcm = has_method3(node, "BCM");
    has_bqc = has_method3(node, "BQC");
    has_bcl = has_method3(node, "BCL");
    if (!hid && !has_bcm && !has_bqc && !has_bcl) return;

    present = dev_present(node);
    if (!present && !has_bqc && !has_bcl) return;

    if (hid) score += 8;
    if (has_bcm) score += 4;
    if (has_bqc) score += 3;
    if (has_bcl) score += 2;
    if (present) score += 1;

    if (!best->node || score > best->score) {
        best->node = node;
        best->score = score;
    }
}

static void acpi_backlight_init(void) {
    char path[256];
    uint64_t sta = 0;
    struct backlight_find_ctx find_ctx;
    g_backlight_node = 0;
    backlight_reset_levels();
    g_brightness = 0;
    g_brightness_raw = 0;

    find_ctx.node = 0;
    find_ctx.score = -1;
    ns_walk(find_backlight_cb, &find_ctx);
    g_backlight_node = find_ctx.node;
    if (!g_backlight_node) {
        kprint("ACPI: no backlight device\n");
        return;
    }
    ns_build_path(g_backlight_node, path, sizeof(path));
    if (get_sta_flags(g_backlight_node, &sta) == 0) {
        kprint("ACPI: backlight node %s _STA=0x%llx\n", path, sta);
    } else {
        kprint("ACPI: backlight node %s _STA=missing\n", path);
    }

    struct acpi_object *bcl = 0;
    if (eval_path_obj(g_backlight_node, "BCL", &bcl) == 0 && bcl && bcl->type == AML_OBJ_PACKAGE) {
        backlight_parse_bcl(bcl);
    }

    struct acpi_object *bqc = 0;
    if (eval_path_obj(g_backlight_node, "BQC", &bqc) == 0 && bqc) {
        g_brightness_raw = (uint32_t)obj_to_int(bqc);
    } else if (g_bcl_count > 0) {
        g_brightness_raw = g_bcl_levels[0];
    }
    if (g_brightness_raw == 0 && g_bcl_supported_count > 0) {
        for (uint32_t i = 0; i < g_bcl_supported_count; i++) {
            if (g_bcl_supported_levels[i] != 0) {
                g_brightness_raw = g_bcl_supported_levels[i];
                break;
            }
        }
    }
    g_brightness = backlight_raw_to_percent(g_brightness_raw);
    kprint("ACPI: backlight levels=%u supported=%u raw=%u current=%u%%\n",
           g_bcl_count, g_bcl_supported_count, g_brightness_raw, g_brightness);
}

int acpi_backlight_set(uint32_t percent) {
    if (!g_backlight_node) return -1;
    uint32_t raw = backlight_percent_to_raw(percent);
    struct acpi_object *args[1];
    args[0] = aml_obj_make_int(raw);
    struct acpi_object *ret = 0;
    if (eval_path_obj_args(g_backlight_node, "BCM", args, 1, &ret) != 0) {
        return -1;
    }
    if (percent > 100u) percent = 100u;
    g_brightness_raw = raw;
    g_brightness = percent;
    return 0;
}

uint32_t acpi_backlight_get(void) {
    /* Return cached brightness; avoid AML eval on every userspace poll. */
    return g_brightness;
}

void acpi_backlight_refresh(void) {
    if (!g_backlight_node) return;
    struct acpi_object *bqc = 0;
    if (eval_path_obj(g_backlight_node, "BQC", &bqc) == 0 && bqc) {
        g_brightness_raw = (uint32_t)obj_to_int(bqc);
        g_brightness = backlight_raw_to_percent(g_brightness_raw);
    }
}

int acpi_battery_get(struct fry_battery_status *out) {
    if (!out) return -1;
    if (g_batt_count == 0) return -1;
    *out = g_batt;
    return 0;
}

int acpi_get_diag(struct fry_acpi_diag *out) {
    if (!out) return -1;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    /* Namespace node count */
    out->ns_nodes = ns_node_count();

    /* EC */
    out->ec_ok = (uint8_t)ec_available();
    ec_get_ports(&out->ec_data_port, &out->ec_cmd_port);
    ec_get_probe_diag(&out->ec_probe_step, &out->ec_probe_status, &out->ec_probe_attempts);
    out->ec_node_found = g_ec_node_found;
    out->ec_reg_called = g_ec_reg_called;
    out->ec_ini_found  = g_ec_ini_found;
    out->ec_gpe_found  = g_ec_gpe_found;
    out->ec_gpe_num    = g_ec_gpe_num;
    out->lpc_ioe_before = g_lpc_ioe_before;
    out->lpc_ioe_after  = g_lpc_ioe_after;
    out->ec_ports_source = g_ec_ports_source;
    out->lpc_bus = g_lpc_bus;
    out->lpc_slot = g_lpc_slot;
    out->lpc_func = g_lpc_func;
    out->lpc_vendor = g_lpc_vendor;
    out->lpc_device = g_lpc_device;
    out->lpc_class_code = g_lpc_class_code;
    out->lpc_subclass = g_lpc_subclass;
    out->lpc_prog_if = g_lpc_prog_if;
    out->lpc_write_attempted = g_lpc_write_attempted;
    out->lpc_write_performed = g_lpc_write_performed;
    out->lpc_cmd = g_lpc_cmd;
    out->lpc_reg80_before = g_lpc_reg80_before;
    out->lpc_reg80_after = g_lpc_reg80_after;
    out->lpc_reg84 = g_lpc_reg84;
    out->lpc_reg88 = g_lpc_reg88;
    out->pcr_ioe_before = g_pcr_ioe_before;
    out->pcr_ioe_after = g_pcr_ioe_after;
    out->pcr_mirror_done = g_pcr_mirror_done;
    out->p2sb_hidden = g_p2sb_was_hidden;
    out->ec_early_sts = g_ec_early_sts;
    out->ec_post_lpc_sts = g_ec_post_lpc_sts;
    out->pcr_pid = g_pcr_pid_used;
    out->ec_pre_reg_sts = g_ec_pre_reg_sts;
    out->ec_pre_reg_probe_ok = g_ec_pre_reg_probe_ok;
    out->ec_recovery_method = g_ec_recovery_method;
    {
        uint32_t s = ec_get_reg_suppressed_count();
        out->ec_reg_suppressed = (s > 0xFFFF) ? 0xFFFF : (uint16_t)s;
    }
    /* fry421 immediate probe diagnostics */
    out->ec_imm_step = g_ec_imm_step;
    out->ec_imm_val = g_ec_imm_val;
    out->ec_imm_post_sts = g_ec_imm_post_sts;
    out->ec_post_setup_sts = g_ec_post_setup_sts;
    /* fry422 floating bus + force SMI diagnostics */
    out->ec_ibf_seen = g_ec_ibf_seen;
    out->ec_pre_data = g_ec_pre_data;
    out->ec_smi_sent = g_ec_smi_sent;
    out->ec_imm2_step = g_ec_imm2_step;
    /* fry423: Gen3/Gen4 + eSPI diagnostics */
    out->lpc_reg8c = g_lpc_reg8c;
    out->lpc_reg90 = g_lpc_reg90;
    for (int i = 0; i < 8; i++) out->espi_raw[i] = g_espi_raw[i];
    out->espi_probed = g_espi_probed;
    out->espi_pid = g_espi_pid;
    out->espi_en = g_espi_en_strap;
    /* fry435: eSPI error clear diagnostics */
    for (int i = 0; i < 4; i++) out->espi_pre_clear[i]  = g_espi_pre_clear[i];
    for (int i = 0; i < 4; i++) out->espi_post_clear[i] = g_espi_post_clear[i];
    out->espi_clear_run   = g_espi_clear_run;
    out->espi_clear_found = g_espi_clear_found;
    out->espi_clear_ok    = g_espi_clear_ok;

    /* G445: EC policy diagnostics */
    {
        const ec_policy_t *pol = ec_policy_get();
        out->ec_policy_timeout  = pol->ibf_obf_timeout;
        out->ec_policy_retries  = (uint8_t)pol->probe_retries;
        out->ec_policy_max_fail = (uint8_t)pol->max_consec_fail;
        out->ec_policy_flags    = (uint8_t)(
            (pol->try_alternate_ports ? 1u : 0u) |
            (pol->try_swapped_ports   ? 2u : 0u) |
            (pol->suppress_reg_io     ? 4u : 0u)
        );
    }

    /* G451/G452: EC query/event diagnostics */
    out->ec_queries_dispatched = ec_get_queries_dispatched();
    out->ec_queries_dropped    = ec_get_queries_dropped();
    out->ec_storm_count        = ec_get_storm_count();
    out->ec_events_frozen      = ec_events_frozen() ? 1 : 0;

    /* G446: candidate count */
    out->ec_cand_count        = g_ec_cand_count;
    out->ec_best_cand_source  = g_ec_ports_source;

    /* fry444: eSPI slave channel diagnostics */
    out->espi_slave_pc_cap    = g_espi_slave_pc_cap;
    out->espi_slave_vw_cap    = g_espi_slave_vw_cap;
    out->espi_slave_pc_en     = g_espi_slave_pc_en;
    out->espi_slave_vw_en     = g_espi_slave_vw_en;
    out->espi_gen_chan_sup     = g_espi_gen_chan_sup;
    out->espi_slave_read_ok   = g_espi_slave_read_ok;

    /* fry446: eSPI channel initialization diagnostics */
    out->espi_chinit_result   = g_espi_chinit_result;
    out->espi_pltrst_state    = g_espi_pltrst_state;
    out->espi_pltrst_sent     = g_espi_pltrst_sent;
    out->espi_chinit_retries  = g_espi_chinit_retries;
    out->espi_chinit_pc_cap   = g_espi_chinit_pc_cap;
    out->espi_chinit_vw_cap   = g_espi_chinit_vw_cap;

    /* Battery */
    out->batt_count = (uint8_t)g_batt_count;
    if (g_batt_count > 0 && g_batt_nodes[0]) {
        ns_build_path(g_batt_nodes[0], out->batt_path, 64);
        uint64_t sta = 0;
        if (get_sta_flags(g_batt_nodes[0], &sta) == 0)
            out->batt_sta = (uint32_t)sta;
        else
            out->batt_sta = 0xFFFFFFFFu;
    }

    /* Backlight */
    out->bl_found = g_backlight_node ? 1 : 0;
    out->bl_bcl_count = g_bcl_count;
    out->bl_supported_count = g_bcl_supported_count;
    out->bl_raw = g_brightness_raw;
    out->bl_percent = g_brightness;
    if (g_backlight_node) {
        ns_build_path(g_backlight_node, out->bl_path, 64);
        uint64_t sta = 0;
        if (get_sta_flags(g_backlight_node, &sta) == 0)
            out->bl_sta = (uint32_t)sta;
        else
            out->bl_sta = 0xFFFFFFFFu;
    }

    return 0;
}

static void ec_reg_init(void) {
    uint16_t data_port = 0x62;
    uint16_t cmd_port = 0x66;
    uint16_t ecdt_data_port = 0;
    uint16_t ecdt_cmd_port = 0;
    uint8_t ecdt_gpe_bit = 0xFF;
    int ecdt_ports_valid = 0;
    struct acpi_node *ec_node = 0;

    /*
     * fry422 PHASE 0: FLOATING BUS DETECTION + IMMEDIATE RAW EC PROBE
     *
     * Before ANY initialization (no LPC decode config, no ACPI_ENABLE SMI,
     * no namespace scan, no AML evaluation).  Just raw port I/O to 0x66/0x62.
     *
     * fry421 showed: status always 0x00, IBF clears instantly, OBF never sets.
     * This is consistent with a FLOATING BUS (no device on ports 0x62/0x66).
     *
     * NEW in fry422:
     *   A) Read port 0x62 BEFORE any command — check for pending EC data
     *   B) After writing READ cmd to 0x66, IMMEDIATELY check if IBF is set.
     *      On a real EC, IBF (bit 1) is briefly asserted while the EC reads
     *      the command byte.  On a floating bus, IBF is never set.
     *   C) Extended OBF timeout: 1M iterations (~2-4 seconds) instead of 100K
     *   D) Log status samples during OBF wait to detect transient changes
     *   E) If Phase 0 fails: send force ACPI_ENABLE SMI, then re-probe (Phase 1)
     */
    {
        uint8_t sts = inb(0x66u);
        g_ec_imm_step = 0;
        g_ec_imm_val = 0;
        g_ec_ibf_seen = 0;
        g_ec_smi_sent = 0;
        g_ec_imm2_step = 0;

        /* Read data port without command — check for pending output */
        g_ec_pre_data = inb(0x62u);
        kprint("ACPI: EC fry422: initial sts=0x%02x pre_data=0x%02x OBF=%u IBF=%u\n",
               (uint32_t)sts, (uint32_t)g_ec_pre_data,
               (sts & 0x01u) ? 1u : 0u, (sts & 0x02u) ? 1u : 0u);

        /* If OBF is already set, drain it first */
        if (sts & 0x01u) {
            kprint("ACPI: EC fry422: OBF set at boot! reading data port to drain\n");
            uint8_t drained = inb(0x62u);
            kprint("ACPI: EC fry422: drained=0x%02x\n", (uint32_t)drained);
            sts = inb(0x66u);
            kprint("ACPI: EC fry422: after drain sts=0x%02x\n", (uint32_t)sts);
        }

        /*
         * Pre-_REG command probes can desync some firmware EC state machines.
         * Keep fry422 fields for diagnostics, but default to passive sampling.
         */
        const int run_active_early_probe = 0;
        if (!run_active_early_probe) {
            g_ec_imm_step = 0;
            g_ec_imm2_step = 0;
            g_ec_imm_post_sts = inb(0x66u);
            kprint("ACPI: EC fry422: early active probe disabled (pre-_REG safety)\n");
        } else if (sts == 0xFFu) {
            g_ec_imm_step = 1; /* STS_FF — no device */
            kprint("ACPI: EC fry422: 0xFF, skipping\n");
        } else {
            /* IBF clear before command */
            int ok = 0;
            for (int i = 0; i < 100000; i++) {
                if (!(inb(0x66u) & 0x02u)) { ok = 1; break; }
                __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
            }
            if (!ok) {
                g_ec_imm_step = 2; /* IBF_PRE */
                kprint("ACPI: EC fry422: IBF stuck before cmd\n");
            } else {
                /* Write READ command to command port */
                outb(0x66u, 0x80u); /* EC_CMD_READ */
                __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0)); /* io_wait */

                /*
                 * FLOATING BUS TEST: read status IMMEDIATELY after write.
                 * On a real EC, IBF (bit 1) is asserted when the host writes
                 * to the command port.  The EC clears IBF when it reads the
                 * byte.  If IBF is NEVER set even on the first read after
                 * the write, the port is floating (no device).
                 */
                uint8_t ibf1 = inb(0x66u);
                uint8_t ibf_set = (ibf1 & 0x02u) ? 1 : 0;
                g_ec_ibf_seen = ibf_set;
                kprint("ACPI: EC fry422: IBF test: sts_after_write=0x%02x IBF=%u\n",
                       (uint32_t)ibf1, (uint32_t)ibf_set);
                if (!ibf_set) {
                    kprint("ACPI: EC fry422: *** IBF NOT SET — FLOATING BUS LIKELY ***\n");
                    kprint("ACPI: EC fry422: (real EC would assert IBF for ~1-100us)\n");
                }

                /* Wait for IBF to clear (count iterations) */
                ok = 0;
                uint32_t ibf_iters = 0;
                for (int i = 0; i < 100000; i++) {
                    if (!(inb(0x66u) & 0x02u)) { ok = 1; ibf_iters = (uint32_t)i; break; }
                    __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
                }
                if (!ok) {
                    g_ec_imm_step = 3; /* IBF_POST */
                    kprint("ACPI: EC fry422: IBF stuck after cmd (100K iters)\n");
                } else {
                    kprint("ACPI: EC fry422: IBF cleared after %u iters\n", ibf_iters);

                    /* Send address byte */
                    outb(0x62u, 0x00u); /* address byte: reg[0] */

                    /*
                     * EXTENDED OBF WAIT: 1M iterations (~2-4 seconds).
                     * fry421 used 100K — maybe EC needs much longer to respond.
                     * Sample status every 200K iterations for diagnostic.
                     */
                    ok = 0;
                    uint32_t obf_iters = 0;
                    for (int i = 0; i < 1000000; i++) {
                        uint8_t s = inb(0x66u);
                        if (s & 0x01u) { ok = 1; obf_iters = (uint32_t)i; break; }
                        /* Log samples at 100K, 300K, 500K, 700K, 900K */
                        if (i == 100000 || i == 300000 || i == 500000 ||
                            i == 700000 || i == 900000) {
                            kprint("ACPI: EC fry422: OBF wait sample @%dk sts=0x%02x\n",
                                   i / 1000, (uint32_t)s);
                        }
                        if (s == 0xFFu && i > 200000) {
                            kprint("ACPI: EC fry422: EC went 0xFF at iter %u — aborting\n",
                                   (uint32_t)i);
                            break;
                        }
                        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
                    }
                    if (ok) {
                        g_ec_imm_val = inb(0x62u);
                        g_ec_imm_step = 5; /* OK */
                        kprint("ACPI: EC fry422: OK! reg[0]=0x%02x after %u iters\n",
                               (uint32_t)g_ec_imm_val, obf_iters);
                    } else {
                        g_ec_imm_step = 4; /* OBF_TMO */
                        kprint("ACPI: EC fry422: OBF timeout after 1M iterations\n");
                    }
                }
            }
        }
        if (run_active_early_probe) {
        g_ec_imm_post_sts = inb(0x66u);
        kprint("ACPI: EC fry422: IMM post-sts=0x%02x\n", (uint32_t)g_ec_imm_post_sts);

        if (g_ec_imm_step == 5) {
            /*
             * EC responds to raw READ with ZERO initialization!
             * Set up minimal state and SKIP all dangerous init steps.
             */
            ec_setup_ports(0x62u, 0x66u); /* sets g_ec_ok=1 */
            g_ec_early_sts = sts;
            g_ec_post_lpc_sts = sts;
            g_ec_pre_reg_sts = sts;
            g_ec_pre_reg_probe_ok = 1;
            g_ec_recovery_method = 1;
            g_ec_post_setup_sts = sts;
            kprint("ACPI: EC fry422: IMMEDIATE PROBE OK — skipping all init\n");
            struct ec_scan_ctx scan;
            scan.count = 0;
            ns_walk(collect_ec_candidates_cb, &scan);
            if (scan.count > 0) {
                g_ec_node_found = 1;
            }
            g_ec_gpe_found = 1;
            g_ec_gpe_num = 111;
            return;
        }

        /*
         * fry422 PHASE 1: FORCE ACPI_ENABLE SMI + RE-PROBE
         *
         * Maybe the EC needs the ACPI_ENABLE SMI to start responding.
         * ec_try_acpi_enable skips if SCI_EN is already set, but the
         * Dell UEFI might set SCI_EN in PM1a without actually sending
         * ACPI_ENABLE to the EC firmware.  Force-send it regardless.
         */
        kprint("ACPI: EC fry422: Phase 1 — force ACPI_ENABLE then re-probe\n");
        {
            const struct fadt_info *f = fadt_get_info();
            if (f && f->smi_cmd && f->acpi_enable && f->smi_cmd <= 0xFFFFu) {
                uint16_t pm1_pre = 0;
                if (f->pm1a_cnt_blk) {
                    pm1_pre = inw((uint16_t)f->pm1a_cnt_blk);
                }
                kprint("ACPI: EC fry422: PM1a=0x%04x SCI_EN=%u (sending ACPI_ENABLE anyway)\n",
                       (uint32_t)pm1_pre, (uint32_t)(pm1_pre & 1u));
                outb((uint16_t)f->smi_cmd, f->acpi_enable);
                g_ec_smi_sent = 1;

                /* Wait ~100ms for SMI to process */
                for (volatile int i = 0; i < 10000000; i++) {}

                if (f->pm1a_cnt_blk) {
                    uint16_t pm1_post = inw((uint16_t)f->pm1a_cnt_blk);
                    kprint("ACPI: EC fry422: after SMI PM1a=0x%04x SCI_EN=%u\n",
                           (uint32_t)pm1_post, (uint32_t)(pm1_post & 1u));
                }

                /* Check EC status after SMI */
                uint8_t smi_sts = inb(0x66u);
                kprint("ACPI: EC fry422: post-SMI EC sts=0x%02x\n", (uint32_t)smi_sts);

                /* Re-probe: same sequence as Phase 0 */
                if (smi_sts != 0xFFu) {
                    int ok2 = 0;
                    for (int i = 0; i < 100000; i++) {
                        if (!(inb(0x66u) & 0x02u)) { ok2 = 1; break; }
                        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
                    }
                    if (ok2) {
                        outb(0x66u, 0x80u); /* READ cmd */
                        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));

                        uint8_t ibf2 = inb(0x66u);
                        kprint("ACPI: EC fry422: Phase1 IBF test: 0x%02x IBF=%u\n",
                               (uint32_t)ibf2, (ibf2 & 0x02u) ? 1u : 0u);
                        if (ibf2 & 0x02u) g_ec_ibf_seen = 1;

                        ok2 = 0;
                        for (int i = 0; i < 100000; i++) {
                            if (!(inb(0x66u) & 0x02u)) { ok2 = 1; break; }
                            __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
                        }
                        if (ok2) {
                            outb(0x62u, 0x00u); /* addr byte */
                            ok2 = 0;
                            for (int i = 0; i < 1000000; i++) {
                                if (inb(0x66u) & 0x01u) { ok2 = 1; break; }
                                __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
                            }
                            if (ok2) {
                                uint8_t v2 = inb(0x62u);
                                g_ec_imm2_step = 5; /* OK */
                                kprint("ACPI: EC fry422: Phase1 OK! reg[0]=0x%02x\n",
                                       (uint32_t)v2);
                                g_ec_imm_val = v2;
                                ec_setup_ports(0x62u, 0x66u);
                                g_ec_early_sts = sts;
                                g_ec_post_lpc_sts = sts;
                                g_ec_pre_reg_sts = sts;
                                g_ec_pre_reg_probe_ok = 1;
                                g_ec_recovery_method = 1;
                                g_ec_post_setup_sts = smi_sts;
                                struct ec_scan_ctx scan;
                                scan.count = 0;
                                ns_walk(collect_ec_candidates_cb, &scan);
                                if (scan.count > 0) g_ec_node_found = 1;
                                g_ec_gpe_found = 1;
                                g_ec_gpe_num = 111;
                                return;
                            } else {
                                g_ec_imm2_step = 4; /* OBF_TMO */
                                kprint("ACPI: EC fry422: Phase1 OBF timeout\n");
                            }
                        } else {
                            g_ec_imm2_step = 3; /* IBF_POST */
                            kprint("ACPI: EC fry422: Phase1 IBF stuck after cmd\n");
                        }
                    } else {
                        g_ec_imm2_step = 2; /* IBF_PRE */
                        kprint("ACPI: EC fry422: Phase1 IBF stuck before cmd\n");
                    }
                } else {
                    g_ec_imm2_step = 1; /* STS_FF */
                    kprint("ACPI: EC fry422: Phase1 status 0xFF after SMI\n");
                }
            } else {
                kprint("ACPI: EC fry422: no FADT SMI_CMD, skipping Phase 1\n");
            }
        }
        kprint("ACPI: EC fry422: both immediate probes failed, continuing normal init\n");
        }
    }

    g_ec_early_sts = inb(0x66u);
    kprint("ACPI: EC early status 0x%02x (before init)\n", (uint32_t)g_ec_early_sts);

    g_ec_node_found = 0;
    g_ec_reg_called = 0;
    g_ec_ini_found = 0;
    g_ec_gpe_found = 0;
    g_ec_gpe_num = 0xFF;
    g_ec_ports_source = 0;
    g_lpc_ioe_before = 0xFFFFu;
    g_lpc_ioe_after = 0xFFFFu;
    g_lpc_bus = 0;
    g_lpc_slot = 0x1Fu;
    g_lpc_func = 0u;
    g_lpc_vendor = 0xFFFFu;
    g_lpc_device = 0xFFFFu;
    g_lpc_class_code = 0xFFu;
    g_lpc_subclass = 0xFFu;
    g_lpc_prog_if = 0xFFu;
    g_lpc_cmd = 0xFFFFu;
    g_lpc_reg80_before = 0xFFFFFFFFu;
    g_lpc_reg80_after = 0xFFFFFFFFu;
    g_lpc_reg84 = 0xFFFFFFFFu;
    g_lpc_reg88 = 0xFFFFFFFFu;
    g_lpc_reg8c = 0xFFFFFFFFu;
    g_lpc_reg90 = 0xFFFFFFFFu;
    g_lpc_write_attempted = 0;
    g_lpc_write_performed = 0;
    g_pcr_ioe_before = 0xFFFFFFFFu;
    g_pcr_ioe_after = 0xFFFFFFFFu;
    g_pcr_mirror_done = 0;
    g_p2sb_was_hidden = 0;
    g_ec_post_lpc_sts = 0xFFu;
    g_ec_pre_reg_probe_ok = 0;
    g_ec_recovery_method = 0;

    /*
     * Try ECDT table first (boot-time EC info, no AML needed).
     * G447: Hardened ECDT parsing — validate GAS fields, extract GPE_BIT + EC_ID.
     *
     * ECDT layout (ACPI spec 5.2.16):
     *   Header    (36 bytes, offset 0)
     *   EC_CONTROL GAS (12 bytes, offset 36) — command/status port
     *   EC_DATA    GAS (12 bytes, offset 48) — data port
     *   UID        (4 bytes, offset 60)
     *   GPE_BIT    (1 byte, offset 64)
     *   EC_ID      (null-terminated string, offset 65+)
     */
    struct acpi_sdt_header *ecdt = acpi_find_table("ECDT");
    if (ecdt && ecdt->length >= 65) {
        const uint8_t *raw = (const uint8_t *)ecdt;

        /* GAS field validation (12-byte Generic Address Structure) */
        uint8_t ctrl_space   = raw[36];     /* address_space_id */
        uint8_t ctrl_width   = raw[36 + 1]; /* bit_width */
        uint8_t ctrl_offset  = raw[36 + 2]; /* bit_offset */
        uint8_t ctrl_access  = raw[36 + 3]; /* access_size */
        uint64_t ctrl_addr   = *(const uint64_t *)(raw + 36 + 4);

        uint8_t data_space   = raw[48];
        uint8_t data_width   = raw[48 + 1];
        uint8_t data_offset  = raw[48 + 2];
        uint8_t data_access  = raw[48 + 3];
        uint64_t data_addr   = *(const uint64_t *)(raw + 48 + 4);

        kprint("ACPI: ECDT ctrl: space=%u width=%u offset=%u access=%u addr=0x%x\n",
               (uint32_t)ctrl_space, (uint32_t)ctrl_width,
               (uint32_t)ctrl_offset, (uint32_t)ctrl_access, (uint32_t)ctrl_addr);
        kprint("ACPI: ECDT data: space=%u width=%u offset=%u access=%u addr=0x%x\n",
               (uint32_t)data_space, (uint32_t)data_width,
               (uint32_t)data_offset, (uint32_t)data_access, (uint32_t)data_addr);

        /* Validate: must be System I/O space (1), sensible bit_width, zero bit_offset */
        int ecdt_valid = 1;
        if (ctrl_space != 1) {
            kprint("ACPI: ECDT reject: ctrl space=%u (expected 1=IO)\n", (uint32_t)ctrl_space);
            ecdt_valid = 0;
        }
        if (data_space != 1) {
            kprint("ACPI: ECDT reject: data space=%u (expected 1=IO)\n", (uint32_t)data_space);
            ecdt_valid = 0;
        }
        if (ctrl_width != 0 && ctrl_width != 8) {
            kprint("ACPI: ECDT warning: ctrl bit_width=%u (expected 8)\n", (uint32_t)ctrl_width);
        }
        if (data_width != 0 && data_width != 8) {
            kprint("ACPI: ECDT warning: data bit_width=%u (expected 8)\n", (uint32_t)data_width);
        }
        if (ctrl_offset != 0 || data_offset != 0) {
            kprint("ACPI: ECDT reject: non-zero bit_offset ctrl=%u data=%u\n",
                   (uint32_t)ctrl_offset, (uint32_t)data_offset);
            ecdt_valid = 0;
        }
        if (ctrl_addr == 0 || ctrl_addr > 0xFFFF || data_addr == 0 || data_addr > 0xFFFF) {
            kprint("ACPI: ECDT reject: addr out of range ctrl=0x%x data=0x%x\n",
                   (uint32_t)ctrl_addr, (uint32_t)data_addr);
            ecdt_valid = 0;
        }

        if (ecdt_valid) {
            ecdt_cmd_port = (uint16_t)ctrl_addr;
            ecdt_data_port = (uint16_t)data_addr;
            ecdt_ports_valid = 1;

            /* Extract GPE_BIT (offset 64) */
            if (ecdt->length >= 65) {
                ecdt_gpe_bit = raw[64];
                kprint("ACPI: ECDT GPE_BIT=%u\n", (uint32_t)ecdt_gpe_bit);
            }

            /* Extract EC_ID string (offset 65+) */
            if (ecdt->length > 65) {
                const char *ecid = (const char *)(raw + 65);
                kprint("ACPI: ECDT EC_ID=\"%s\"\n", ecid);
            }

            kprint("ACPI: ECDT candidate ports data=0x%02x cmd=0x%02x gpe=%u\n",
                   (uint32_t)ecdt_data_port, (uint32_t)ecdt_cmd_port, (uint32_t)ecdt_gpe_bit);
        }
    } else {
        kprint("ACPI: ECDT not found (len=%u)\n", ecdt ? ecdt->length : 0);
    }

    /*
     * Scan every PNP0C09 device and pick the strongest EC candidate.
     * This avoids "first match wins" errors on firmware with multiple EC-like
     * nodes or placeholder devices.
     */
    struct ec_scan_ctx scan;
    scan.count = 0;
    ns_walk(collect_ec_candidates_cb, &scan);

    /* Inject ECDT as a candidate if it provides valid ports (G446) */
    if (ecdt_ports_valid && scan.count < EC_MAX_CANDIDATES) {
        struct ec_candidate *ecdt_cand = &scan.cands[scan.count];
        ecdt_cand->node = 0;
        ecdt_cand->data_port = ecdt_data_port;
        ecdt_cand->cmd_port = ecdt_cmd_port;
        ecdt_cand->has_crs_ports = 1;
        ecdt_cand->sta_known = 0;
        ecdt_cand->present = 1;
        ecdt_cand->has_reg = 0;
        ecdt_cand->has_ini = 0;
        ecdt_cand->has_gpe = 0;
        ecdt_cand->source = 1; /* ECDT */
        ecdt_cand->gpe_num = ecdt_gpe_bit;
        ecdt_cand->score = 0;
        /* ECDT gets moderate score: present + has_crs_ports */
        ecdt_cand->score += 16; /* present */
        ecdt_cand->score += 8;  /* has ports */
        if (ecdt_cand->gpe_num != 0xFF) ecdt_cand->score += 2; /* has GPE */
        scan.count++;
        kprint("ACPI: EC cand[%u] src=ECDT data=0x%02x cmd=0x%02x gpe=%u score=%d\n",
               scan.count - 1, (uint32_t)ecdt_data_port, (uint32_t)ecdt_cmd_port,
               (uint32_t)ecdt_gpe_bit, ecdt_cand->score);
    }

    /* Inject default fallback candidate if no others found (G446) */
    if (scan.count == 0) {
        struct ec_candidate *fb = &scan.cands[0];
        fb->node = 0;
        fb->data_port = 0x62;
        fb->cmd_port = 0x66;
        fb->has_crs_ports = 1;
        fb->sta_known = 0;
        fb->present = 1;
        fb->has_reg = 0;
        fb->has_ini = 0;
        fb->has_gpe = 0;
        fb->source = 0; /* fallback */
        fb->gpe_num = 0xFF;
        fb->score = 10; /* low score: fallback only */
        scan.count = 1;
        kprint("ACPI: EC cand[0] src=fallback data=0x62 cmd=0x66 score=10\n");
    }

    /*
     * Sort candidates by score (descending) so we try the best one first.
     */
    g_ec_cand_count = (uint8_t)scan.count;
    for (uint32_t i = 0; i < scan.count; i++) {
        for (uint32_t j = i + 1; j < scan.count; j++) {
            if (scan.cands[j].score > scan.cands[i].score) {
                struct ec_candidate tmp = scan.cands[i];
                scan.cands[i] = scan.cands[j];
                scan.cands[j] = tmp;
            }
        }
    }

    /* Log all candidates */
    for (uint32_t i = 0; i < scan.count; i++) {
        char p[256];
        const char *src_str = "fallback";
        if (scan.cands[i].source == 1) src_str = "ECDT";
        else if (scan.cands[i].source == 2) src_str = "CRS";
        if (scan.cands[i].node) {
            ns_build_path(scan.cands[i].node, p, sizeof(p));
        } else {
            p[0] = '-'; p[1] = 0;
        }
        kprint("ACPI: EC cand[%u] src=%s %s score=%d present=%u crs=%u reg=%u gpe=%u ini=%u\n",
               i, src_str, p, (uint32_t)scan.cands[i].score,
               (uint32_t)scan.cands[i].present,
               (uint32_t)scan.cands[i].has_crs_ports,
               (uint32_t)scan.cands[i].has_reg,
               (uint32_t)scan.cands[i].has_gpe,
               (uint32_t)scan.cands[i].has_ini);
    }

    /*
     * Step 1: Ensure LPC decode is enabled for EC ports (once, before
     * iterating candidates). This writes PCI config IOE and mirrors to
     * PCR[DMI] sideband on Intel.
     */
    ec_enable_lpc_decode();

    /* fry446: eSPI channel initialization — bring up PC/VW channels before
     * any EC probe attempt. Without PCRDY, I/O cycles to 0x62/0x66 are NOT
     * tunneled to the EC via eSPI and return 0xFF (floating bus). */
    espi_channel_init();

    /* fry435: proactive eSPI W1C error clear before EC probe */
    espi_clear_errors(0);

    /*
     * G449: Per-Candidate Bring-Up State Machine
     *
     * For each candidate in priority order:
     *   1. ec_setup_ports(data, cmd)
     *   2. _REG(3,1) if node has it
     *   3. _INI if present
     *   4. Enable _GPE + install EC GPE handler
     *   5. Direct probe
     *   6. Recovery methods 2-7 if probe fails
     *   7. If all fail → next candidate
     */
    for (uint32_t ci = 0; ci < scan.count; ci++) {
        struct ec_candidate *cand = &scan.cands[ci];
        const char *src_str = "fallback";
        if (cand->source == 1) src_str = "ECDT";
        else if (cand->source == 2) src_str = "CRS";

        data_port = cand->has_crs_ports ? cand->data_port : 0x62u;
        cmd_port = cand->has_crs_ports ? cand->cmd_port : 0x66u;
        ec_node = cand->node;

        if (ec_node) {
            g_ec_node_found = 1;
        }
        g_ec_ports_source = cand->source;

        kprint("ACPI: EC cand[%u] src=%s trying ports 0x%02x/0x%02x\n",
               ci, src_str, (uint32_t)data_port, (uint32_t)cmd_port);

        /* Checkpoint: post-LPC status at this candidate's port */
        g_ec_post_lpc_sts = inb(cmd_port);

        /* Step 2: Set up ports and enable ACPI mode */
        ec_setup_ports(data_port, cmd_port);
        g_ec_post_setup_sts = inb(cmd_port);

        /* Step 3: Pre-_REG status read (diagnostic, no commands) */
        g_ec_pre_reg_sts = inb(cmd_port);
        if (g_ec_pre_reg_sts != 0xFF) {
            g_ec_pre_reg_probe_ok = 1;
        }

        /* Step 4: ACPI handshake — _REG → _INI → _GPE */
        if (ec_node) {
            /* _REG(3,1) */
            if (has_method3(ec_node, "REG")) {
                struct acpi_object *reg_args[2];
                struct acpi_object *reg_ret = 0;
                reg_args[0] = aml_obj_make_int(3);
                reg_args[1] = aml_obj_make_int(1);
                ec_set_reg_in_progress(1);
                if (invoke_path_method(ec_node, "REG", reg_args, 2, &reg_ret) == 0) {
                    g_ec_reg_called = 1;
                    kprint("ACPI: EC cand[%u] _REG(3,1) invoked\n", ci);
                } else {
                    kprint("ACPI: EC cand[%u] _REG(3,1) failed\n", ci);
                }
                ec_set_reg_in_progress(0);
            }

            /* _INI */
            if (has_method3(ec_node, "INI")) {
                struct acpi_object *ini_ret = 0;
                if (invoke_path_method(ec_node, "INI", 0, 0, &ini_ret) == 0) {
                    g_ec_ini_found = 1;
                    kprint("ACPI: EC cand[%u] _INI invoked\n", ci);
                }
            }

            /* _GPE — install EC GPE handler (G449 new) */
            {
                struct acpi_object *gpe_obj = 0;
                uint32_t gpe_num = 0;
                int have_gpe = 0;

                if (eval_path_obj(ec_node, "GPE", &gpe_obj) == 0 &&
                    gpe_obj && gpe_obj->type == AML_OBJ_INTEGER) {
                    gpe_num = (uint32_t)obj_to_int(gpe_obj);
                    have_gpe = 1;
                } else if (cand->gpe_num != 0xFF) {
                    gpe_num = cand->gpe_num;
                    have_gpe = 1;
                }

                if (have_gpe) {
                    g_ec_gpe_found = 1;
                    g_ec_gpe_num = (uint8_t)(gpe_num & 0xFFu);
                    /* Install EC GPE handler for _Qxx dispatch */
                    acpi_install_gpe_handler(gpe_num, ec_gpe_handler, 0);
                    acpi_enable_gpe(gpe_num);
                    kprint("ACPI: EC cand[%u] GPE=%u handler installed + enabled\n",
                           ci, gpe_num);
                    /* Set EC namespace node for _Qxx dispatch */
                    ec_set_ns_node(ec_node);
                } else {
                    g_ec_gpe_found = 0;
                    g_ec_gpe_num = 0xFF;
                    kprint("ACPI: EC cand[%u] no GPE found\n", ci);
                }
            }
        } else if (cand->gpe_num != 0xFF) {
            /* ECDT or fallback candidate with GPE */
            g_ec_gpe_found = 1;
            g_ec_gpe_num = cand->gpe_num;
            acpi_install_gpe_handler(cand->gpe_num, ec_gpe_handler, 0);
            acpi_enable_gpe(cand->gpe_num);
            kprint("ACPI: EC cand[%u] GPE=%u (ECDT) handler installed\n",
                   ci, (uint32_t)cand->gpe_num);
        }

        /* Step 5: Probe sequence — try direct then recovery methods */
        ec_reset_fail_count();

        /* Method 1: Direct probe */
        kprint("ACPI: EC cand[%u] probe: direct\n", ci);
        if (ec_probe() == 0) {
            g_ec_recovery_method = 1;
            /* G450: update candidate score */
            cand->score += 100;
            kprint("ACPI: EC cand[%u] src=%s probe=ok (direct)\n", ci, src_str);
            goto ec_cand_success;
        }
        /* G450: status-based scoring */
        {
            uint8_t sts = inb(cmd_port);
            if (sts != 0xFF) cand->score += 10;
        }

        /* Fast bail: 0xFF = floating bus / no eSPI tunneling.
         * Recovery methods 2-6 can't fix missing hardware. */
        if (ec_last_probe_stsff()) {
            kprint("ACPI: EC cand[%u] src=%s STS_FF — skip recovery methods\n", ci, src_str);
            continue;
        }

        /* Method 2: Burst mode */
        kprint("ACPI: EC cand[%u] recovery: burst (method 2)\n", ci);
        ec_reset_state();
        ec_reset_fail_count();
        if (ec_burst_enable()) {
            if (ec_probe() == 0) {
                ec_burst_disable();
                g_ec_recovery_method = 3;
                cand->score += 100;
                kprint("ACPI: EC cand[%u] src=%s probe=ok (burst)\n", ci, src_str);
                goto ec_cand_success;
            }
            ec_burst_disable();
        }

        /* Method 3: SCI drain */
        kprint("ACPI: EC cand[%u] recovery: SCI drain (method 3)\n", ci);
        ec_sci_query_drain();
        ec_reset_state();
        ec_reset_fail_count();
        if (ec_probe() == 0) {
            g_ec_recovery_method = 4;
            cand->score += 100;
            kprint("ACPI: EC cand[%u] src=%s probe=ok (sci_drain)\n", ci, src_str);
            goto ec_cand_success;
        }

        /* Method 4: Force ACPI_ENABLE */
        kprint("ACPI: EC cand[%u] recovery: force ACPI_ENABLE (method 4)\n", ci);
        if (ec_force_acpi_enable()) {
            ec_reset_state();
            ec_reset_fail_count();
            if (ec_probe() == 0) {
                g_ec_recovery_method = 5;
                cand->score += 100;
                kprint("ACPI: EC cand[%u] src=%s probe=ok (force_en)\n", ci, src_str);
                goto ec_cand_success;
            }
        }

        /* Method 5: Force ACPI_ENABLE + Burst */
        kprint("ACPI: EC cand[%u] recovery: force_en + burst (method 5)\n", ci);
        ec_reset_state();
        ec_reset_fail_count();
        if (ec_burst_enable()) {
            if (ec_probe() == 0) {
                ec_burst_disable();
                g_ec_recovery_method = 6;
                cand->score += 100;
                kprint("ACPI: EC cand[%u] src=%s probe=ok (force+burst)\n", ci, src_str);
                goto ec_cand_success;
            }
            ec_burst_disable();
        }

        /* Method 6: eSPI error clear + probe */
        kprint("ACPI: EC cand[%u] recovery: eSPI clear (method 6)\n", ci);
        espi_clear_errors(1);
        espi_settle_delay(ESPI_CLEAR_SETTLE_LOOPS);
        ec_reset_state();
        ec_reset_fail_count();
        if (ec_probe() == 0) {
            g_ec_recovery_method = 7;
            cand->score += 100;
            kprint("ACPI: EC cand[%u] src=%s probe=ok (espi_clear)\n", ci, src_str);
            goto ec_cand_success;
        }

        /* All methods failed for this candidate */
        kprint("ACPI: EC cand[%u] src=%s probe=FAIL (all methods exhausted)\n", ci, src_str);
        continue;

ec_cand_success:
        /* Start EC query worker thread (G451) */
        ec_query_worker_start();
        goto ec_recovery_done;
    }

    /* No candidate succeeded */
    g_ec_recovery_method = 0;
    kprint("ACPI: EC all candidates failed, EC not available\n");

ec_recovery_done:
    /* Run eSPI diagnostics after EC bring-up */
    espi_diag_probe();
}

void acpi_extended_init(void) {
    acpi_pci_prt_init();
    acpi_pci_osc_init();
    ec_reg_init();
    acpi_battery_init();
    acpi_backlight_init();

    struct fry_process *t = process_create_kernel(thermal_thread, 0, "thermal");
    if (t) {
        sched_add(t->pid);
    }
}
