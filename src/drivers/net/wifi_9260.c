// Intel 9260 WiFi driver: NIC reset + TLV FW parse + DMA rings for Dell Precision 7530

#include <stdint.h>
#include <stdarg.h>
#include "../pci/pci.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/heap.h"
#include "../pci/msi.h"
#include "../../kernel/irq/manage.h"
#include "../../drivers/irqchip/lapic.h"
#include "../../shared/wifi_abi.h"
#include "iwlwifi_fw.h"
#include "netcore.h"
#include "iwl_cmd.h"
#include "iwl_mac80211.h"
#include "iwl_wpa2.h"
#include "wifi_boot_test_config.h"
#include "../../kernel/acpi/tables.h"

void kprint(const char *fmt, ...);

#define IWL_VENDOR_INTEL            0x8086u
#define IWL_DEVICE_THUNDER_PEAK     0x2526u
#define IWL_SUBSYS_DELL_7530_9260   0x40108086u
#define IWL_MMIO_SIZE               0x8000u   /* 32 KB of CSR space */
#define IWL_VTD_REG_WINDOW_SIZE     0x1000u

/* Intel VT-d remapping hardware registers */
#define IWL_DMAR_GCMD_REG           0x18u
#define IWL_DMAR_GSTS_REG           0x1Cu
#define IWL_DMAR_GCMD_TE            (1u << 31)
#define IWL_DMAR_GSTS_TES           (1u << 31)

/* ---- CSR register offsets (from Linux iwl-csr.h) ---- */
#define CSR_HW_IF_CONFIG_REG    0x000
#define CSR_INT_COALESCING      0x004   /* interrupt coalescing timer (8-bit, 32-usec units) */
#define CSR_INT                 0x008
#define CSR_INT_MASK            0x00C
#define CSR_FH_INT_STATUS       0x010
#define CSR_GPIO_IN             0x018
#define CSR_RESET               0x020
#define CSR_GP_CNTRL            0x024
#define CSR_HW_REV              0x028
#define CSR_EEPROM_REG          0x02C
#define CSR_GIO_REG             0x03C
#define CSR_CTXT_INFO_BA        0x040
#define CSR_MBOX_SET_REG        0x088
#define CSR_GIO_CHICKEN_BITS    0x100
#define CSR_ANA_PLL_CFG         0x20C
#define CSR_HW_REV_STEP_DASH    0x22C
#define CSR_DBG_HPET_MEM_REG    0x240
#define CSR_DBG_LINK_PWR_MGMT_REG 0x250
#define CSR_HW_RF_ID            0x09C

/* CSR_RESET bits */
#define CSR_RESET_REG_FLAG_NEVO_RESET       (1u << 0)   /* 0x01 */
#define CSR_RESET_REG_FLAG_FORCE_NMI        (1u << 1)   /* 0x02 */
#define CSR_RESET_REG_FLAG_SW_RESET         (1u << 7)   /* 0x80 */
#define CSR_RESET_REG_FLAG_MASTER_DISABLED  (1u << 8)   /* 0x100 */
#define CSR_RESET_REG_FLAG_STOP_MASTER      (1u << 9)   /* 0x200 */
#define CSR_RESET_LINK_PWR_MGMT_DISABLED    0x80000000u

/* CSR_GP_CNTRL bits */
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY   (1u << 0)
#define CSR_GP_CNTRL_REG_FLAG_AUTO_FUNC_BOOT    (1u << 1)
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE         (1u << 2)
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ    (1u << 3)
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP    (1u << 4)

/* CSR_HW_IF_CONFIG_REG bits */
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY          0x00400000u
#define CSR_HW_IF_CONFIG_REG_PCI_OWN_SET            0x00400000u
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE   0x02000000u
#define CSR_HW_IF_CONFIG_REG_WAKE_ME                0x08000000u
#define CSR_HW_IF_CONFIG_REG_PREPARE                0x08000000u
#define CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH      0x0000000Fu
#define CSR_HW_IF_CONFIG_REG_BIT_MAC_SI             0x00000100u
#define CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI           0x00000200u
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE           0x00000C00u
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH           0x00003000u
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP           0x0000C000u
#define CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE           10u
#define CSR_HW_IF_CONFIG_REG_POS_PHY_DASH           12u
#define CSR_HW_IF_CONFIG_REG_POS_PHY_STEP           14u
#define CSR_HW_REV_STEP_DASH_VAL(_v) \
    ((_v) & CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH)

/* CSR_GIO_CHICKEN_BITS */
#define CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX  (1u << 23)
#define CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER (1u << 29)
#define CSR_GIO_REG_VAL_L0S_DISABLED                 0x00000002u

/* APMG PRPH registers (Linux iwl-prph.h) */
#define APMG_BASE                            0x00003000u
#define APMG_PS_CTRL_REG                     (APMG_BASE + 0x000Cu)
#define APMG_PS_CTRL_MSK_PWR_SRC             0x03000000u
#define APMG_PS_CTRL_VAL_PWR_SRC_VMAIN       0x00000000u
#define APMG_PS_CTRL_VAL_PWR_SRC_VAUX        0x02000000u

/* CSR_HW_RF_ID masks */
#define CSR_HW_RF_ID_TYPE_MASK      0x001F0000u
#define CSR_HW_RF_ID_STEP_MASK      0x00003000u

/* PHY configuration bits carried in IWL_UCODE_TLV_PHY_SKU */
#define FW_PHY_CFG_RADIO_TYPE_POS   0u
#define FW_PHY_CFG_RADIO_TYPE       (0x3u << FW_PHY_CFG_RADIO_TYPE_POS)
#define FW_PHY_CFG_RADIO_STEP_POS   2u
#define FW_PHY_CFG_RADIO_STEP       (0x3u << FW_PHY_CFG_RADIO_STEP_POS)
#define FW_PHY_CFG_RADIO_DASH_POS   4u
#define FW_PHY_CFG_RADIO_DASH       (0x3u << FW_PHY_CFG_RADIO_DASH_POS)

/* Flow-handler interrupt bits */
#define CSR_INT_BIT_ALIVE         (1u << 0)
#define CSR_INT_BIT_WAKEUP        (1u << 1)
#define CSR_INT_BIT_RESET_DONE    (1u << 2)
#define CSR_INT_BIT_SW_RX         (1u << 3)
#define CSR_INT_BIT_RF_KILL       (1u << 7)
#define CSR_INT_BIT_SW_ERR        (1u << 25)
#define CSR_INT_BIT_FH_TX         (1u << 27)
#define CSR_INT_BIT_RX_PERIODIC   (1u << 28)
#define CSR_INT_BIT_HW_ERR        (1u << 29)
#define CSR_INT_BIT_FH_RX         (1u << 31)
#define CSR_INI_SET_MASK          (CSR_INT_BIT_FH_RX       | \
                                   CSR_INT_BIT_HW_ERR      | \
                                   CSR_INT_BIT_FH_TX       | \
                                   CSR_INT_BIT_SW_ERR      | \
                                   CSR_INT_BIT_RF_KILL     | \
                                   CSR_INT_BIT_SW_RX       | \
                                   CSR_INT_BIT_WAKEUP      | \
                                   CSR_INT_BIT_RESET_DONE  | \
                                   CSR_INT_BIT_ALIVE       | \
                                   CSR_INT_BIT_RX_PERIODIC)
#define CSR_FH_INT_BIT_ERR          (1u << 31)
#define CSR_FH_INT_BIT_TX_CHNL1     (1u << 1)

/* Interrupt coalescing default: 0x40 = 64 * 32us = 2048us (matches Linux) */
#define IWL_HOST_INT_TIMEOUT_DEF    0x40
#define CSR_FH_INT_BIT_TX_CHNL0     (1u << 0)
#define CSR_FH_INT_TX_MASK          (CSR_FH_INT_BIT_TX_CHNL1 | CSR_FH_INT_BIT_TX_CHNL0)

/* Mailbox bits */
#define CSR_MBOX_SET_REG_OS_ALIVE   (1u << 5)

/* Additional CSR registers (gen2 init) */
#define CSR_UCODE_DRV_GP1_CLR      0x05C
#define CSR_UCODE_SW_BIT_RFKILL              0x00000002u
#define CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED    0x00000004u
#define CSR_MAC_SHADOW_REG_CTRL    0x0A8
#define CSR_HW_IF_CONFIG_REG_HAP_WAKE   0x00080000u
#define CSR_DBG_HPET_MEM_REG_VAL        0xFFFF0000u

/* UREG CPU init kick register (prph space, gen2 9000+) */
#define UREG_CPU_INIT_RUN          0xa05c44u

/* CPU status registers (prph) — read on ALIVE timeout to determine if FW is running */
#define SB_CPU_1_STATUS            0xA01E30u
#define SB_CPU_2_STATUS            0xA01E34u
#define UREG_UMAC_CURRENT_PC       0xA05C18u
#define UREG_LMAC1_CURRENT_PC      0xA05C1Cu
#define WFPM_ARC1_PD_NOTIFICATION  0xA03044u
#define WFPM_LMAC1_PD_NOTIFICATION 0xA0338Cu
#define HPM_SECONDARY_DEVICE_STATE 0xA03404u
#define WFPM_MAC_OTP_CFG7_ADDR     0xA03338u
#define WFPM_MAC_OTP_CFG7_DATA     0xA0333Cu
#define FSEQ_ERROR_CODE            0xA340C8u
#define FSEQ_TOP_INIT_VERSION      0xA34038u
#define FSEQ_CNVIO_INIT_VERSION    0xA3403Cu
#define FSEQ_OTP_VERSION           0xA340FCu
#define FSEQ_TOP_CONTENT_VERSION   0xA340F4u
#define FSEQ_ALIVE_TOKEN           0xA340F0u
#define FSEQ_CNVI_ID               0xA3408Cu
#define FSEQ_CNVR_ID               0xA34090u
#define FSEQ_PREV_CNVIO_INIT_VERSION 0xA34084u
#define FSEQ_WIFI_FSEQ_VERSION     0xA34040u
#define FSEQ_BT_FSEQ_VERSION       0xA34044u
#define FSEQ_CLASS_TP_VERSION      0xA34078u

#define IWL_UMAC_ERROR_TABLE_BASE  0x00800000u
#define IWL_UMAC_ERROR_TABLE_VALID 0x0000deadu
#define FW_SYSASSERT_CPU_MASK      0xF0000000u
#define FW_SYSASSERT_PNVM_MISSING  0x0010070Du

/* Timeout poll iterations (~1 us per outb 0x80 loop) */
#define IWL_POLL_TIMEOUT_US     25000u  /* 25 ms */
#define IWL_RESET_SETTLE_US     5000u   /* 5 ms after SW reset */
#define IWL_HW_READY_TIMEOUT_US 50u
#define IWL_PREPARE_HW_POLL_US  150000u
#define IWL_PREPARE_HW_DELAY_US 500u
#define IWL_PREPARE_HW_RETRY_DELAY_US 25000u

/* ---- FH / TFH registers ---- */
#define FH_MEM_LOWER_BOUND         0x1000

/* 9260 firmware upload uses FH service channel 9, not TFH_SRV_DMA. */
#define FH_SRVC_CHNL               9u
#define FH_SRVC_LOWER_BOUND        (FH_MEM_LOWER_BOUND + 0x9C8u)
#define FH_SRVC_CHNL_SRAM_ADDR_REG(_chnl) \
    (FH_SRVC_LOWER_BOUND + (((_chnl) - FH_SRVC_CHNL) * 0x4u))

#define FH_TFDIB_LOWER_BOUND       (FH_MEM_LOWER_BOUND + 0x900u)
#define FH_TFDIB_CTRL0_REG(_chnl)  (FH_TFDIB_LOWER_BOUND + 0x8u * (_chnl))
#define FH_TFDIB_CTRL1_REG(_chnl)  (FH_TFDIB_LOWER_BOUND + 0x8u * (_chnl) + 0x4u)
#define FH_MEM_TFDIB_REG1_ADDR_BITSHIFT 28u

#define FH_TCSR_LOWER_BOUND        (FH_MEM_LOWER_BOUND + 0xD00u)
#define FH_TCSR_CHNL_TX_CONFIG_REG(_chnl) \
    (FH_TCSR_LOWER_BOUND + 0x20u * (_chnl))
#define FH_TCSR_CHNL_TX_BUF_STS_REG(_chnl) \
    (FH_TCSR_LOWER_BOUND + 0x20u * (_chnl) + 0x8u)
#define FH_TCSR_TX_CONFIG_REG_VAL_MSG_MODE_DRV     0x00000001u
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE 0x00000000u
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD 0x00100000u
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE   0x00000000u
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE  0x80000000u
#define FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID 0x00000003u
#define FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX     12u
#define FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM     20u

/* TFH TX path registers (for runtime TX after firmware is alive) */
#define TFH_TFDQ_CBB_TABLE         0x1C00  /* TFD circular buffer base (gen2) */
#define TFH_TSSR_TX_STATUS         0x1EA0  /* TX status register */

/* Gen1 TX queue registration (9000-series uses FH_MEM_CBBC, NOT TFH_TFDQ_CBB_TABLE) */
#define FH_KW_MEM_ADDR_REG         (FH_MEM_LOWER_BOUND + 0x97Cu)  /* 0x197C: keep-warm buf */
#define FH_MEM_CBBC_QUEUE_0        (FH_MEM_LOWER_BOUND + 0x9D0u)  /* 0x19D0: queue 0 TFD base */

/* SCD (Scheduler) registers — PRPH space */
#define SCD_BASE                    0xA02C00u
#define SCD_TXFACT                  (SCD_BASE + 0x10u)    /* TX FIFO activate */
#define SCD_GP_CTRL                 (SCD_BASE + 0x1A8u)   /* scheduler control */
#define SCD_GP_CTRL_AUTO_ACTIVE_MODE  (1u << 18)
#define SCD_GP_CTRL_ENABLE_31_QUEUES (1u << 0)
#define SCD_QUEUE_STTS_REG_POS_ACTIVE    3u
#define SCD_QUEUE_STTS_REG_POS_SCD_ACT_EN 19u
#define SCD_QUEUE_STATUS_BITS(q)    ((q) < 20u ? \
                                     (SCD_BASE + 0x10Cu + (q) * 4u) : \
                                     (SCD_BASE + 0x334u + ((q) - 20u) * 4u))

/* Number of FH TX channels to stop before firmware start (Linux: FH_TCSR_CHNL_NUM) */
#define FH_TCSR_CHNL_NUM            10u
#define FH_TSSR_TX_STATUS_REG       (FH_MEM_LOWER_BOUND + 0xEA0u)
#define FH_TSSR_TX_STATUS_REG_MSK_CHNL_IDLE(_chnl) (((1u << (_chnl)) << 16))

/* SRAM read/write via HBUS target memory window (CSR space) */
#define HBUS_TARG_MEM_RADDR     0x40Cu
#define HBUS_TARG_MEM_RDAT      0x41Cu
#define HBUS_TARG_MEM_WADDR     0x410u
#define HBUS_TARG_MEM_WDAT      0x418u
#define HBUS_TARG_MBX_C         0x030u  /* RB timeout for 9000-series */
#define IWL_9000_RB_TIMEOUT     0x64u   /* 128us @ 25MHz osc (100 * 32 * 40ns) */

/* 9000-specific: persistence bit (must be cleared before sw_reset) */
#define HPM_DEBUG                       0xA03440u  /* UMAC prph */
#define HPM_DEBUG_PERSISTENCE_BIT       (1u << 12) /* 0x1000 */
#define PREG_PRPH_WPROT_9000            0xA04CE0u  /* write-protection register */
#define PREG_WFPM_ACCESS                (1u << 12) /* guard bit in wprot */

/* 9000-specific: RFKILL wake via L1A (not enabled by default on 9000+) */
#define CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN  (1u << 26) /* 0x04000000 */

/* 9000-series RX uses RFH (Receive Frame Handler) in PRPH space,
   NOT the old FH CSR registers.  All accessed via iwl_write_prph(). */
#define RFH_Q0_FRBDCB_BA_LSB       0xA08000  /* Free ring base addr LSB */
#define RFH_Q0_FRBDCB_BA_MSB       0xA08004  /* Free ring base addr MSB */
#define RFH_Q0_FRBDCB_WIDX         0xA08080  /* Free ring write index */
#define RFH_Q0_FRBDCB_RIDX         0xA080C0  /* Free ring read index */
#define RFH_Q0_URBDCB_BA_LSB       0xA08100  /* Used ring base addr LSB */
#define RFH_Q0_URBDCB_BA_MSB       0xA08104  /* Used ring base addr MSB */
#define RFH_Q0_URBDCB_WIDX         0xA08180  /* Used ring write index (device) */
#define RFH_Q0_URBD_STTS_WPTR_LSB  0xA08200  /* Status writeback addr LSB */
#define RFH_Q0_URBD_STTS_WPTR_MSB  0xA08204  /* Status writeback addr MSB */
#define RFH_RXF_DMA_CFG            0xA09820  /* RFH DMA config */
#define RFH_GEN_CFG                0xA09800  /* RFH general config */
#define RFH_GEN_STATUS             0xA09808  /* RFH general status */
#define RFH_RXF_RXQ_ACTIVE         0xA0980C  /* RX queue active bitmask */

/* RFH DMA config bits */
#define RFH_DMA_EN_VAL              (1u << 31)
#define RFH_RXF_DMA_RB_SIZE_4K     (4u << 16)  /* enumerated: 4=4K in bits [19:16] */
#define RFH_RXF_DMA_RBDCB_SIZE_512 (9u << 20)  /* log2(512) in bits [23:20] */
#define RFH_RXF_DMA_MIN_RB_4_8     (3u << 24)  /* min 4 RBs before IRQ */
#define RFH_RXF_DMA_DROP_TOO_LARGE (1u << 26)
#define RFH_RXF_DMA_SINGLE_FRAME   (1u << 29)

/* RFH general config bits */
#define RFH_GEN_CFG_SERVICE_DMA_SNOOP (1u << 0)
#define RFH_GEN_CFG_RFH_DMA_SNOOP    (1u << 1)
#define RFH_GEN_CFG_RB_CHUNK_SIZE_128 (1u << 4)  /* 128-byte chunks for PCIe */

/* RFH write-index trigger (CSR space doorbell — kicks RFH DMA engine) */
#define RFH_Q_FRBDCB_WIDX_TRG(q)     (0x1C80 + (q) * 4)

/* RFH status bits */
#define RFH_GEN_STATUS_DMA_IDLE     (1u << 31)

/* ---- TFD (Transmit Frame Descriptor) structures ---- */
/* Gen2 TFD: each entry describes one DMA transfer (up to 25 scatter-gather TBs) */
#define IWL_TFD_TB_MAX          25   /* max transfer buffers per TFD */
#define IWL_TFD_QUEUE_SIZE      256  /* entries per TFD queue */
#define IWL_CMD_QUEUE           0    /* queue 0 = firmware/command queue */

/* Transfer Buffer descriptor (TB) — 12 bytes each */
struct iwl_tfd_tb {
    uint32_t lo;    /* physical address low 32 bits */
    uint16_t hi;    /* physical address bits [47:32] */
    uint16_t len;   /* transfer length in bytes */
} __attribute__((packed));

/* Gen2 TFD — 68 bytes (4 byte header + 25 * 12 byte TBs), padded to 256 bytes */
struct iwl_tfd {
    uint8_t  num_tbs;   /* number of valid TBs (bits [4:0]) */
    uint8_t  rsvd[3];
    struct iwl_tfd_tb tbs[IWL_TFD_TB_MAX];
} __attribute__((packed));

/* RX Buffer descriptor — 8 bytes.
 * On 9000-series MQ RX, the low 12 bits carry a non-zero VID and the upper
 * bits carry the 4K-aligned DMA address. */
struct iwl_rx_bd {
    uint64_t addr;  /* physical address of the receive buffer */
} __attribute__((packed));

/* RX completion status — written by device after each RX */
struct iwl_rx_status {
    uint16_t closed_rb_num;   /* index of last closed RB */
    uint16_t closed_fr_num;   /* index of last closed frame */
    uint16_t finished_rb_num; /* index of last finished RB */
    uint16_t finished_fr_num; /* index of last finished frame */
    uint32_t spare;
} __attribute__((packed));

/* ---- Context Information for 9000-series gen2 boot ROM ---- */
/* Boot ROM reads this struct from DMA memory to self-load firmware
   and learn where the RX/TX queues are.  CSR offset 0x40 holds the phys addr. */
#define IWL_MAX_DRAM_ENTRY      64
#define CPU1_CPU2_SEPARATOR     0xFFFFCCCCu
#define PAGING_SEPARATOR        0xAAAABBBBu

/* Control flag bits */
#define CTXT_INFO_TFD_FORMAT_LONG   0x0100u    /* bit 8 per Linux iwlwifi (bit 0 is AUTO_FUNC_INIT) */
#define CTXT_INFO_RB_SIZE_POS       9
#define CTXT_INFO_RB_SIZE_4K        0x4u       /* 4K = 0x4 per Linux IWL_CTXT_INFO_RB_SIZE_4K */
#define CTXT_INFO_RB_CB_SIZE_POS    4

struct iwl_ctxt_info_version {
    uint16_t mac_id;
    uint16_t version;
    uint16_t size;          /* sizeof(iwl_context_info) / 4 */
    uint16_t reserved;
} __attribute__((packed));

struct iwl_ctxt_info_control {
    uint32_t control_flags;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_ctxt_info_rbd_cfg {
    uint64_t free_rbd_addr;     /* FRBDCB phys */
    uint64_t used_rbd_addr;     /* URBDCB phys */
    uint64_t status_wr_ptr;     /* RX status writeback phys */
} __attribute__((packed));

struct iwl_ctxt_info_hcmd_cfg {
    uint64_t cmd_queue_addr;    /* TFD ring phys */
    uint8_t  cmd_queue_size;    /* TFD_QUEUE_CB_SIZE = ilog2(n)-3 */
    uint8_t  reserved[7];
} __attribute__((packed));

struct iwl_ctxt_info_dump_cfg {
    uint64_t addr;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_ctxt_info_dram {
    uint64_t umac_img[IWL_MAX_DRAM_ENTRY];
    uint64_t lmac_img[IWL_MAX_DRAM_ENTRY];
    uint64_t virtual_img[IWL_MAX_DRAM_ENTRY];
} __attribute__((packed));

struct iwl_context_info {
    struct iwl_ctxt_info_version version;       /* +0x000:    8 */
    struct iwl_ctxt_info_control control;       /* +0x008:    8 */
    uint64_t reserved0;                         /* +0x010:    8 */
    struct iwl_ctxt_info_rbd_cfg rbd_cfg;       /* +0x018:   24 */
    struct iwl_ctxt_info_hcmd_cfg hcmd_cfg;     /* +0x030:   16 */
    uint32_t reserved1[4];                      /* +0x040:   16 */
    struct iwl_ctxt_info_dump_cfg dump_cfg;     /* +0x050:   16 */
    struct iwl_ctxt_info_dump_cfg edbg_cfg;     /* +0x060:   16  (early debug, zeroed) */
    struct iwl_ctxt_info_dump_cfg pnvm_cfg;     /* +0x070:   16  (platform NVM, zeroed) */
    uint32_t reserved2[16];                     /* +0x080:   64 */
    struct iwl_ctxt_info_dram dram;             /* +0x0C0: 1536 */
    uint32_t reserved3[16];                     /* +0x6C0:   64 */
} __attribute__((packed));                      /* total:  1792 */

/* Context info + FW section DMA state (allocated in iwl_start_fw_with_ctxt_info) */
static struct iwl_context_info *g_iwl_ctxt_info;
static uint64_t g_iwl_ctxt_info_phys;

#define IWL_CTXT_INFO_MIN_PHYS      0x00200000ULL
#define IWL_CTXT_CANARY0_OFF        0x700u
#define IWL_CTXT_CANARY1_OFF        0x7FCu
#define IWL_CTXT_CANARY0_VAL        0x43545830u  /* "CTX0" */
#define IWL_CTXT_CANARY1_VAL        0x43545831u  /* "CTX1" */

#define IWL_MAX_FW_DMA_SEC  32
static struct {
    uint64_t phys[IWL_MAX_FW_DMA_SEC];
    uint32_t size[IWL_MAX_FW_DMA_SEC];
    uint32_t count;
} g_iwl_fw_dma;

/* ---- DMA ring state ---- */
#define IWL_RX_QUEUE_SIZE       512
#define IWL_RX_BUF_SIZE         4096  /* one page per RX buffer */

/* Allocation layout (all below 4 GB for DMA):
   Pages 0-15:    TFD ring (256 entries * 256 bytes = 64 KB)
   Page 16:       Byte-count table (256 * 2 bytes = 512 bytes)
   Page 17:       RX status buffer
   Page 18:       RX BD ring (FRBDCB, 512 * 8 bytes = 4 KB)
   Page 19:       RX used ring (URBDCB, 512 * 4 bytes = 2 KB)
   Pages 20-531:  RX data buffers (512 * 4 KB)
   Pages 532-535: Command buffers (4 * 4 KB for host command payloads)
   Pages 536-537: Firmware upload bounce buffer (8 KB, page/16-byte aligned)
   Page 538:      Keep-warm buffer (NIC power management)
   Total: 539 pages
*/

#define IWL_DMA_TFD_PAGES       16   /* 64 KB for 256 TFDs */
#define IWL_DMA_BC_PAGES        1    /* byte-count table */
#define IWL_DMA_RXSTATUS_PAGES  1    /* RX status */
#define IWL_DMA_RXBD_PAGES      1    /* RX free ring buffer descriptors (FRBDCB) */
#define IWL_DMA_RXUSED_PAGES    1    /* RX used ring (URBDCB, 512 * 4 bytes) */
#define IWL_DMA_RXBUF_PAGES     512  /* 512 RX data buffers */
#define IWL_DMA_CMDBUF_PAGES    4    /* 4 command buffers */
#define IWL_DMA_FW_BOUNCE_PAGES 2    /* 8 KB FH service-channel bounce buffer */
#define IWL_DMA_KW_PAGES        1    /* keep-warm buffer */
#define IWL_DMA_TOTAL_PAGES     (IWL_DMA_TFD_PAGES + IWL_DMA_BC_PAGES + \
                                 IWL_DMA_RXSTATUS_PAGES + IWL_DMA_RXBD_PAGES + \
                                 IWL_DMA_RXUSED_PAGES + IWL_DMA_RXBUF_PAGES + \
                                 IWL_DMA_CMDBUF_PAGES + \
                                 IWL_DMA_FW_BOUNCE_PAGES + \
                                 IWL_DMA_KW_PAGES)  /* = 539 */

/* Byte-count table entry (gen2): 16 bits per entry */
struct iwl_bc_entry {
    uint16_t bc;  /* byte count for this TFD */
} __attribute__((packed));

/* Global DMA state */
static struct {
    /* Physical base of the entire DMA allocation */
    uint64_t dma_phys_base;

    /* TFD ring */
    struct iwl_tfd *tfd_ring;       /* virtual ptr */
    uint64_t        tfd_ring_phys;  /* physical addr */
    uint16_t        tfd_write_idx;  /* next TFD to fill */

    /* Byte-count table */
    struct iwl_bc_entry *bc_table;
    uint64_t             bc_table_phys;

    /* RX free ring buffer descriptors (FRBDCB) */
    struct iwl_rx_bd *rxbd_ring;
    uint64_t          rxbd_ring_phys;
    uint16_t          rx_write_idx;

    /* RX used ring (URBDCB) — device writes completion entries (uint32_t per entry) */
    uint32_t *rx_used_ring;
    uint64_t  rx_used_ring_phys;

    /* RX status (device writes here) */
    struct iwl_rx_status *rx_status;
    uint64_t              rx_status_phys;

    /* RX data buffer physical addresses (for building BDs) */
    uint64_t rx_buf_phys[IWL_RX_QUEUE_SIZE];
    uint8_t *rx_buf_virt[IWL_RX_QUEUE_SIZE];

    /* Command buffers (for host command TX) */
    uint8_t *cmd_buf_virt[IWL_DMA_CMDBUF_PAGES];
    uint64_t cmd_buf_phys[IWL_DMA_CMDBUF_PAGES];
    uint8_t  cmd_buf_idx; /* next command buffer to use (round-robin) */

    /* Firmware upload bounce buffer for FH service DMA. */
    uint8_t *fw_bounce_virt;
    uint64_t fw_bounce_phys;

    /* Keep-warm buffer (NIC power management) */
    uint64_t kw_phys;

    /* RX read index for tracking consumed responses */
    uint16_t rx_read_idx;

    uint8_t allocated;
} g_iwl_dma;

struct iwl_dmar_table {
    struct acpi_sdt_header hdr;
    uint8_t host_addr_width;
    uint8_t flags;
    uint8_t reserved[10];
} __attribute__((packed));

struct iwl_dmar_remap_header {
    uint16_t type;
    uint16_t length;
} __attribute__((packed));

struct iwl_dmar_drhd {
    uint16_t type;
    uint16_t length;
    uint8_t flags;
    uint8_t reserved;
    uint16_t segment;
    uint64_t register_base;
} __attribute__((packed));

/* ---- MMIO helpers ---- */
static volatile uint8_t *g_iwl_mmio;

/* ---- Debug log ring buffer (readable from userspace via SYS_WIFI_DEBUG) ---- */
#define IWL_DBG_BUF_INIT 16384   /* initial capacity */
static char  *g_iwl_dbg_buf;     /* heap-allocated debug log */
static uint32_t g_iwl_dbg_cap;   /* current capacity */
static uint32_t g_iwl_dbg_pos;   /* write cursor */

/* Persistent boot breadcrumbs for bare-metal wd diagnostics. */
#define IWL_BOOTDBG_RFKILL_CLEARED    (1u << 0)
#define IWL_BOOTDBG_SHADOW_ENABLED    (1u << 1)
#define IWL_BOOTDBG_CTXT_INFO_WRITTEN (1u << 2)
#define IWL_BOOTDBG_CTXT_INFO_READ    (1u << 3)
#define IWL_BOOTDBG_CPU_KICK_WRITTEN  (1u << 4)
#define IWL_BOOTDBG_CPU_KICK_READ     (1u << 5)
#define IWL_BOOTDBG_WFPM_WRITTEN      (1u << 6)
#define IWL_BOOTDBG_WFPM_READ         (1u << 7)
#define IWL_BOOTDBG_CPU_RESET_WRITTEN (1u << 8)
#define IWL_BOOTDBG_CPU_RESET_READ    (1u << 9)
#define IWL_BOOTDBG_CPU_RESET_RETRY   (1u << 10)
#define IWL_BOOTDBG_CHICKEN_SET       (1u << 11)
#define IWL_BOOTDBG_CHICKEN_FALLBACK  (1u << 12)
#define IWL_BOOTDBG_CHICKEN_FAIL      (1u << 13)

struct iwl_bootdbg_fw_pass {
    uint32_t start_idx;
    uint32_t break_idx;
    uint32_t break_off;
    uint32_t loaded;
    uint32_t first_off;
    uint32_t last_off;
    uint32_t done_status;
};

#define IWL_SRAM_PROBE_WORDS 16
#define IWL_SRAM_EXT_START   0x00040000u
#define IWL_SRAM_EXT_END     0x00057FFFu
#define IWL_VERIFY_SCAN_START 0x00800000u
#define IWL_VERIFY_SCAN_END   0x00810000u
#define IWL_VERIFY_SCAN_MAX   8u
struct iwl_sram_probe_entry {
    uint32_t sec_idx;
    uint32_t cpu_id;
    uint32_t addr;
    uint32_t size;
    uint32_t extended;
    uint32_t word_count;
    uint32_t staged_valid;
    uint32_t expected[IWL_SRAM_PROBE_WORDS];
    uint32_t staged[IWL_SRAM_PROBE_WORDS];
};

static struct {
    uint32_t flags;
    uint32_t rfkill_mask;
    uint32_t shadow_ctrl;
    uint32_t secure_cpu1_hdr_readback;
    uint32_t secure_cpu2_hdr_readback;
    uint32_t ctxt_info_write_lo;
    uint32_t ctxt_info_write_hi;
    uint32_t ctxt_info_read_lo;
    uint32_t ctxt_info_read_hi;
    uint32_t cpu_init_run_written;
    uint32_t cpu_init_run_readback;
    uint32_t load_int_mask;
    uint32_t alive_int_mask;
    uint32_t wfpm_gp2_written;
    uint32_t wfpm_gp2_readback;
    uint32_t release_cpu_reset_readback;
    uint32_t chicken_before;
    uint32_t chicken_after;
    uint32_t chicken_want;
    uint32_t fw_upload_is_init;
    uint32_t fw_sec_total;
    struct iwl_bootdbg_fw_pass fw_cpu1;
    struct iwl_bootdbg_fw_pass fw_cpu2;
    uint32_t rfh_frbdcb_ba_lsb;
    uint32_t rfh_urbdcb_ba_lsb;
    uint32_t rfh_status_wptr_lsb;
    uint32_t rfh_widx_trg_written;
    uint32_t fh_load_status_after_upload;
    uint32_t fw_sram_probe_count;
    struct iwl_sram_probe_entry fw_sram_probe[IWL_MAX_SEC_SECTIONS];
} g_iwl_bootdbg;

#define IWL_BOOT_TRACE_MAX 32
struct iwl_boot_trace_entry {
    const char *tag;
    uint32_t step;
    int16_t rc;
    uint16_t reserved;
    uint32_t irq_count;
    uint32_t int_mask;
    uint32_t csr_int;
    uint32_t fh_int;
    uint32_t gp_cntrl;
    uint32_t csr_reset;
    uint32_t fh_load;
    uint32_t wfpm_gp2;
    uint32_t cpu_reset;
    uint32_t rfh_act;
    uint32_t rfh_w;
    uint32_t rfh_r;
    uint32_t rfh_u;
    uint32_t cpu1;
    uint32_t cpu2;
};
static struct iwl_boot_trace_entry g_iwl_boot_trace[IWL_BOOT_TRACE_MAX];
static uint32_t g_iwl_boot_trace_count;
static volatile uint32_t g_iwl_last_fh_int;
static uint8_t g_iwl_init_step;

#define IWL_CMD_TRACE_MAX 128
enum iwl_cmd_trace_kind {
    IWL_CMD_TRACE_SEND = 1,
    IWL_CMD_TRACE_SEND_FAIL = 2,
    IWL_CMD_TRACE_FIRE = 3,
    IWL_CMD_TRACE_RESP = 4,
    IWL_CMD_TRACE_ASYNC = 5,
    IWL_CMD_TRACE_TIMEOUT = 6,
    IWL_CMD_TRACE_PHASE = 7
};

#define IWL_CMD_TRACE_F_WANT_RESP (1u << 0)

enum iwl_cmd_trace_phase {
    IWL_CMD_PHASE_INIT_ALIVE_OK = 1,
    IWL_CMD_PHASE_INIT_ALIVE_FAIL = 2,
    IWL_CMD_PHASE_INIT_ALIVE_REKICK = 3,
    IWL_CMD_PHASE_INIT_ALIVE_TIMEOUT = 4,
    IWL_CMD_PHASE_INIT_ALIVE_SW_ERR = 5,
    IWL_CMD_PHASE_INIT_ALIVE_HW_ERR = 6,
    IWL_CMD_PHASE_INIT_ALIVE_BAD_STATUS = 7,
    IWL_CMD_PHASE_INIT_PREP_ENTER = 8,
    IWL_CMD_PHASE_INIT_PREP_OK = 9,
    IWL_CMD_PHASE_INIT_PREP_FAIL = 10,
    IWL_CMD_PHASE_INIT_WAIT_ENTER = 11,
    IWL_CMD_PHASE_INIT_WAIT_OK = 12,
    IWL_CMD_PHASE_INIT_WAIT_FAIL = 13,
    IWL_CMD_PHASE_PHY_DB_ENTER = 14,
    IWL_CMD_PHASE_PHY_DB_OK = 15,
    IWL_CMD_PHASE_PHY_DB_FAIL = 16,
    IWL_CMD_PHASE_RT_BOOT_ENTER = 17,
    IWL_CMD_PHASE_RT_ALIVE_OK = 18,
    IWL_CMD_PHASE_RT_ALIVE_FAIL = 19,
    IWL_CMD_PHASE_RT_ALIVE_REKICK = 20,
    IWL_CMD_PHASE_RT_ALIVE_TIMEOUT = 21,
    IWL_CMD_PHASE_RT_ALIVE_SW_ERR = 22,
    IWL_CMD_PHASE_RT_ALIVE_HW_ERR = 23,
    IWL_CMD_PHASE_RT_ALIVE_BAD_STATUS = 24,
    IWL_CMD_PHASE_HCMD_ENTER = 25,
    IWL_CMD_PHASE_HCMD_OK = 26,
    IWL_CMD_PHASE_HCMD_FAIL = 27
};

struct iwl_cmd_trace_entry {
    uint32_t ord;
    uint8_t kind;
    uint8_t step;
    uint8_t flags;
    int8_t rc;
    uint8_t cmd_id;
    uint8_t group_id;
    uint16_t seq;
    uint16_t tx_len;
    uint16_t timeout_ms;
    uint8_t rx_cmd;
    uint8_t rx_group;
    uint16_t rx_seq;
    uint16_t rx_len;
    uint16_t rx_slot;
    uint16_t closed_rb;
    uint16_t async_seen;
    uint16_t reserved;
    uint32_t csr_int;
    uint32_t fh_int;
};
static struct iwl_cmd_trace_entry g_iwl_cmd_trace[IWL_CMD_TRACE_MAX];
static uint32_t g_iwl_cmd_trace_count;
static uint32_t g_iwl_cmd_trace_next;
static uint32_t g_iwl_cmd_trace_ord;

static uint32_t iwl_read32(uint32_t off);
static void iwl_cache_flush(void *buf, uint64_t len);
static void iwl_udelay(uint32_t us);

static struct iwl_cmd_trace_entry *iwl_cmd_trace_add(uint8_t kind, uint8_t flags, int rc,
                                                     uint8_t cmd_id, uint8_t group_id, uint16_t seq,
                                                     uint16_t tx_len, uint16_t timeout_ms,
                                                     uint8_t rx_cmd, uint8_t rx_group, uint16_t rx_seq,
                                                     uint16_t rx_len, uint16_t rx_slot,
                                                     uint16_t closed_rb, uint16_t async_seen,
                                                     uint32_t csr_int) {
    struct iwl_cmd_trace_entry *e = &g_iwl_cmd_trace[g_iwl_cmd_trace_next];
    *e = (struct iwl_cmd_trace_entry){0};
    e->ord = ++g_iwl_cmd_trace_ord;
    e->kind = kind;
    e->step = g_iwl_init_step;
    e->flags = flags;
    e->rc = (rc < -128) ? -128 : (rc > 127 ? 127 : (int8_t)rc);
    e->cmd_id = cmd_id;
    e->group_id = group_id;
    e->seq = seq;
    e->tx_len = tx_len;
    e->timeout_ms = timeout_ms;
    e->rx_cmd = rx_cmd;
    e->rx_group = rx_group;
    e->rx_seq = rx_seq;
    e->rx_len = rx_len;
    e->rx_slot = rx_slot;
    e->closed_rb = closed_rb;
    e->async_seen = async_seen;
    e->csr_int = csr_int;
    e->fh_int = g_iwl_last_fh_int;

    g_iwl_cmd_trace_next = (g_iwl_cmd_trace_next + 1u) % IWL_CMD_TRACE_MAX;
    if (g_iwl_cmd_trace_count < IWL_CMD_TRACE_MAX)
        g_iwl_cmd_trace_count++;
    return e;
}

static uint16_t iwl_cmd_trace_closed_rb_snapshot(void) {
    if (!g_iwl_dma.allocated || !g_iwl_dma.rx_status)
        return 0;

    iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
    return g_iwl_dma.rx_status->closed_rb_num & 0x0FFFu;
}

static void iwl_cmd_trace_mark_phase_state(uint8_t phase, int rc, uint16_t arg,
                                           uint16_t elapsed_ms,
                                           uint32_t csr_int, uint32_t fh_int) {
    struct iwl_cmd_trace_entry *e =
        iwl_cmd_trace_add(IWL_CMD_TRACE_PHASE, 0, rc,
                          phase, 0, arg, 0, elapsed_ms,
                          0, 0, 0, 0, 0,
                          iwl_cmd_trace_closed_rb_snapshot(), 0,
                          csr_int);
    e->fh_int = fh_int;
}

static void iwl_cmd_trace_mark_phase(uint8_t phase, int rc, uint16_t arg) {
    iwl_cmd_trace_mark_phase_state(phase, rc, arg, 0,
                                   g_iwl_mmio ? iwl_read32(CSR_INT) : 0,
                                   g_iwl_mmio ? iwl_read32(CSR_FH_INT_STATUS) : g_iwl_last_fh_int);
}

static const char *iwl_cmd_trace_phase_name(uint8_t phase) {
    switch (phase) {
        case IWL_CMD_PHASE_INIT_ALIVE_OK: return "init-alive-ok";
        case IWL_CMD_PHASE_INIT_ALIVE_FAIL: return "init-alive-fail";
        case IWL_CMD_PHASE_INIT_ALIVE_REKICK: return "init-alive-rekick";
        case IWL_CMD_PHASE_INIT_ALIVE_TIMEOUT: return "init-alive-timeout";
        case IWL_CMD_PHASE_INIT_ALIVE_SW_ERR: return "init-alive-sw-err";
        case IWL_CMD_PHASE_INIT_ALIVE_HW_ERR: return "init-alive-hw-err";
        case IWL_CMD_PHASE_INIT_ALIVE_BAD_STATUS: return "init-alive-bad-status";
        case IWL_CMD_PHASE_INIT_PREP_ENTER: return "init-prep-enter";
        case IWL_CMD_PHASE_INIT_PREP_OK: return "init-prep-ok";
        case IWL_CMD_PHASE_INIT_PREP_FAIL: return "init-prep-fail";
        case IWL_CMD_PHASE_INIT_WAIT_ENTER: return "init-wait-enter";
        case IWL_CMD_PHASE_INIT_WAIT_OK: return "init-wait-ok";
        case IWL_CMD_PHASE_INIT_WAIT_FAIL: return "init-wait-fail";
        case IWL_CMD_PHASE_PHY_DB_ENTER: return "phy-db-enter";
        case IWL_CMD_PHASE_PHY_DB_OK: return "phy-db-ok";
        case IWL_CMD_PHASE_PHY_DB_FAIL: return "phy-db-fail";
        case IWL_CMD_PHASE_RT_BOOT_ENTER: return "rt-boot-enter";
        case IWL_CMD_PHASE_RT_ALIVE_OK: return "rt-alive-ok";
        case IWL_CMD_PHASE_RT_ALIVE_FAIL: return "rt-alive-fail";
        case IWL_CMD_PHASE_RT_ALIVE_REKICK: return "rt-alive-rekick";
        case IWL_CMD_PHASE_RT_ALIVE_TIMEOUT: return "rt-alive-timeout";
        case IWL_CMD_PHASE_RT_ALIVE_SW_ERR: return "rt-alive-sw-err";
        case IWL_CMD_PHASE_RT_ALIVE_HW_ERR: return "rt-alive-hw-err";
        case IWL_CMD_PHASE_RT_ALIVE_BAD_STATUS: return "rt-alive-bad-status";
        case IWL_CMD_PHASE_HCMD_ENTER: return "hcmd-enter";
        case IWL_CMD_PHASE_HCMD_OK: return "hcmd-ok";
        case IWL_CMD_PHASE_HCMD_FAIL: return "hcmd-fail";
        default: return "?";
    }
}

static void iwl_dbg_grow(void) {
    uint32_t newcap = g_iwl_dbg_cap * 2;
    char *nb = (char *)krealloc(g_iwl_dbg_buf, newcap);
    if (!nb) return;              /* OOM — stop growing, keep what we have */
    g_iwl_dbg_buf = nb;
    g_iwl_dbg_cap = newcap;
}

static void iwl_dbg_append(const char *s) {
    while (*s) {
        if (g_iwl_dbg_pos >= g_iwl_dbg_cap - 1)
            iwl_dbg_grow();
        if (g_iwl_dbg_pos >= g_iwl_dbg_cap - 1)
            break;                /* grow failed — truncate */
        g_iwl_dbg_buf[g_iwl_dbg_pos++] = *s++;
    }
    if (g_iwl_dbg_buf)
        g_iwl_dbg_buf[g_iwl_dbg_pos] = '\0';
}

/* Mini snprintf for the debug buffer — supports %s %u %d %x %08x %04x %02x %llx */
static void iwl_dbg_fmt(char *buf, int bufsz, const char *fmt, va_list ap) {
    int bi = 0;
    for (const char *p = fmt; *p && bi < bufsz - 1; p++) {
        if (*p != '%') { buf[bi++] = *p; continue; }
        p++;
        int zero_pad = 0, width = 0;
        int is_long_long = 0;
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        if (*p == 'l' && *(p+1) == 'l') { is_long_long = 1; p += 2; }
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && bi < bufsz - 1) buf[bi++] = *s++;
        } else if (*p == 'u') {
            uint32_t v = va_arg(ap, uint32_t);
            char tmp[12]; int ti = 0;
            if (v == 0) { tmp[ti++] = '0'; }
            else { while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; } }
            while (ti < width) tmp[ti++] = zero_pad ? '0' : ' ';
            for (int j = ti - 1; j >= 0 && bi < bufsz - 1; j--) buf[bi++] = tmp[j];
        } else if (*p == 'd') {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) { buf[bi++] = '-'; v = -v; }
            char tmp[12]; int ti = 0;
            if (v == 0) { tmp[ti++] = '0'; }
            else { while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; } }
            while (ti < width) tmp[ti++] = zero_pad ? '0' : ' ';
            for (int j = ti - 1; j >= 0 && bi < bufsz - 1; j--) buf[bi++] = tmp[j];
        } else if (*p == 'x') {
            uint64_t v = is_long_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            char tmp[17]; int ti = 0;
            if (v == 0) { tmp[ti++] = '0'; }
            else { while (v) { tmp[ti++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } }
            while (ti < width) tmp[ti++] = zero_pad ? '0' : ' ';
            for (int j = ti - 1; j >= 0 && bi < bufsz - 1; j--) buf[bi++] = tmp[j];
        } else if (*p == '%') {
            buf[bi++] = '%';
        }
    }
    buf[bi] = '\0';
}

/* Log to both serial (kprint) and the debug ring buffer */
__attribute__((format(printf, 1, 2)))
void iwlwifi_dbg(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    iwl_dbg_fmt(tmp, (int)sizeof(tmp), fmt, ap);
    va_end(ap);
    kprint("%s", tmp);
    iwl_dbg_append(tmp);
}

#define iwl_dbg iwlwifi_dbg

static uint32_t iwl_read32(uint32_t off) {
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(g_iwl_mmio + off);
    return *reg;
}

static void iwl_write32(uint32_t off, uint32_t val) {
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(g_iwl_mmio + off);
    *reg = val;
}

static void iwl_write8(uint32_t off, uint8_t val) {
    volatile uint8_t *reg = (volatile uint8_t *)(uintptr_t)(g_iwl_mmio + off);
    *reg = val;
}

static void iwl_write64_split(uint32_t off, uint64_t val) {
    iwl_write32(off, (uint32_t)(val & 0xFFFFFFFFu));
    iwl_write32(off + 4u, (uint32_t)(val >> 32));
}

static void iwl_set_bit(uint32_t off, uint32_t bits) {
    iwl_write32(off, iwl_read32(off) | bits);
}

/* Used in FW upload (step 4) and runtime */
static void iwl_clear_bit(uint32_t off, uint32_t bits) __attribute__((unused));
static void iwl_clear_bit(uint32_t off, uint32_t bits) {
    iwl_write32(off, iwl_read32(off) & ~bits);
}

/* ~1 us busy-wait via port 0x80 write */
static void iwl_udelay(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) {
        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
    }
}

/* Poll until (reg & mask) == expected, or timeout. Returns 0 on success. */
static int iwl_poll_bit(uint32_t off, uint32_t mask, uint32_t expected, uint32_t timeout_us) {
    for (uint32_t i = 0; i < timeout_us; i++) {
        uint32_t v = iwl_read32(off);
        if (v == 0xFFFFFFFFu)
            return -1;
        if ((v & mask) == expected)
            return 0;
        iwl_udelay(1);
    }
    return -1;
}

static uint32_t iwl_full_int_mask(void) {
    return CSR_INI_SET_MASK;
}

static void iwl_disable_interrupts(void) {
    iwl_write32(CSR_INT_MASK, 0x00000000u);
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    iwl_write32(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
}

static void iwl_enable_interrupts(void) {
    iwl_write32(CSR_INT_MASK, CSR_INI_SET_MASK);
}

static void iwl_enable_rfkill_int(void) {
    iwl_write32(CSR_INT_MASK, CSR_INT_BIT_RF_KILL);

    /* 9000-series discrete 9260 needs this wake bit armed explicitly. */
    iwl_set_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN);
}

static void iwl_enable_fw_load_int(void) {
    /* Direct FH upload only needs FH_TX while the service channel is active. */
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    iwl_write32(CSR_INT_MASK, CSR_INT_BIT_FH_TX);
    g_iwl_bootdbg.load_int_mask = CSR_INT_BIT_FH_TX;
}

static void iwl_enable_fw_alive_ints(void) {
    /* Keep Linux's init mask across the upload-to-ALIVE handoff.
       Narrowing here created a blind window between the runtime done marker
       and the later ALIVE wait poll loop, while the hardware still selects
       the ALIVE/SW_ERR boundary in that interval. */
    uint32_t alive_mask = iwl_full_int_mask();
    iwl_write32(CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    iwl_write32(CSR_INT_MASK, alive_mask);
    g_iwl_bootdbg.alive_int_mask = alive_mask;
}

/*
 * Context-info boot only works when AUTO_FUNC_BOOT is armed.
 * Multiple fry64x logs showed the bit regressing back to 0, which matches
 * the bare-metal GP_CNTRL=...0d dump and leaves the gen2 boot ROM idle.
 */
static void iwl_arm_ctxt_info_boot(void) {
    iwl_set_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_AUTO_FUNC_BOOT);
    iwl_udelay(10);
}

/*
 * iwl_pcie_set_hw_ready - claim PCI ownership exactly like Linux.
 */
static int iwl_pcie_set_hw_ready(void) {
    int ret;

    iwl_set_bit(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PCI_OWN_SET);

    ret = iwl_poll_bit(CSR_HW_IF_CONFIG_REG,
                       CSR_HW_IF_CONFIG_REG_PCI_OWN_SET,
                       CSR_HW_IF_CONFIG_REG_PCI_OWN_SET,
                       IWL_HW_READY_TIMEOUT_US);
    if (ret == 0)
        iwl_set_bit(CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);

    iwl_dbg("IWL9260: hardware%s ready\n", ret ? " not" : "");
    return ret;
}

/*
 * iwl_pcie_prepare_card_hw - Linux 7.0-rc2 ownership handshake.
 */
static int iwl_pcie_prepare_card_hw(void) {
    int ret;
    int iter;

    ret = iwl_pcie_set_hw_ready();
    if (!ret)
        return 0;

    iwl_set_bit(CSR_DBG_LINK_PWR_MGMT_REG, CSR_RESET_LINK_PWR_MGMT_DISABLED);
    iwl_udelay(1500);

    for (iter = 0; iter < 10; iter++) {
        uint32_t t;

        iwl_set_bit(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_WAKE_ME);

        for (t = 0; t < IWL_PREPARE_HW_POLL_US; t += IWL_PREPARE_HW_DELAY_US) {
            ret = iwl_pcie_set_hw_ready();
            if (!ret)
                return 0;

            iwl_udelay(IWL_PREPARE_HW_DELAY_US);
        }

        iwl_udelay(IWL_PREPARE_HW_RETRY_DELAY_US);
    }

    iwl_dbg("IWL9260: couldn't prepare the card HW_IF=%08x GP=%08x RST=%08x\n",
            iwl_read32(CSR_HW_IF_CONFIG_REG),
            iwl_read32(CSR_GP_CNTRL),
            iwl_read32(CSR_RESET));
    return ret;
}

/* ---- PCI helpers ---- */
static uint64_t iwl_bar0_phys(const struct pci_device_info *dev) {
    if (!dev) return 0;
    uint32_t bar0 = dev->bar0;
    if (bar0 == 0 || (bar0 & 0x1u)) return 0;

    uint64_t phys = (uint64_t)(bar0 & ~0xFULL);
    uint32_t type = (bar0 >> 1) & 0x3u;
    if (type == 0x2u) {
        phys |= ((uint64_t)dev->bar1 << 32);
    }
    return phys;
}

static uint8_t iwl_is_target_device(const struct pci_device_info *dev) {
    if (!dev) return 0;
    if (dev->vendor_id != IWL_VENDOR_INTEL) return 0;
    if (dev->class_code != 0x02 || dev->subclass != 0x80) return 0;
    return dev->device_id == IWL_DEVICE_THUNDER_PEAK;
}

static uint16_t iwl_read_cfg16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off) {
    uint32_t v = pci_ecam_read32(0, bus, slot, func, off & ~3u);
    uint32_t shift = (uint32_t)(off & 2u) * 8u;
    return (uint16_t)((v >> shift) & 0xFFFFu);
}

static void iwl_write_cfg16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint16_t value) {
    pci_ecam_write8(0, bus, slot, func, off, (uint8_t)(value & 0xFFu));
    pci_ecam_write8(0, bus, slot, func, (uint16_t)(off + 1u), (uint8_t)(value >> 8));
}

/* ---- Step 1: NIC Reset Sequence ---- */

/* Full software reset of the NIC */
static void iwl_sw_reset(void) {
    iwl_set_bit(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
    iwl_udelay(IWL_RESET_SETTLE_US);
}

static void iwl_pcie_apm_config(void) {
    iwl_set_bit(CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);
}

static int iwl_pcie_gen1_2_activate_nic(void) {
    int rc;

    iwl_set_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    rc = iwl_poll_bit(CSR_GP_CNTRL,
                      CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                      CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                      IWL_POLL_TIMEOUT_US);
    return rc;
}

/* APM (Autonomous Power Management) init for the 9260 / 9000-family path.
 * Ported directly from Linux 7.0-rc2 iwl_pcie_apm_init() + iwl_pcie_apm_config()
 * + iwl_pcie_gen1_2_activate_nic(), specialized for FAMILY_9000 (fry713).
 *
 * Key ordering vs. previous code (fry712):
 *   - L0S_DISABLED (apm_config) now set BEFORE INIT_DONE, matching Linux
 *   - Simple iwl_set_bit() calls like Linux
 *   - No DIS_L0S_EXIT_TIMER (correct: device_family >= 8000)
 *   - No APMG clock/L1/RFKILL (apmg_not_supported=true)
 *   - No OSC_CLK (host_interrupt_operation_mode=false)
 *   - No ANA_PLL_CFG (pll_cfg=false)
 */
static int iwl_apm_init(void) {
    int ret;

    g_iwl_bootdbg.chicken_before = iwl_read32(CSR_GIO_CHICKEN_BITS);
    iwl_set_bit(CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);
    iwl_set_bit(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
    iwl_set_bit(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_HAP_WAKE);

    g_iwl_bootdbg.chicken_after = iwl_read32(CSR_GIO_CHICKEN_BITS);
    g_iwl_bootdbg.chicken_want = CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX;
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_CHICKEN_SET;

    iwl_pcie_apm_config();

    ret = iwl_pcie_gen1_2_activate_nic();
    if (ret) {
        kprint("IWL9260: MAC clock ready timeout GP=0x%08x CHICKEN=0x%08x HW_IF=0x%08x\n",
               iwl_read32(CSR_GP_CNTRL),
               iwl_read32(CSR_GIO_CHICKEN_BITS),
               iwl_read32(CSR_HW_IF_CONFIG_REG));
        return -1;
    }

    return 0;
}

/* Forward declarations needed for persistence bit clearing */
static int iwl_grab_nic_access(void);
static void iwl_release_nic_access(void);
static void iwl_write_prph(uint32_t addr, uint32_t val);
static uint32_t iwl_read_prph(uint32_t addr);

/* Linux iwl_pcie_set_pwr(): keep the NIC on VMAIN during firmware load.
 * 9260: apmg_not_supported=true → this is a NO-OP.
 * Linux checks this flag first and returns early.  Previous code (pre-fry713)
 * was reading/writing APMG_PS_CTRL_REG on a device that doesn't support it,
 * which could cause hangs or undefined behavior. */
static void iwl_pcie_set_pwr(int vaux) {
    /* 9260 (apmg_not_supported=true): nothing to do */
    (void)vaux;
}

/* 9000-specific: clear persistence bit before sw_reset (fry710 BUG A).
 * Linux: iwl_trans_pcie_clear_persistence_bit() in pcie/gen1_2/trans.c.
 * If UEFI left this bit set, firmware sees stale power management state
 * and may SYSASSERT during early self-check. */
static void iwl_clear_persistence_bit(void) {
    /* Need NIC access for prph reads/writes.  grab_nic_access sets
     * MAC_ACCESS_REQ — the subsequent sw_reset will clear it anyway. */
    if (iwl_grab_nic_access() != 0) {
        kprint("IWL9260: persistence: grab_nic_access failed (non-fatal)\n");
        return;
    }

    uint32_t hpm = iwl_read_prph(HPM_DEBUG);
    if (hpm & HPM_DEBUG_PERSISTENCE_BIT) {
        uint32_t wprot = iwl_read_prph(PREG_PRPH_WPROT_9000);
        if (wprot & PREG_WFPM_ACCESS) {
            kprint("IWL9260: persistence bit set but write-protected (wprot=0x%08x)\n", wprot);
            return;
        }
        iwl_write_prph(HPM_DEBUG, hpm & ~HPM_DEBUG_PERSISTENCE_BIT);
        kprint("IWL9260: cleared persistence bit (HPM_DEBUG was 0x%08x)\n", hpm);
    } else {
        iwl_dbg("IWL9260: persistence bit already clear (HPM_DEBUG=0x%08x)\n", hpm);
    }
    /* No explicit release — sw_reset follows immediately and resets everything */
}

/* Full NIC reset + bring-up to a known state.
 * Ported from Linux 7.0-rc2 _iwl_trans_pcie_start_hw(), specialized for
 * FAMILY_9000 (fry713).
 *
 * Key changes vs. previous code:
 *   - NO iwl_apm_stop_master() — Linux doesn't call this in start_hw
 *   - NO explicit MAC_ACCESS_REQ — Linux obtains MAC access per-use via
 *     grab_nic_access, not as a blanket step after APM init
 *   - NO interrupt disable here — moved to boot_attempt (start_fw path)
 *   - RFKILL_WAKE_L1A_EN set after APM init (matches iwl_enable_rfkill_int)
 */
static int _iwl_trans_pcie_start_hw(void) {
    /* Read HW revision before reset for diagnostics */
    uint32_t hw_rev = iwl_read32(CSR_HW_REV);
    uint32_t hw_rev2 = iwl_read32(CSR_HW_REV_STEP_DASH);
    uint32_t rf_id = iwl_read32(CSR_HW_RF_ID);
    kprint("IWL9260: pre-reset HW_REV=0x%08x STEP_DASH=0x%08x RF_ID=0x%08x\n",
           hw_rev, hw_rev2, rf_id);

    int rc = iwl_pcie_prepare_card_hw();
    if (rc != 0) {
        kprint("IWL9260: prepare_card_hw (pre-reset) FAILED\n");
        return -3;
    }

    iwl_clear_persistence_bit();

    iwl_sw_reset();
    rc = iwl_pcie_prepare_card_hw();
    if (rc != 0) {
        kprint("IWL9260: prepare_card_hw (post-reset) FAILED\n");
        return -3;
    }

    rc = iwl_apm_init();
    if (rc != 0)
        return rc;

    iwl_enable_rfkill_int();

    /* Re-read HW revision after reset */
    hw_rev = iwl_read32(CSR_HW_REV);
    kprint("IWL9260: start_hw OK HW_REV=0x%08x GP_CNTRL=0x%08x\n",
           hw_rev, iwl_read32(CSR_GP_CNTRL));
    return 0;
}

/* ---- Step 3: DMA Ring Setup ---- */

static void iwl_zero(void *ptr, uint64_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < len; i++) p[i] = 0;
}

static void iwl_copy(void *dst, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static void iwl_write32_mem(void *base, uint32_t off, uint32_t val) {
    uint8_t *p = (uint8_t *)base + off;
    p[0] = (uint8_t)(val & 0xFFu);
    p[1] = (uint8_t)((val >> 8) & 0xFFu);
    p[2] = (uint8_t)((val >> 16) & 0xFFu);
    p[3] = (uint8_t)((val >> 24) & 0xFFu);
}

static uint32_t iwl_read32_mem(const void *base, uint32_t off) {
    const uint8_t *p = (const uint8_t *)base + off;
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int iwl_mac_is_zero(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return 0;
    }
    return 1;
}

static void iwl_cache_flush(void *buf, uint64_t len) {
    if (!buf || len == 0) return;
    uintptr_t start = (uintptr_t)buf;
    uintptr_t end = start + (uintptr_t)len;
    uintptr_t p = start & ~(uintptr_t)63u;
    while (p < end) {
        __asm__ volatile("clflush (%0)" : : "r"((void *)p) : "memory");
        p += 64u;
    }
    __asm__ volatile("mfence" : : : "memory");
}

static void iwl_dma_reset_state(void) {
    if (!g_iwl_dma.allocated)
        return;

    iwl_zero(g_iwl_dma.tfd_ring, (uint64_t)IWL_DMA_TFD_PAGES * 4096u);
    iwl_zero(g_iwl_dma.bc_table, (uint64_t)IWL_DMA_BC_PAGES * 4096u);
    iwl_zero(g_iwl_dma.rx_status, (uint64_t)IWL_DMA_RXSTATUS_PAGES * 4096u);
    iwl_zero(g_iwl_dma.rxbd_ring, (uint64_t)IWL_DMA_RXBD_PAGES * 4096u);
    iwl_zero(g_iwl_dma.rx_used_ring, (uint64_t)IWL_DMA_RXUSED_PAGES * 4096u);
    for (uint32_t i = 0; i < IWL_RX_QUEUE_SIZE; i++)
        iwl_zero(g_iwl_dma.rx_buf_virt[i], 4096u);
    for (uint32_t i = 0; i < IWL_DMA_CMDBUF_PAGES; i++)
        iwl_zero(g_iwl_dma.cmd_buf_virt[i], 4096u);
    iwl_zero(g_iwl_dma.fw_bounce_virt, (uint64_t)IWL_DMA_FW_BOUNCE_PAGES * 4096u);
    iwl_zero((void *)(uintptr_t)vmm_phys_to_virt(g_iwl_dma.kw_phys), 4096u);

    g_iwl_dma.tfd_write_idx = 0;
    g_iwl_dma.rx_write_idx = 0;
    g_iwl_dma.rx_read_idx = 0;
    g_iwl_dma.cmd_buf_idx = 0;
}

/* Best-effort VT-d shutdown so the NIC can DMA to physical addresses directly. */
static int iwl_disable_intel_iommu(void) {
    struct iwl_dmar_table *dmar = (struct iwl_dmar_table *)acpi_find_table("DMAR");
    if (!dmar) return 0;
    if (dmar->hdr.length < sizeof(*dmar)) {
        iwl_dbg("IWL9260: DMAR length too small (%u)\n", dmar->hdr.length);
        return -1;
    }

    uint8_t *p = (uint8_t *)dmar + sizeof(*dmar);
    uint8_t *end = (uint8_t *)dmar + dmar->hdr.length;
    int drhd_count = 0;
    int disabled = 0;
    int already_off = 0;
    int failures = 0;

    while (p + sizeof(struct iwl_dmar_remap_header) <= end) {
        struct iwl_dmar_remap_header *rh = (struct iwl_dmar_remap_header *)p;
        if (rh->length < sizeof(*rh) || p + rh->length > end) {
            iwl_dbg("IWL9260: DMAR entry truncated type=%u len=%u\n",
                    rh->type, rh->length);
            break;
        }

        if (rh->type == 0 && rh->length >= sizeof(struct iwl_dmar_drhd)) {
            struct iwl_dmar_drhd *drhd = (struct iwl_dmar_drhd *)p;
            uint64_t base = drhd->register_base;
            if (base) {
                vmm_ensure_physmap_uc(base + IWL_VTD_REG_WINDOW_SIZE);
                volatile uint32_t *gcmd =
                    (volatile uint32_t *)(uintptr_t)vmm_phys_to_virt(base + IWL_DMAR_GCMD_REG);
                volatile uint32_t *gsts =
                    (volatile uint32_t *)(uintptr_t)vmm_phys_to_virt(base + IWL_DMAR_GSTS_REG);
                uint32_t gcmd_before = *gcmd;
                uint32_t gsts_before = *gsts;
                uint32_t gsts_after = gsts_before;

                drhd_count++;
                if ((gsts_before & IWL_DMAR_GSTS_TES) == 0) {
                    already_off++;
                } else {
                    *gcmd = gcmd_before & ~IWL_DMAR_GCMD_TE;
                    for (uint32_t spin = 0; spin < 1000000u; spin++) {
                        gsts_after = *gsts;
                        if ((gsts_after & IWL_DMAR_GSTS_TES) == 0)
                            break;
                    }
                    if ((gsts_after & IWL_DMAR_GSTS_TES) == 0)
                        disabled++;
                    else
                        failures++;
                }

                iwl_dbg("IWL9260: DMAR DRHD seg=%u base=0x%llx GCMD=%08x GSTS=%08x->%08x\n",
                        drhd->segment, (unsigned long long)base,
                        gcmd_before, gsts_before, gsts_after);
            }
        }

        p += rh->length;
    }

    if (drhd_count == 0) {
        iwl_dbg("IWL9260: DMAR present but no DRHD units found\n");
        return 0;
    }

    iwl_dbg("IWL9260: DMAR disable summary drhd=%d disabled=%d already_off=%d failures=%d\n",
            drhd_count, disabled, already_off, failures);
    return failures ? -1 : disabled;
}

/* Forward declarations for functions used before their definition */
static int iwl_grab_nic_access(void);
static int iwl_wait_alive(int use_init, uint32_t attempt_idx);
static void iwl_log_fw_verify_summary(void);
void wifi_9260_init(void);

/*
 * Read a 32-bit word from NIC SRAM via the HBUS target memory window.
 * Requires NIC access (MAC_ACCESS_REQ) to be held.
 */
static uint32_t iwl_read_sram(uint32_t sram_addr) {
    iwl_write32(HBUS_TARG_MEM_RADDR, sram_addr);
    return iwl_read32(HBUS_TARG_MEM_RDAT);
}

static uint32_t iwl_sram_probe_word_count(uint32_t size) {
    uint32_t words = (size + 3u) / 4u;
    if (words > IWL_SRAM_PROBE_WORDS)
        words = IWL_SRAM_PROBE_WORDS;
    return words;
}

static uint32_t iwl_sram_probe_word_mask(uint32_t size, uint32_t word_idx) {
    uint32_t off = word_idx * 4u;
    if (off >= size)
        return 0u;
    if (off + 4u <= size)
        return 0xFFFFFFFFu;

    uint32_t valid_bytes = size - off;
    uint32_t mask = 0u;
    for (uint32_t i = 0; i < valid_bytes; i++)
        mask |= (0xFFu << (i * 8u));
    return mask;
}

static uint32_t iwl_read_le_word_partial(const uint8_t *src, uint32_t len,
                                         uint32_t word_idx) {
    uint32_t off = word_idx * 4u;
    uint32_t v = 0;

    if (!src || off >= len)
        return 0;

    for (uint32_t i = 0; i < 4u && off + i < len; i++)
        v |= ((uint32_t)src[off + i]) << (i * 8u);

    return v;
}

static void iwl_reset_sram_probes(void) {
    g_iwl_bootdbg.fw_sram_probe_count = 0;
    iwl_zero(g_iwl_bootdbg.fw_sram_probe, sizeof(g_iwl_bootdbg.fw_sram_probe));
}

static int iwl_sram_probe_words_match(const struct iwl_sram_probe_entry *probe,
                                      const uint32_t *observed,
                                      uint32_t *mismatch_word) {
    if (!probe || !observed)
        return 0;

    for (uint32_t i = 0; i < probe->word_count; i++) {
        uint32_t mask = iwl_sram_probe_word_mask(probe->size, i);
        if (((probe->expected[i] ^ observed[i]) & mask) != 0u) {
            if (mismatch_word)
                *mismatch_word = i;
            return 0;
        }
    }

    if (mismatch_word)
        *mismatch_word = probe->word_count;
    return 1;
}

static int iwl_umac_error_marker_matches(uint32_t valid) {
    return valid == IWL_UMAC_ERROR_TABLE_VALID ||
           (valid & 0xFFFF0000u) == 0xDEAD0000u;
}

static void iwl_record_sram_probe(uint32_t sec_idx, uint32_t cpu_id,
                                  uint32_t sram_addr, uint32_t payload_size,
                                  const uint8_t *payload) {
    if (!payload || payload_size == 0 ||
        g_iwl_bootdbg.fw_sram_probe_count >= IWL_MAX_SEC_SECTIONS)
        return;

    struct iwl_sram_probe_entry *probe =
        &g_iwl_bootdbg.fw_sram_probe[g_iwl_bootdbg.fw_sram_probe_count++];
    iwl_zero(probe, sizeof(*probe));
    probe->sec_idx = sec_idx;
    probe->cpu_id = cpu_id;
    probe->addr = sram_addr;
    probe->size = payload_size;
    probe->extended = (sram_addr >= IWL_SRAM_EXT_START &&
                       sram_addr <= IWL_SRAM_EXT_END) ? 1u : 0u;
    probe->word_count = iwl_sram_probe_word_count(payload_size);

    for (uint32_t i = 0; i < probe->word_count; i++)
        probe->expected[i] = iwl_read_le_word_partial(payload, payload_size, i);

    if (iwl_grab_nic_access() == 0) {
        probe->staged_valid = 1;
        for (uint32_t i = 0; i < probe->word_count; i++)
            probe->staged[i] = iwl_read_sram(sram_addr + i * 4u);
    }
}

/*
 * Dump firmware error table from SRAM after SW_ERR.
 * Scans known addresses for the 0xDEAD0000-masked valid marker,
 * then reads the error descriptor fields.
 */
static void iwl_dump_fw_error_table(void) {
    /* Typical error table addresses for 9000-series firmwares.
     * The exact address is in the ALIVE notification (which we didn't get),
     * so we try known candidates.  Also scan for the DEAD marker. */
    static const uint32_t candidates[] = {
        0x800000,   /* UMAC region — many 9000 FW place error table here */
        0x800400,   /* UMAC offset variant */
        0x800200,   /* UMAC offset variant 2 */
        0x000000,   /* LMAC base */
        0x000400,   /* LMAC offset */
        0x404000,   /* first CPU1 section base */
        0x456000,   /* second CPU1 data section */
        0x000200,   /* early SRAM offset */
    };

    if (iwl_grab_nic_access() != 0) {
        iwl_dbg("IWL9260: can't grab NIC for error table\n");
        return;
    }

    for (uint32_t c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++) {
        uint32_t base = candidates[c];
        uint32_t valid = iwl_read_sram(base);
        if (valid == 0 || valid == 0xFFFFFFFFu)
            continue;

        /* Found something non-trivial — dump with Linux error table field names.
         * Linux struct iwl_error_event_table layout (from iwl-fh.h):
         *   +00: valid (0xDEAD0000 mask = valid)
         *   +04: error_id
         *   +08: blink1 (PC at error)
         *   +0C: blink2 (caller PC)
         *   +10: ilink1
         *   +14: ilink2
         *   +18: data1
         *   +1C: data2
         *   +20: data3
         *   +24: bcon_time
         *   +28: tsf_low
         *   +2C: tsf_hi
         *   +30: gp1, +34: gp2, +38: fw_rev_type
         *   +3C: major, +40: minor, +44: hw_ver, +48: brd_ver */
        iwl_dbg("IWL9260: ERR_TBL[%08x] valid=%08x\n", base, valid);
        uint32_t err_id  = iwl_read_sram(base + 0x04);
        uint32_t blink1  = iwl_read_sram(base + 0x08);
        uint32_t blink2  = iwl_read_sram(base + 0x0C);
        uint32_t ilink1  = iwl_read_sram(base + 0x10);
        uint32_t ilink2  = iwl_read_sram(base + 0x14);
        uint32_t data1   = iwl_read_sram(base + 0x18);
        uint32_t data2   = iwl_read_sram(base + 0x1C);
        uint32_t data3   = iwl_read_sram(base + 0x20);
        iwl_dbg("  err_id=%08x PC=%08x caller=%08x\n", err_id, blink1, blink2);
        iwl_dbg("  ilink1=%08x ilink2=%08x\n", ilink1, ilink2);
        iwl_dbg("  data1=%08x data2=%08x data3=%08x\n", data1, data2, data3);
        /* Dump remaining words for context */
        for (uint32_t i = 9; i < 24; i++) {
            uint32_t v = iwl_read_sram(base + i * 4u);
            iwl_dbg("  +%02x: %08x\n", i * 4u, v);
        }
    }

    /* Scan SRAM in 0x400-byte steps through LMAC region for DEAD marker */
    iwl_dbg("IWL9260: scanning for DEAD markers...\n");
    int found = 0;
    for (uint32_t addr = 0; addr < 0x080000u && found < 3; addr += 0x400u) {
        uint32_t v = iwl_read_sram(addr);
        if (iwl_umac_error_marker_matches(v)) {
            uint32_t err_id = iwl_read_sram(addr + 4);
            uint32_t pc     = iwl_read_sram(addr + 8);
            iwl_dbg("  DEAD @%06x valid=%08x err=%08x pc=%08x\n",
                    addr, v, err_id, pc);
            found++;
        }
    }
    if (!found)
        iwl_dbg("  (no DEAD markers found in LMAC 0x000000-0x080000)\n");

    /* Also dump first 8 words of SRAM at 0x00000000 to verify upload */
    iwl_dbg("IWL9260: SRAM[00000000] (verify upload):\n");
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t v = iwl_read_sram(i * 4u);
        iwl_dbg("  +%02x: %08x\n", i * 4u, v);
    }
}

/* Allocate all DMA rings and buffers in a single contiguous block below 4 GB */
static int iwl_dma_alloc(void) {
    if (g_iwl_dma.allocated) {
        iwl_dma_reset_state();
        return 0;
    }

    uint64_t phys = pmm_alloc_pages_below(IWL_DMA_TOTAL_PAGES, 0x100000000ULL);
    if (!phys) {
        kprint("IWL9260: DMA alloc failed (%u pages)\n", IWL_DMA_TOTAL_PAGES);
        return -1;
    }

    g_iwl_dma.dma_phys_base = phys;
    uint64_t off = 0;

    /* TFD ring: 16 pages (64 KB) */
    g_iwl_dma.tfd_ring_phys = phys + off;
    g_iwl_dma.tfd_ring = (struct iwl_tfd *)(uintptr_t)vmm_phys_to_virt(phys + off);
    off += (uint64_t)IWL_DMA_TFD_PAGES * 4096u;

    /* Byte-count table: 1 page */
    g_iwl_dma.bc_table_phys = phys + off;
    g_iwl_dma.bc_table = (struct iwl_bc_entry *)(uintptr_t)vmm_phys_to_virt(phys + off);
    off += (uint64_t)IWL_DMA_BC_PAGES * 4096u;

    /* RX status: 1 page */
    g_iwl_dma.rx_status_phys = phys + off;
    g_iwl_dma.rx_status = (struct iwl_rx_status *)(uintptr_t)vmm_phys_to_virt(phys + off);
    off += (uint64_t)IWL_DMA_RXSTATUS_PAGES * 4096u;

    /* RX BD ring (FRBDCB): 1 page */
    g_iwl_dma.rxbd_ring_phys = phys + off;
    g_iwl_dma.rxbd_ring = (struct iwl_rx_bd *)(uintptr_t)vmm_phys_to_virt(phys + off);
    off += (uint64_t)IWL_DMA_RXBD_PAGES * 4096u;

    /* RX used ring (URBDCB): 1 page (512 * 4B fits in 4K) */
    g_iwl_dma.rx_used_ring_phys = phys + off;
    g_iwl_dma.rx_used_ring = (uint32_t *)(uintptr_t)vmm_phys_to_virt(phys + off);
    off += (uint64_t)IWL_DMA_RXUSED_PAGES * 4096u;

    /* RX data buffers: one 4K page per RX slot */
    for (uint32_t i = 0; i < IWL_RX_QUEUE_SIZE; i++) {
        g_iwl_dma.rx_buf_phys[i] = phys + off;
        g_iwl_dma.rx_buf_virt[i] = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys + off);
        off += 4096u;
    }

    /* Command buffers: 4 pages */
    for (uint32_t i = 0; i < IWL_DMA_CMDBUF_PAGES; i++) {
        g_iwl_dma.cmd_buf_phys[i] = phys + off;
        g_iwl_dma.cmd_buf_virt[i] = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys + off);
        off += 4096u;
    }

    /* Firmware upload bounce buffer: 2 pages */
    g_iwl_dma.fw_bounce_phys = phys + off;
    g_iwl_dma.fw_bounce_virt = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys + off);
    off += (uint64_t)IWL_DMA_FW_BOUNCE_PAGES * 4096u;

    /* Keep-warm buffer: 1 page */
    g_iwl_dma.kw_phys = phys + off;
    off += (uint64_t)IWL_DMA_KW_PAGES * 4096u;

    /* Zero everything */
    iwl_zero(g_iwl_dma.tfd_ring, (uint64_t)IWL_DMA_TFD_PAGES * 4096u);
    iwl_zero(g_iwl_dma.bc_table, (uint64_t)IWL_DMA_BC_PAGES * 4096u);
    iwl_zero(g_iwl_dma.rx_status, (uint64_t)IWL_DMA_RXSTATUS_PAGES * 4096u);
    iwl_zero(g_iwl_dma.rxbd_ring, (uint64_t)IWL_DMA_RXBD_PAGES * 4096u);
    iwl_zero(g_iwl_dma.rx_used_ring, (uint64_t)IWL_DMA_RXUSED_PAGES * 4096u);
    for (uint32_t i = 0; i < IWL_DMA_CMDBUF_PAGES; i++)
        iwl_zero(g_iwl_dma.cmd_buf_virt[i], 4096u);
    iwl_zero(g_iwl_dma.fw_bounce_virt, (uint64_t)IWL_DMA_FW_BOUNCE_PAGES * 4096u);
    iwl_zero((void *)(uintptr_t)vmm_phys_to_virt(g_iwl_dma.kw_phys), 4096u);

    g_iwl_dma.tfd_write_idx = 0;
    g_iwl_dma.rx_write_idx = 0;
    g_iwl_dma.rx_read_idx = 0;
    g_iwl_dma.cmd_buf_idx = 0;
    g_iwl_dma.allocated = 1;

    kprint("IWL9260: DMA alloc OK: %u pages at phys=0x%08x\n",
           IWL_DMA_TOTAL_PAGES, (uint32_t)phys);
    kprint("IWL9260:   TFD ring phys=0x%08x  BC table phys=0x%08x\n",
           (uint32_t)g_iwl_dma.tfd_ring_phys, (uint32_t)g_iwl_dma.bc_table_phys);
    kprint("IWL9260:   RXBD ring phys=0x%08x  RX used phys=0x%08x\n",
           (uint32_t)g_iwl_dma.rxbd_ring_phys, (uint32_t)g_iwl_dma.rx_used_ring_phys);
    kprint("IWL9260:   RX status phys=0x%08x\n",
           (uint32_t)g_iwl_dma.rx_status_phys);
    kprint("IWL9260:   FW bounce phys=0x%08x size=%u\n",
           (uint32_t)g_iwl_dma.fw_bounce_phys,
           IWL_DMA_FW_BOUNCE_PAGES * 4096u);

    return 0;
}

/* Forward declarations for PRPH access (defined later, needed by RFH init) */
static void iwl_write_prph(uint32_t addr, uint32_t val);
static uint32_t iwl_read_prph(uint32_t addr);
static uint64_t iwl_rx_encode_bd_addr(uint64_t phys, uint16_t vid);
static void iwl_trace_capture(const char *tag);
static void iwl_rfh_kick_rx_free_ring(const char *tag);
static int iwl_start_fw_with_ctxt_info(const struct iwl_fw_pieces *pieces);

static uint32_t iwl_fh_mem_cbbc_queue(uint32_t q) {
    if (q < 16u)
        return FH_MEM_LOWER_BOUND + 0x9D0u + q * 4u;
    if (q < 20u)
        return FH_MEM_LOWER_BOUND + 0xBF0u + (q - 16u) * 4u;
    return FH_MEM_LOWER_BOUND + 0xB20u + (q - 20u) * 4u;
}

static void iwl_scd_txq_set_inactive(uint32_t q) {
    iwl_write_prph(SCD_QUEUE_STATUS_BITS(q),
                   (0u << SCD_QUEUE_STTS_REG_POS_ACTIVE) |
                   (1u << SCD_QUEUE_STTS_REG_POS_SCD_ACT_EN));
}

static void iwl_pcie_tx_stop_fh(void) {
    uint32_t idle_mask = 0;

    for (uint32_t ch = 0; ch < FH_TCSR_CHNL_NUM; ch++) {
        iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(ch), 0);
        idle_mask |= FH_TSSR_TX_STATUS_REG_MSK_CHNL_IDLE(ch);
    }

    if (iwl_poll_bit(FH_TSSR_TX_STATUS_REG, idle_mask, idle_mask, 5000u) != 0) {
        iwl_dbg("IWL9260: FH TX stop timeout TSSR=0x%08x mask=0x%08x\n",
                iwl_read32(FH_TSSR_TX_STATUS_REG), idle_mask);
    }
}

static int iwl_pcie_rx_mq_hw_init(void) {
    if (iwl_grab_nic_access() != 0)
        return -2;

    iwl_write_prph(RFH_RXF_DMA_CFG, 0);
    iwl_write_prph(RFH_RXF_RXQ_ACTIVE, 0);
    iwl_write_prph(RFH_Q0_FRBDCB_BA_LSB, (uint32_t)g_iwl_dma.rxbd_ring_phys);
    iwl_write_prph(RFH_Q0_FRBDCB_BA_MSB, (uint32_t)(g_iwl_dma.rxbd_ring_phys >> 32));
    iwl_write_prph(RFH_Q0_URBDCB_BA_LSB, (uint32_t)g_iwl_dma.rx_used_ring_phys);
    iwl_write_prph(RFH_Q0_URBDCB_BA_MSB, (uint32_t)(g_iwl_dma.rx_used_ring_phys >> 32));
    iwl_write_prph(RFH_Q0_URBD_STTS_WPTR_LSB, (uint32_t)g_iwl_dma.rx_status_phys);
    iwl_write_prph(RFH_Q0_URBD_STTS_WPTR_MSB, (uint32_t)(g_iwl_dma.rx_status_phys >> 32));
    iwl_write_prph(RFH_Q0_FRBDCB_WIDX, 0);
    iwl_write_prph(RFH_Q0_FRBDCB_RIDX, 0);
    iwl_write_prph(RFH_Q0_URBDCB_WIDX, 0);
    iwl_write_prph(RFH_RXF_DMA_CFG,
                   RFH_DMA_EN_VAL |
                   RFH_RXF_DMA_RB_SIZE_4K |
                   RFH_RXF_DMA_MIN_RB_4_8 |
                   RFH_RXF_DMA_DROP_TOO_LARGE |
                   RFH_RXF_DMA_RBDCB_SIZE_512);
    iwl_write_prph(RFH_GEN_CFG,
                   RFH_GEN_CFG_RFH_DMA_SNOOP |
                   RFH_GEN_CFG_SERVICE_DMA_SNOOP |
                   RFH_GEN_CFG_RB_CHUNK_SIZE_128);
    iwl_write_prph(RFH_RXF_RXQ_ACTIVE, (1u << 0) | (1u << 16));

    g_iwl_bootdbg.rfh_frbdcb_ba_lsb = (uint32_t)g_iwl_dma.rxbd_ring_phys;
    g_iwl_bootdbg.rfh_urbdcb_ba_lsb = (uint32_t)g_iwl_dma.rx_used_ring_phys;
    g_iwl_bootdbg.rfh_status_wptr_lsb = (uint32_t)g_iwl_dma.rx_status_phys;

    iwl_release_nic_access();
    iwl_write8(CSR_INT_COALESCING, (uint8_t)IWL_HOST_INT_TIMEOUT_DEF);
    return 0;
}

static int iwl_pcie_rx_init(void) {
    int ret;

    ret = iwl_dma_alloc();
    if (ret != 0)
        return ret;

    for (uint32_t i = 0; i < IWL_RX_QUEUE_SIZE; i++) {
        g_iwl_dma.rxbd_ring[i].addr =
            iwl_rx_encode_bd_addr(g_iwl_dma.rx_buf_phys[i], (uint16_t)(i + 1u));
    }

    iwl_cache_flush(g_iwl_dma.rxbd_ring, IWL_RX_QUEUE_SIZE * sizeof(struct iwl_rx_bd));
    iwl_cache_flush(g_iwl_dma.rx_used_ring, (uint64_t)IWL_DMA_RXUSED_PAGES * 4096u);
    iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));

    ret = iwl_pcie_rx_mq_hw_init();
    if (ret != 0)
        return ret;

    /* 9000 discrete parts need an RB timeout so single-frame ALIVE can retire. */
    iwl_write32(HBUS_TARG_MBX_C, IWL_9000_RB_TIMEOUT);

    iwl_rfh_kick_rx_free_ring("rfh-widx");
    iwl_trace_capture("rfh-init");
    return 0;
}

static int iwl_pcie_tx_init(void) {
    uint32_t scd;

    if (!g_iwl_dma.allocated)
        return -1;
    if (iwl_grab_nic_access() != 0)
        return -2;

    iwl_pcie_tx_stop_fh();
    iwl_write_prph(SCD_TXFACT, 0);
    for (uint32_t q = 0; q < 31u; q++)
        iwl_scd_txq_set_inactive(q);
    iwl_write32(FH_KW_MEM_ADDR_REG, (uint32_t)(g_iwl_dma.kw_phys >> 4));
    for (uint32_t q = 0; q < 31u; q++)
        iwl_write32(iwl_fh_mem_cbbc_queue(q), (uint32_t)(g_iwl_dma.tfd_ring_phys >> 8));

    scd = iwl_read_prph(SCD_GP_CTRL);
    if (scd == 0xFFFFFFFFu)
        scd = 0;
    iwl_write_prph(SCD_GP_CTRL, scd | SCD_GP_CTRL_AUTO_ACTIVE_MODE);
    iwl_write_prph(SCD_GP_CTRL, scd | SCD_GP_CTRL_AUTO_ACTIVE_MODE |
                                  SCD_GP_CTRL_ENABLE_31_QUEUES);
    iwl_release_nic_access();

    return 0;
}

/*
 * Re-advertise the RX free-ring to RFH.
 * Reused during ALIVE handoff in case the initial restock doorbell was lost
 * during the upload-to-runtime transition.
 */
static void iwl_rfh_kick_rx_free_ring(const char *tag) {
    if (!g_iwl_dma.allocated)
        return;

    uint32_t write_actual = (IWL_RX_QUEUE_SIZE - 1u) & ~7u;
    /* fry707: Linux v6.0 trigger format is JUST write_actual — no upper
     * bits.  The previous (write_actual << 16) duplication was based on
     * an older kernel version; the current upstream writes only the
     * rounded-down index.  Extra upper bits corrupt the RFH doorbell
     * and may cause the firmware's UMAC to SYSASSERT when it tries to
     * DMA the ALIVE notification through a misconfigured RFH.
     * Ref: iwl_pcie_rxmq_restock() in Linux v6.0. */
    g_iwl_bootdbg.rfh_widx_trg_written = write_actual;
    iwl_cache_flush(g_iwl_dma.rxbd_ring,
                    IWL_RX_QUEUE_SIZE * sizeof(struct iwl_rx_bd));
    iwl_cache_flush(g_iwl_dma.rx_used_ring,
                    (uint64_t)IWL_DMA_RXUSED_PAGES * 4096u);
    iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));

    /* Read RFH state BEFORE doorbell */
    uint32_t pre_widx = 0, pre_ridx = 0, pre_act = 0, pre_dma = 0, pre_gen = 0;
    if (iwl_grab_nic_access() == 0) {
        pre_widx = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        pre_ridx = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        pre_act  = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        pre_dma  = iwl_read_prph(RFH_RXF_DMA_CFG);
        pre_gen  = iwl_read_prph(RFH_GEN_STATUS);
    }

    iwl_write32(RFH_Q_FRBDCB_WIDX_TRG(0), g_iwl_bootdbg.rfh_widx_trg_written);
    iwl_udelay(100);

    /* Read RFH state AFTER doorbell */
    uint32_t post_widx = 0, post_ridx = 0, post_act = 0, post_gen = 0;
    uint32_t csr_trg_readback = 0;
    if (iwl_grab_nic_access() == 0) {
        post_widx = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        post_ridx = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        post_act  = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        post_gen  = iwl_read_prph(RFH_GEN_STATUS);
    }
    csr_trg_readback = iwl_read32(RFH_Q_FRBDCB_WIDX_TRG(0));

    /* Read first RXBD entry via PRPH to see what device sees */
    uint32_t rxbd_ba_lsb = 0, rxbd_ba_msb = 0;
    if (iwl_grab_nic_access() == 0) {
        rxbd_ba_lsb = iwl_read_prph(RFH_Q0_FRBDCB_BA_LSB);
        rxbd_ba_msb = iwl_read_prph(RFH_Q0_FRBDCB_BA_MSB);
    }

    iwl_dbg("IWL9260: RFH kick [%s] wrote=%u to CSR 0x%04x\n",
            tag ? tag : "?", g_iwl_bootdbg.rfh_widx_trg_written,
            RFH_Q_FRBDCB_WIDX_TRG(0));
    iwl_dbg("  PRE:  WIDX=%u RIDX=%u ACT=%08x DMA=%08x GEN=%08x\n",
            pre_widx, pre_ridx, pre_act, pre_dma, pre_gen);
    iwl_dbg("  POST: WIDX=%u RIDX=%u ACT=%08x GEN=%08x TRG_RB=%08x\n",
            post_widx, post_ridx, post_act, post_gen, csr_trg_readback);
    iwl_dbg("  BA: LSB=%08x MSB=%08x (expect %08x)\n",
            rxbd_ba_lsb, rxbd_ba_msb, (uint32_t)g_iwl_dma.rxbd_ring_phys);

    /* Verify first 2 RXBD entries from host memory match what we expect */
    iwl_cache_flush(g_iwl_dma.rxbd_ring, 16);
    uint64_t bd0 = g_iwl_dma.rxbd_ring[0].addr;
    uint64_t bd1 = g_iwl_dma.rxbd_ring[1].addr;
    iwl_dbg("  RXBD[0]=%08x%08x RXBD[1]=%08x%08x\n",
            (uint32_t)(bd0 >> 32), (uint32_t)bd0,
            (uint32_t)(bd1 >> 32), (uint32_t)bd1);

    /* Check GP_CNTRL for sleep/clock state */
    uint32_t gp = iwl_read32(CSR_GP_CNTRL);
    uint32_t rst = iwl_read32(CSR_RESET);
    iwl_dbg("  GP=%08x RST=%08x\n", gp, rst);

    if (tag)
        iwl_trace_capture(tag);
}

/* Set up a TFD entry pointing to a host memory buffer for DMA transfer. */
__attribute__((unused))
static void iwl_tfd_set_tb(struct iwl_tfd *tfd, uint8_t idx, uint64_t phys, uint16_t len) {
    if (idx >= IWL_TFD_TB_MAX) return;
    tfd->tbs[idx].lo = (uint32_t)(phys & 0xFFFFFFFFu);
    tfd->tbs[idx].hi = (uint16_t)((phys >> 32) & 0xFFFFu);
    tfd->tbs[idx].len = len;
    tfd->num_tbs = idx + 1;
}

/* ---- Step 4: Firmware Upload ---- */

/* HBUS target peripheral register access (for internal NIC registers) */
#define HBUS_TARG_PRPH_WADDR    0x444
#define HBUS_TARG_PRPH_RADDR    0x448
#define HBUS_TARG_PRPH_WDAT     0x44C
#define HBUS_TARG_PRPH_RDAT     0x450

/* Peripheral register access needs MAC_ACCESS_REQ to be active.
   The (3 << 24) sets dword access mode. */
static void iwl_write_prph(uint32_t addr, uint32_t val) {
    iwl_write32(HBUS_TARG_PRPH_WADDR, ((addr & 0x000FFFFFu) | (3u << 24)));
    iwl_write32(HBUS_TARG_PRPH_WDAT, val);
}

__attribute__((unused))
static uint32_t iwl_read_prph(uint32_t addr) {
    iwl_write32(HBUS_TARG_PRPH_RADDR, ((addr & 0x000FFFFFu) | (3u << 24)));
    return iwl_read32(HBUS_TARG_PRPH_RDAT);
}

/* 9000-series firmware load status / reset sequencing */
#define FH_UCODE_LOAD_STATUS    0x1AF0  /* FH register: per-section load bits */
#define UREG_UCODE_LOAD_STATUS  0xA05C40 /* gen2 final CPU-ready marker */
#define RELEASE_CPU_RESET       0x300C
#define RELEASE_CPU_RESET_BIT   (1u << 24) /* fry718: revert to BIT(24) — BIT(0) caused full device reset wiping SRAM */
#define WFPM_GP2                0xA030B4
#define LMPM_CHICK              0xA01FF8
#define LMPM_CHICK_EXTENDED_ADDR_SPACE  0x01
#define IWL_FW_MEM_EXTENDED_START      0x00040000u
#define IWL_FW_MEM_EXTENDED_END        0x00057FFFu

/* Linux iwl-prph.h PRPH offsets for secure header registers. */
#define LMPM_SECURE_UCODE_LOAD_CPU1_HDR_ADDR   0x1E78
#define LMPM_SECURE_UCODE_LOAD_CPU2_HDR_ADDR   0x1E7C
#define LMPM_SECURE_CPU1_HDR_MEM_SPACE          0x420000
#define LMPM_SECURE_CPU2_HDR_MEM_SPACE          0x420400

/* Default SRAM addresses for legacy firmware sections (pre-gen2) */
#define IWL_FW_INST_SRAM_ADDR   0x00000000u
#define IWL_FW_DATA_SRAM_ADDR   0x00800000u

/* Max bytes per DMA chunk — hardware limit */
#define IWL_FW_DMA_CHUNK_SIZE   0x20000u  /* 128 KB */
#define IWL_FW_BOUNCE_SIZE      ((uint32_t)IWL_DMA_FW_BOUNCE_PAGES * 4096u)
#define IWL_FW_SINGLE_SECTION_IDX 0xFFFFFFFFu

static uint32_t iwl_fw_sec_get_offset(const struct iwl_fw_section *sec);
static int iwl_fw_sec_is_separator(const struct iwl_fw_section *sec);

static void iwl_set_prph_bit(uint32_t addr, uint32_t bits) {
    iwl_write_prph(addr, iwl_read_prph(addr) | bits);
}

static void iwl_clear_prph_bit(uint32_t addr, uint32_t bits) {
    iwl_write_prph(addr, iwl_read_prph(addr) & ~bits);
}

/*
 * Ensure NIC is awake and accepting register accesses.
 * Must be called before any prph writes or FH register accesses.
 * Linux wraps every prph access in grab/release; we re-assert here
 * at key points to handle the NIC autonomously going to sleep.
 */
static int iwl_grab_nic_access(void) {
    iwl_set_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    int rc = iwl_poll_bit(CSR_GP_CNTRL,
                          CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                          CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
                          15000);
    if (rc != 0) {
        uint32_t gp = iwl_read32(CSR_GP_CNTRL);
        kprint("IWL9260: grab_nic_access FAILED GP_CNTRL=0x%08x\n", gp);
        if (gp & CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP)
            kprint("IWL9260: NIC is GOING_TO_SLEEP\n");
    }
    return rc;
}

static void iwl_release_nic_access(void) {
    iwl_clear_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

/*
 * Program NIC radio config from FW TLV phy_config before firmware upload.
 * Mirrors Linux iwl_mvm_nic_config() + iwl_pcie_apm_config() behavior.
 */
static int iwl_nic_config(const struct iwl_fw_pieces *pieces) {
    if (!pieces)
        return -1;
    if (iwl_grab_nic_access() != 0)
        return -2;

    uint32_t phy_config = pieces->phy_config;
    uint32_t radio_cfg_type = (phy_config & FW_PHY_CFG_RADIO_TYPE) >>
                              FW_PHY_CFG_RADIO_TYPE_POS;
    uint32_t radio_cfg_step = (phy_config & FW_PHY_CFG_RADIO_STEP) >>
                              FW_PHY_CFG_RADIO_STEP_POS;
    uint32_t radio_cfg_dash = (phy_config & FW_PHY_CFG_RADIO_DASH) >>
                              FW_PHY_CFG_RADIO_DASH_POS;

    uint32_t hw_step_dash = CSR_HW_REV_STEP_DASH_VAL(iwl_read32(CSR_HW_REV_STEP_DASH));
    /* fry708: Match Linux mask — include MAC_SI + RADIO_SI.
       For 8000+ family, neither SI bit should be set (cleared via mask). */
    uint32_t cfg_mask = CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH |
                        CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE |
                        CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP |
                        CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH |
                        CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI |
                        CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
    uint32_t cfg_val = hw_step_dash |
                       (radio_cfg_type << CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE) |
                       (radio_cfg_step << CSR_HW_IF_CONFIG_REG_POS_PHY_STEP) |
                       (radio_cfg_dash << CSR_HW_IF_CONFIG_REG_POS_PHY_DASH);
    uint32_t old_cfg = iwl_read32(CSR_HW_IF_CONFIG_REG);
    uint32_t new_cfg = (old_cfg & ~cfg_mask) | (cfg_val & cfg_mask);

    iwl_write32(CSR_HW_IF_CONFIG_REG, new_cfg);
    iwl_set_bit(CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);
    iwl_release_nic_access();

    iwl_dbg("IWL9260: NIC cfg phy=0x%08x radio=%u-%u-%u HW_IF %08x->%08x\n",
            phy_config, radio_cfg_type, radio_cfg_step, radio_cfg_dash,
            old_cfg, new_cfg);
    return 0;
}

static int iwl_pcie_nic_init(const struct iwl_fw_pieces *pieces, uint8_t *failed_step) {
    int ret;

    ret = iwl_apm_init();
    if (ret)
        return ret;

    iwl_pcie_set_pwr(0);

    ret = iwl_nic_config(pieces);
    if (ret)
        return ret;

    g_iwl_init_step = WIFI_STEP_DMA_ALLOC;
    if (failed_step)
        *failed_step = WIFI_STEP_DMA_ALLOC;
    ret = iwl_pcie_rx_init();
    if (ret)
        return ret;

    g_iwl_init_step = WIFI_STEP_TFH_INIT;
    if (failed_step)
        *failed_step = WIFI_STEP_TFH_INIT;
    ret = iwl_pcie_tx_init();
    if (ret)
        return ret;

    iwl_write32(CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFFu);
    g_iwl_bootdbg.shadow_ctrl = iwl_read32(CSR_MAC_SHADOW_REG_CTRL);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_SHADOW_ENABLED;
    return 0;
}

/*
 * DMA one chunk of firmware to device SRAM via FH service channel 9.
 * The FH engine requires aligned host DRAM, so we bounce every chunk through
 * a dedicated page-aligned upload buffer.
 *
 * dst_sram: destination address in device SRAM
 * src: host source data
 * byte_cnt: number of bytes to transfer (max IWL_FW_DMA_CHUNK_SIZE)
 */
static int iwl_load_fw_chunk(uint32_t dst_sram, const uint8_t *src, uint32_t byte_cnt) {
    if (!g_iwl_dma.allocated || !g_iwl_dma.fw_bounce_virt) return -1;
    if (!src || byte_cnt == 0 || byte_cnt > IWL_FW_DMA_CHUNK_SIZE ||
        byte_cnt > IWL_FW_BOUNCE_SIZE) {
        return -2;
    }

    int extended_addr = (dst_sram >= IWL_FW_MEM_EXTENDED_START &&
                         dst_sram <= IWL_FW_MEM_EXTENDED_END);

    iwl_copy(g_iwl_dma.fw_bounce_virt, src, byte_cnt);
    iwl_cache_flush(g_iwl_dma.fw_bounce_virt, byte_cnt);

    /* Clear stale FH interrupts before arming the service channel. */
    iwl_write32(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);

    iwl_write32(FH_SRVC_CHNL_SRAM_ADDR_REG(FH_SRVC_CHNL), dst_sram);
    iwl_write32(FH_TFDIB_CTRL0_REG(FH_SRVC_CHNL),
                (uint32_t)(g_iwl_dma.fw_bounce_phys & 0xFFFFFFFFu));
    iwl_write32(FH_TFDIB_CTRL1_REG(FH_SRVC_CHNL),
                ((((uint32_t)(g_iwl_dma.fw_bounce_phys >> 32)) & 0xFu)
                    << FH_MEM_TFDIB_REG1_ADDR_BITSHIFT) |
                byte_cnt);
    iwl_write32(FH_TCSR_CHNL_TX_BUF_STS_REG(FH_SRVC_CHNL),
                (1u << FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM) |
                (1u << FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX) |
                FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID);
    if (extended_addr)
        iwl_set_prph_bit(LMPM_CHICK, LMPM_CHICK_EXTENDED_ADDR_SPACE);
    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
                FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);

    for (uint32_t i = 0; i < 500000u; i++) {
        uint32_t fh_int = iwl_read32(CSR_FH_INT_STATUS);
        if (fh_int & CSR_FH_INT_BIT_ERR) {
            iwl_write32(CSR_FH_INT_STATUS, fh_int);
            iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
                        FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);
            if (extended_addr)
                iwl_clear_prph_bit(LMPM_CHICK, LMPM_CHICK_EXTENDED_ADDR_SPACE);
            kprint("IWL9260: FH service DMA error dst=0x%08x fh_int=0x%08x\n",
                   dst_sram, fh_int);
            return -3;
        }
        if (fh_int & CSR_FH_INT_TX_MASK) {
            iwl_write32(CSR_FH_INT_STATUS, fh_int);
            iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
                        FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);
            if (extended_addr)
                iwl_clear_prph_bit(LMPM_CHICK, LMPM_CHICK_EXTENDED_ADDR_SPACE);
            return 0;
        }
        iwl_udelay(1);
    }

    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);
    if (extended_addr)
        iwl_clear_prph_bit(LMPM_CHICK, LMPM_CHICK_EXTENDED_ADDR_SPACE);
    kprint("IWL9260: FH service DMA timeout dst=0x%08x len=%u fh_int=0x%08x cfg=0x%08x buf=0x%08x\n",
           dst_sram, byte_cnt,
           iwl_read32(CSR_FH_INT_STATUS),
           iwl_read32(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL)),
           iwl_read32(FH_TCSR_CHNL_TX_BUF_STS_REG(FH_SRVC_CHNL)));
    return -4;
}

/*
 * Upload a firmware payload to device SRAM.
 *
 * Parsed SEC_RT/SEC_INIT sections already carry their destination address
 * in sec->offset, so callers pass the SRAM address explicitly here. Legacy
 * INST/DATA uploads also use this helper with their fixed SRAM destinations.
 */
static int iwl_load_fw_payload(uint32_t sram_addr,
                               const uint8_t *payload,
                               uint32_t payload_size,
                               const char *name, uint32_t sec_idx,
                               uint32_t cpu_id) {
    if (!payload || payload_size == 0u) {
        if (sec_idx == IWL_FW_SINGLE_SECTION_IDX) {
            kprint("IWL9260: %s invalid payload ptr=0x%08x size=%u\n",
                   name ? name : "fw",
                   (uint32_t)(uintptr_t)payload, payload_size);
        } else {
            kprint("IWL9260: %s[%u] invalid payload ptr=0x%08x size=%u\n",
                   name ? name : "fw", sec_idx,
                   (uint32_t)(uintptr_t)payload, payload_size);
        }
        return -2;
    }

    if (sec_idx == IWL_FW_SINGLE_SECTION_IDX) {
        kprint("IWL9260: uploading %s: %u bytes -> SRAM 0x%08x\n",
               name, payload_size, sram_addr);
    } else {
        kprint("IWL9260: uploading %s[%u]: %u bytes -> SRAM 0x%08x\n",
               name, sec_idx, payload_size, sram_addr);
    }

    /* Upload in chunks that fit both the hardware and the bounce buffer. */
    uint32_t offset = 0;
    while (offset < payload_size) {
        uint32_t chunk = payload_size - offset;
        if (chunk > IWL_FW_DMA_CHUNK_SIZE)
            chunk = IWL_FW_DMA_CHUNK_SIZE;
        if (chunk > IWL_FW_BOUNCE_SIZE)
            chunk = IWL_FW_BOUNCE_SIZE;

        int rc = iwl_load_fw_chunk(sram_addr + offset, payload + offset, chunk);
        if (rc != 0) {
            if (sec_idx == IWL_FW_SINGLE_SECTION_IDX) {
                kprint("IWL9260: %s upload FAILED at offset %u/%u\n",
                       name, offset, payload_size);
            } else {
                kprint("IWL9260: %s[%u] upload FAILED at offset %u/%u\n",
                       name, sec_idx, offset, payload_size);
            }
            return -1;
        }

        offset += chunk;
    }

    if (sec_idx == IWL_FW_SINGLE_SECTION_IDX) {
        kprint("IWL9260: %s upload OK (%u bytes)\n", name, payload_size);
    } else {
        kprint("IWL9260: %s[%u] upload OK (%u bytes)\n",
               name, sec_idx, payload_size);
    }
    iwl_record_sram_probe(sec_idx, cpu_id, sram_addr, payload_size, payload);
    return 0;
}

static int iwl_pcie_load_cpu_sections_8000(const struct iwl_fw_section *sections,
                                           uint32_t count,
                                           int cpu,
                                           uint32_t *first_ucode_section) {
    uint32_t shift_param;
    uint32_t sec_num = 0x1u;
    uint32_t i;
    uint32_t last_read_idx;
    uint32_t loaded = 0;
    uint32_t first_off = 0;
    uint32_t last_off = 0;
    uint32_t break_idx = count;
    uint32_t break_off = 0;
    uint32_t cpu_id = (cpu == 1) ? 1u : 2u;
    struct iwl_bootdbg_fw_pass *dbg_pass =
        (cpu == 1) ? &g_iwl_bootdbg.fw_cpu1 : &g_iwl_bootdbg.fw_cpu2;

    if (cpu == 1) {
        shift_param = 0;
        *first_ucode_section = 0;
    } else {
        shift_param = 16;
        (*first_ucode_section)++;
    }

    iwl_zero(dbg_pass, sizeof(*dbg_pass));
    dbg_pass->start_idx = *first_ucode_section;
    last_read_idx = *first_ucode_section;

    for (i = *first_ucode_section; i < count; i++) {
        uint32_t val;
        int ret;

        last_read_idx = i;
        if (!sections[i].data ||
            iwl_fw_sec_is_separator(&sections[i])) {
            break_idx = i;
            if (sections[i].data)
                break_off = iwl_fw_sec_get_offset(&sections[i]);
            break;
        }

        ret = iwl_load_fw_payload(iwl_fw_sec_get_offset(&sections[i]),
                                  sections[i].data, sections[i].size,
                                  cpu == 1 ? "CPU1" : "CPU2", i, cpu_id);
        if (ret)
            return ret;

        if (loaded == 0)
            first_off = iwl_fw_sec_get_offset(&sections[i]);
        last_off = iwl_fw_sec_get_offset(&sections[i]);

        val = iwl_read32(FH_UCODE_LOAD_STATUS);
        val |= (sec_num << shift_param);
        iwl_write32(FH_UCODE_LOAD_STATUS, val);
        sec_num = (sec_num << 1) | 0x1u;
        loaded++;
    }

    dbg_pass->break_idx = break_idx;
    dbg_pass->break_off = break_off;
    dbg_pass->loaded = loaded;
    dbg_pass->first_off = first_off;
    dbg_pass->last_off = last_off;
    *first_ucode_section = last_read_idx;

    if (cpu == 1 && loaded == 0)
        return -11;

    /* fry718: REMOVED sentinel writes to SRAM 0x00-0x38.
     * fry716 wrote 0xCAFE0000..0xCAFE000E to the error table region, but
     * section [1] starts at SRAM 0x00000000 — the sentinel corrupted the
     * first 60 bytes of uploaded firmware data.  If the firmware validates
     * its own header integrity at boot, this corruption causes a silent crash.
     * fry716 already proved the error table 0x2E is template data, not real. */

    iwl_enable_interrupts();
    iwl_write32(FH_UCODE_LOAD_STATUS, cpu == 1 ? 0x0000FFFFu : 0xFFFFFFFFu);
    dbg_pass->done_status = iwl_read32(FH_UCODE_LOAD_STATUS);

    /* fry718: SRAM checkpoint — read first 4 words from the first uploaded
     * section and from SRAM 0x00 immediately after done marker.  Detects
     * whether a device reset wipes SRAM between upload and ALIVE wait. */
    if (cpu == 1 && loaded >= 1u) {
        uint32_t ck0[4], ck1[4];
        for (uint32_t w = 0; w < 4u; w++)
            ck0[w] = iwl_read_sram(first_off + w * 4u);
        for (uint32_t w = 0; w < 4u; w++)
            ck1[w] = iwl_read_sram(w * 4u);
        kprint("IWL9260: fry718 SRAM checkpoint after CPU%d done:\n", cpu);
        kprint("  @%08x: %08x %08x %08x %08x\n", first_off, ck0[0], ck0[1], ck0[2], ck0[3]);
        kprint("  @000000: %08x %08x %08x %08x\n", ck1[0], ck1[1], ck1[2], ck1[3]);
        kprint("  GP=%08x RST=%08x FH=%08x\n",
               iwl_read32(CSR_GP_CNTRL), iwl_read32(CSR_RESET),
               iwl_read32(FH_UCODE_LOAD_STATUS));
    }

    iwl_dbg("IWL9260: CPU%d done (%u sections) FH_STATUS=0x%08x ULD_STATUS=0x%08x\n",
            cpu, loaded, dbg_pass->done_status, iwl_read_prph(UREG_UCODE_LOAD_STATUS));
    iwl_trace_capture(cpu == 1 ? "CPU1" : "CPU2");
    return 0;
}

static int iwl_pcie_load_given_ucode_8000(const struct iwl_fw_pieces *pieces, int use_init) {
    const struct iwl_fw_section *sections;
    uint32_t count;
    uint32_t first_ucode_section = 0;
    int ret;

    if (use_init && pieces->sec_init_count > 0) {
        sections = pieces->sec_init;
        count = pieces->sec_init_count;
    } else {
        sections = pieces->sec_rt;
        count = pieces->sec_rt_count;
    }

    if (!sections || count == 0)
        return -11;

    g_iwl_bootdbg.fw_upload_is_init = use_init ? 1u : 0u;
    g_iwl_bootdbg.fw_sec_total = count;
    iwl_zero(&g_iwl_bootdbg.fw_cpu1, sizeof(g_iwl_bootdbg.fw_cpu1));
    iwl_zero(&g_iwl_bootdbg.fw_cpu2, sizeof(g_iwl_bootdbg.fw_cpu2));
    iwl_reset_sram_probes();

    if (iwl_grab_nic_access() != 0)
        return -10;

    iwl_write32(FH_UCODE_LOAD_STATUS, 0);

    g_iwl_bootdbg.wfpm_gp2_written = 0x01010101u;
    iwl_write_prph(WFPM_GP2, g_iwl_bootdbg.wfpm_gp2_written);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_WFPM_WRITTEN;
    g_iwl_bootdbg.wfpm_gp2_readback = iwl_read_prph(WFPM_GP2);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_WFPM_READ;

    /* fry720: REMOVED iwl_write_prph(RELEASE_CPU_RESET, BIT(24)) — that
     * register (PRPH 0x300C) is for the pre-8000 upload path.  For 8000+,
     * Linux releases the CPU via iwl_write32(CSR_RESET, 0) AFTER all
     * sections are uploaded (see end of this function).
     *
     * fry708 incorrectly removed CSR_RESET=0 claiming it wasn't in the
     * 8000+ path — but Linux 7.0-rc2 iwl_pcie_load_given_ucode_8000()
     * clearly ends with: iwl_enable_interrupts(); iwl_write32(CSR_RESET, 0);
     * The comment in Linux literally says "release CPU reset". */

    ret = iwl_pcie_load_cpu_sections_8000(sections, count, 1, &first_ucode_section);
    if (ret)
        return ret;

    /* Always call CPU2 — even for single-CPU INIT, this writes the
     * final 0xFFFFFFFF done marker.  Without it, boot ROM never starts. */
    ret = iwl_pcie_load_cpu_sections_8000(sections, count, 2, &first_ucode_section);
    if (ret)
        return ret;

    /* fry720: Match Linux 7.0-rc2 iwl_pcie_load_given_ucode_8000() ending:
     *   iwl_enable_interrupts(trans);
     *   // release CPU reset
     *   iwl_write32(trans, CSR_RESET, 0);
     *
     * The enable_interrupts() call inside iwl_pcie_load_cpu_sections_8000
     * already ran (before each done marker), but Linux calls it again here.
     * The CSR_RESET=0 write is what actually releases the CPU on 8000+
     * devices — this was incorrectly removed in fry708. Without it, the
     * CPU stays held and firmware never starts (no ALIVE). */
    iwl_enable_interrupts();
    iwl_write32(CSR_RESET, 0);
    iwl_dbg("IWL9260: fry720 CSR_RESET=0 written (CPU released)\n");

    g_iwl_bootdbg.fh_load_status_after_upload = iwl_read32(FH_UCODE_LOAD_STATUS);
    iwl_dbg("IWL9260: FW upload complete host_status=%08x GP=%08x RST=%08x\n",
            g_iwl_bootdbg.fh_load_status_after_upload,
            iwl_read32(CSR_GP_CNTRL),
            iwl_read32(CSR_RESET));
    iwl_trace_capture("fw-up");
    return 0;
}

/*
 * ---- Context Info Boot (9000-series gen2) ----
 *
 * Multi-section SEC_RT firmware on 9000-series parts boots through the
 * context-info self-load path, not the legacy FH upload flow.
 */

/* Read the SRAM destination offset captured during TLV parsing. */
static uint32_t iwl_fw_sec_get_offset(const struct iwl_fw_section *sec) {
    if (!sec) return 0;
    return sec->offset;
}

static int iwl_fw_sec_is_separator(const struct iwl_fw_section *sec) {
    uint32_t off = iwl_fw_sec_get_offset(sec);
    return sec && (off == CPU1_CPU2_SEPARATOR || off == PAGING_SEPARATOR);
}

/* Count consecutive non-separator sections starting at 'start' */
static uint32_t iwl_get_fw_sec_count(const struct iwl_fw_section *sec,
                                     uint32_t total, uint32_t start) {
    uint32_t n = 0;
    for (uint32_t i = start; i < total; i++) {
        if (!sec[i].data) break;
        if (iwl_fw_sec_is_separator(&sec[i]) || sec[i].size == 0u) break;
        n++;
    }
    return n;
}

/* Allocate DMA-accessible memory for one already-parsed FW payload section. */
static int iwl_alloc_fw_dma_sec(const struct iwl_fw_section *sec, uint32_t idx) {
    if (idx >= IWL_MAX_FW_DMA_SEC) return -1;
    if (!sec || !sec->data || sec->size == 0u) {
        kprint("IWL9260: FW sec[%u] invalid payload size=%u\n",
               idx, sec ? sec->size : 0u);
        return -1;
    }

    const uint8_t *data = sec->data;
    uint32_t size = sec->size;
    uint32_t pages = (size + 4095u) / 4096u;
    uint64_t phys = pmm_alloc_pages_below(pages, 0x100000000ULL);
    if (!phys) {
        kprint("IWL9260: FW sec[%u] DMA alloc failed (%u pages)\n", idx, pages);
        return -1;
    }
    uint8_t *virt = (uint8_t *)(uintptr_t)vmm_phys_to_virt(phys);
    iwl_copy(virt, data, size);
    uint32_t rem = pages * 4096u - size;
    if (rem > 0) iwl_zero(virt + size, rem);
    iwl_cache_flush(virt, (uint64_t)pages * 4096u);

    g_iwl_fw_dma.phys[idx] = phys;
    g_iwl_fw_dma.size[idx] = size;
    if (idx >= g_iwl_fw_dma.count)
        g_iwl_fw_dma.count = idx + 1;
    return 0;
}

/*
 * Build the Context Information table, copy firmware sections into
 * DMA buffers, and arm the boot ROM context-info auto-boot path.
 */
__attribute__((unused))
static int iwl_start_fw_with_ctxt_info(const struct iwl_fw_pieces *pieces) {
    /* --- allocate one page for the context info struct --- */
    uint64_t ci_phys = pmm_alloc_pages_range(1, IWL_CTXT_INFO_MIN_PHYS, 0x100000000ULL);
    if (!ci_phys) {
        kprint("IWL9260: ctxt_info page alloc failed above 0x%08x\n",
               (uint32_t)IWL_CTXT_INFO_MIN_PHYS);
        return -1;
    }
    struct iwl_context_info *ci =
        (struct iwl_context_info *)(uintptr_t)vmm_phys_to_virt(ci_phys);
    iwl_zero(ci, 4096u);
    g_iwl_ctxt_info = ci;
    g_iwl_ctxt_info_phys = ci_phys;

    /* --- version --- */
    uint32_t hw_rev = iwl_read32(CSR_HW_REV);
    ci->version.mac_id  = (uint16_t)(hw_rev & 0xFFFFu);
    ci->version.version = 0;
    ci->version.size    = (uint16_t)(sizeof(struct iwl_context_info) / 4u);

    /* --- control flags --- */
    uint32_t flags = CTXT_INFO_TFD_FORMAT_LONG;
    flags |= (CTXT_INFO_RB_SIZE_4K << CTXT_INFO_RB_SIZE_POS);   /* 0x4 << 9 */
    flags |= (8u << CTXT_INFO_RB_CB_SIZE_POS);                  /* log2(256)=8 << 4 */
    ci->control.control_flags = flags;

    /* --- RX queue addresses --- */
    ci->rbd_cfg.free_rbd_addr = g_iwl_dma.rxbd_ring_phys;
    ci->rbd_cfg.used_rbd_addr = g_iwl_dma.rx_used_ring_phys;
    ci->rbd_cfg.status_wr_ptr = g_iwl_dma.rx_status_phys;

    /* --- command queue (TX) --- */
    ci->hcmd_cfg.cmd_queue_addr = g_iwl_dma.tfd_ring_phys;
    ci->hcmd_cfg.cmd_queue_size = 5;  /* TFD_QUEUE_CB_SIZE(256)=ilog2(256)-3 */

    /* --- split SEC_RT into LMAC / UMAC / paging by separator sections --- */
    uint32_t lmac_cnt = iwl_get_fw_sec_count(pieces->sec_rt,
                                              pieces->sec_rt_count, 0);
    uint32_t umac_start = lmac_cnt + 1;   /* skip separator */
    uint32_t umac_cnt   = 0;
    uint32_t pag_start  = 0;
    uint32_t pag_cnt    = 0;
    if (umac_start < pieces->sec_rt_count) {
        umac_cnt  = iwl_get_fw_sec_count(pieces->sec_rt,
                                          pieces->sec_rt_count, umac_start);
        pag_start = umac_start + umac_cnt + 1;
        if (pag_start < pieces->sec_rt_count)
            pag_cnt = iwl_get_fw_sec_count(pieces->sec_rt,
                                            pieces->sec_rt_count, pag_start);
    }

    iwl_dbg("IWL9260: ctxt_info sec split: lmac=%u umac=%u paging=%u (total=%u)\n",
            lmac_cnt, umac_cnt, pag_cnt, pieces->sec_rt_count);

    /* --- copy each group into DMA buffers and fill DRAM entries --- */
    uint32_t di = 0;  /* running DMA-buffer index */
    int rc;

    for (uint32_t i = 0; i < lmac_cnt && i < IWL_MAX_DRAM_ENTRY; i++) {
        rc = iwl_alloc_fw_dma_sec(&pieces->sec_rt[i], di);
        if (rc) return rc;
        ci->dram.lmac_img[i] = g_iwl_fw_dma.phys[di];
        di++;
    }
    for (uint32_t i = 0; i < umac_cnt && i < IWL_MAX_DRAM_ENTRY; i++) {
        uint32_t si = umac_start + i;
        rc = iwl_alloc_fw_dma_sec(&pieces->sec_rt[si], di);
        if (rc) return rc;
        ci->dram.umac_img[i] = g_iwl_fw_dma.phys[di];
        di++;
    }
    for (uint32_t i = 0; i < pag_cnt && i < IWL_MAX_DRAM_ENTRY; i++) {
        uint32_t si = pag_start + i;
        rc = iwl_alloc_fw_dma_sec(&pieces->sec_rt[si], di);
        if (rc) return rc;
        ci->dram.virtual_img[i] = g_iwl_fw_dma.phys[di];
        di++;
    }

    iwl_dbg("IWL9260: %u FW sections copied to DMA\n", di);
    iwl_dbg("IWL9260: ctxt_info page phys 0x%08x\n", (uint32_t)ci_phys);

    /* Stamp unused tail bytes so wd can tell if the page is getting clobbered. */
    iwl_write32_mem(ci, IWL_CTXT_CANARY0_OFF, IWL_CTXT_CANARY0_VAL);
    iwl_write32_mem(ci, IWL_CTXT_CANARY1_OFF, IWL_CTXT_CANARY1_VAL);

    /* --- flush context info to RAM so device sees it --- */
    iwl_cache_flush(ci, 4096u);

    /* Ownership was already claimed in iwl_nic_reset → iwl_pcie_prepare_card_hw.
       Now arm ALIVE/FH-RX interrupts and advertise OS presence. */
    iwl_enable_fw_alive_ints();
    iwl_arm_ctxt_info_boot();

    /* NOTE: LMPM_SECURE_UCODE registers are used in the gen1 direct-DMA FW
       upload path (iwl_upload_firmware).  The context-info boot path below
       does NOT write them — context info has its own image descriptor. */

    /* --- tell hardware where the context info lives --- */
    g_iwl_bootdbg.ctxt_info_write_lo = (uint32_t)ci_phys;
    g_iwl_bootdbg.ctxt_info_write_hi = (uint32_t)(ci_phys >> 32);
    iwl_write64_split(CSR_CTXT_INFO_BA, ci_phys);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_CTXT_INFO_WRITTEN;
    iwl_udelay(10);

    {
        uint32_t want_lo = (uint32_t)ci_phys;
        uint32_t want_hi = (uint32_t)(ci_phys >> 32);
        uint32_t ci_lo = iwl_read32(CSR_CTXT_INFO_BA);
        uint32_t ci_hi = iwl_read32(CSR_CTXT_INFO_BA + 4);
        if (ci_lo != want_lo || ci_hi != want_hi) {
            iwl_dbg("IWL9260: CTXT_INFO_BA mismatch want=%08x:%08x read=%08x:%08x retry\n",
                    want_hi, want_lo, ci_hi, ci_lo);

            /*
             * Re-arm AUTO_FUNC_BOOT and rewrite the pair high->low.
             * Some 64-bit CSR pairs latch on the low dword write.
             */
            iwl_arm_ctxt_info_boot();
            iwl_write32(CSR_CTXT_INFO_BA + 4u, want_hi);
            iwl_write32(CSR_CTXT_INFO_BA, want_lo);
            iwl_udelay(10);
            ci_lo = iwl_read32(CSR_CTXT_INFO_BA);
            ci_hi = iwl_read32(CSR_CTXT_INFO_BA + 4);
        }

        g_iwl_bootdbg.ctxt_info_read_lo = ci_lo;
        g_iwl_bootdbg.ctxt_info_read_hi = ci_hi;
        g_iwl_bootdbg.flags |= IWL_BOOTDBG_CTXT_INFO_READ;
        iwl_dbg("IWL9260: CTXT_INFO_BA write phys=%08x:%08x read=%08x:%08x\n",
                want_hi, want_lo, ci_hi, ci_lo);
    }

    /* CRITICAL: Kick the NIC CPU to start executing.
       Linux does: iwl_write_prph(trans, UREG_CPU_INIT_RUN, 1);
       Without this, the CPU never reads context info and ALIVE never arrives. */
    g_iwl_bootdbg.cpu_init_run_written = 1u;
    iwl_write_prph(UREG_CPU_INIT_RUN, 1);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_CPU_KICK_WRITTEN;
    g_iwl_bootdbg.cpu_init_run_readback = iwl_read_prph(UREG_CPU_INIT_RUN);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_CPU_KICK_READ;

    iwl_dbg("IWL9260: boot ROM loading FW via context info (CPU kicked)\n");
    return 0;
}

/* ---- Step 5: ALIVE Handshake ---- */

#define IWL_LEGACY_GROUP        0x00   /* Core43/MVM legacy command group */
#define IWL_SYSTEM_GROUP        0x02
#define IWL_ALIVE_CMD_ID        0x01   /* UCODE_ALIVE_NTFY */
#define IWL_ALIVE_STATUS_OK     0xCAFE
#define IWL_PHY_CFG_CMD         0x6A   /* PHY_CONFIGURATION_CMD */
#define IWL_CALIB_RES_NOTIF_PHY_DB 0x6B /* Calibration result from init FW */
#define IWL_PHY_DB_CMD          0x6C   /* PHY DB data -> operational FW */
#define IWL_NVM_ACCESS_CMD      0x88   /* NVM read/write */
#define IWL_TX_ANT_CONFIGURATION_CMD 0x98
#define IWL_MVM_ALIVE_CMD       IWL_ALIVE_CMD_ID
#define IWL_INIT_COMPLETE_NOTIF 0x04   /* Init complete notification */
#define IWL_NVM_GET_INFO        0x00   /* NVM get info (group 0x0C) */
#define FW_PHY_CFG_TX_CHAIN_POS 16u
#define FW_PHY_CFG_TX_CHAIN     (0xFu << FW_PHY_CFG_TX_CHAIN_POS)
#define FW_PHY_CFG_RX_CHAIN_POS 20u
#define FW_PHY_CFG_RX_CHAIN     (0xFu << FW_PHY_CFG_RX_CHAIN_POS)
#define IWL_DEFAULT_CHAIN_MASK  0x3u

struct iwl_lmac_debug_addrs {
    uint32_t error_event_table_ptr;
    uint32_t log_event_table_ptr;
    uint32_t cpu_register_ptr;
    uint32_t dbgm_config_ptr;
    uint32_t alive_counter_ptr;
    uint32_t scd_base_ptr;
    uint32_t st_fwrd_addr;
    uint32_t st_fwrd_size;
} __attribute__((packed));

struct iwl_lmac_alive {
    uint32_t ucode_major;
    uint32_t ucode_minor;
    uint8_t  ver_subtype;
    uint8_t  ver_type;
    uint8_t  mac;
    uint8_t  opt;
    uint32_t timestamp;
    struct iwl_lmac_debug_addrs dbg_ptrs;
} __attribute__((packed));

struct iwl_umac_debug_addrs {
    uint32_t error_info_addr;
    uint32_t dbg_print_buff_addr;
} __attribute__((packed));

struct iwl_umac_alive {
    uint32_t umac_major;
    uint32_t umac_minor;
    struct iwl_umac_debug_addrs dbg_ptrs;
} __attribute__((packed));

struct iwl_alive_ntf_v3 {
    uint16_t status;
    uint16_t flags;
    struct iwl_lmac_alive lmac_data;
    struct iwl_umac_alive umac_data;
} __attribute__((packed));

struct iwl_alive_ntf_v4 {
    uint16_t status;
    uint16_t flags;
    struct iwl_lmac_alive lmac_data[2];
    struct iwl_umac_alive umac_data;
} __attribute__((packed));

/* MSI state for WiFi */
static int g_iwl_msi_vector = -1;
static volatile uint32_t g_iwl_irq_fired;
static volatile uint32_t g_iwl_last_csr_int;
static volatile uint32_t g_iwl_last_fh_int;

static uint16_t iwl_rx_next_rb(uint16_t idx) {
    return (uint16_t)((idx + 1u) % IWL_RX_QUEUE_SIZE);
}

static uint64_t iwl_rx_encode_bd_addr(uint64_t phys, uint16_t vid) {
    return phys | (uint64_t)vid;
}

static int iwl_rx_used_entry_to_idx(uint32_t used_entry) {
    uint16_t vid = (uint16_t)(used_entry & 0x0FFFu);
    if (vid == 0 || vid > IWL_RX_QUEUE_SIZE)
        return -1;
    return (int)(vid - 1u);
}

/* IRQ handler: just set a flag, actual processing done by polling.
 *
 * fry663: Match Linux's CSR_INT ACK pattern — write back (inta | ~mask).
 * This clears both the bits that fired AND any masked-but-pending bits.
 * Without this, stale masked interrupts can block the HW interrupt
 * coalescing engine from generating new MSIs (known HW bug workaround). */
static void iwl_irq_handler(uint32_t vector, void *ctx,
                             void *dev_id, uint64_t error) {
    (void)vector; (void)ctx; (void)dev_id; (void)error;
    g_iwl_irq_fired++;

    if (g_iwl_mmio) {
        volatile uint32_t *int_reg = (volatile uint32_t *)(uintptr_t)(g_iwl_mmio + CSR_INT);
        volatile uint32_t *mask_reg = (volatile uint32_t *)(uintptr_t)(g_iwl_mmio + CSR_INT_MASK);
        volatile uint32_t *fh_int_reg =
            (volatile uint32_t *)(uintptr_t)(g_iwl_mmio + CSR_FH_INT_STATUS);
        uint32_t csr_int = *int_reg;
        uint32_t fh_int = *fh_int_reg;
        uint32_t mask = *mask_reg;
        g_iwl_last_csr_int = csr_int;
        g_iwl_last_fh_int = fh_int;
        /* Linux ACK pattern: write (inta | ~mask) to clear fired bits PLUS
         * any masked-but-pending bits that could block future MSIs. */
        *int_reg = csr_int | ~mask;
        if (fh_int)
            *fh_int_reg = fh_int;
        if (g_iwl_boot_trace_count < 8)
            iwl_trace_capture("irq");
    }
}

/* Find PCI capability by ID (walk capability list) */
static uint8_t iwl_find_pci_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id) {
    uint16_t status = (uint16_t)(pci_ecam_read32(0, bus, slot, func, 0x04) >> 16);
    if (!(status & (1u << 4))) return 0; /* no capabilities list */

    uint8_t ptr = pci_ecam_read8(0, bus, slot, func, 0x34) & 0xFC;
    while (ptr) {
        uint8_t id = pci_ecam_read8(0, bus, slot, func, ptr);
        if (id == cap_id) return ptr;
        ptr = pci_ecam_read8(0, bus, slot, func, (uint16_t)(ptr + 1)) & 0xFC;
        if (ptr == 0) break;
    }
    return 0;
}

static void iwl_enable_pci_cmd_bits(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t cmd = iwl_read_cfg16(bus, slot, func, 0x04);
    uint16_t want = (uint16_t)(cmd | 0x0006u);
    if (want != cmd)
        iwl_write_cfg16(bus, slot, func, 0x04, want);
}

static int iwl_force_pm_d0(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t pmcap = iwl_find_pci_cap(bus, slot, func, 0x01);
    if (!pmcap)
        return -1;

    uint16_t pmcsr = iwl_read_cfg16(bus, slot, func, (uint16_t)(pmcap + 4u));
    uint16_t want = (uint16_t)((pmcsr & ~0x0003u) & ~0x0100u);
    if (pmcsr & 0x8000u)
        want |= 0x8000u;
    if (want != pmcsr)
        iwl_write_cfg16(bus, slot, func, (uint16_t)(pmcap + 4u), want);

    for (uint32_t i = 0; i < 20000u; i++) {
        uint16_t cur = iwl_read_cfg16(bus, slot, func, (uint16_t)(pmcap + 4u));
        if ((cur & 0x0003u) == 0)
            return 0;
        iwl_udelay(10);
    }

    return -1;
}

static void iwl_disable_pcie_aspm(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t pcie = iwl_find_pci_cap(bus, slot, func, 0x10);
    if (!pcie)
        return;

    uint16_t lnkctl = iwl_read_cfg16(bus, slot, func, (uint16_t)(pcie + 0x10u));
    if (lnkctl & 0x0003u) {
        lnkctl = (uint16_t)(lnkctl & ~0x0003u);
        iwl_write_cfg16(bus, slot, func, (uint16_t)(pcie + 0x10u), lnkctl);
    }

}

static int iwl_try_flr(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t pcie = iwl_find_pci_cap(bus, slot, func, 0x10);
    if (!pcie)
        return -1;

    uint32_t devcap = pci_ecam_read32(0, bus, slot, func, (uint16_t)(pcie + 0x04u));
    if (!(devcap & (1u << 28)))
        return -2;

    uint16_t devctl = iwl_read_cfg16(bus, slot, func, (uint16_t)(pcie + 0x08u));
    iwl_write_cfg16(bus, slot, func, (uint16_t)(pcie + 0x08u),
                    (uint16_t)(devctl | 0x8000u));
    iwl_udelay(100000u);
    return 0;
}

static const struct pci_device_info *iwl_find_parent_bridge(uint8_t child_bus) {
    uint32_t count = 0;
    const struct pci_device_info *devs = pci_get_devices(&count);
    if (!devs)
        return 0;

    for (uint32_t i = 0; i < count; i++) {
        const struct pci_device_info *dev = &devs[i];
        if (dev->class_code != 0x06 || dev->subclass != 0x04)
            continue;

        uint32_t buses = pci_ecam_read32(0, dev->bus, dev->slot, dev->func, 0x18);
        uint8_t secondary = (uint8_t)((buses >> 8) & 0xFFu);
        uint8_t subordinate = (uint8_t)((buses >> 16) & 0xFFu);
        if (secondary && child_bus >= secondary && child_bus <= subordinate)
            return dev;
    }

    return 0;
}

static void iwl_pci_prepare_link(const struct pci_device_info *dev, int allow_flr) {
    if (!dev)
        return;

    const struct pci_device_info *bridge = iwl_find_parent_bridge(dev->bus);
    if (bridge) {
        int br_pm = iwl_force_pm_d0(bridge->bus, bridge->slot, bridge->func);
        iwl_disable_pcie_aspm(bridge->bus, bridge->slot, bridge->func);
        iwl_dbg("IWL9260: parent bridge %u:%u.%u pm=%d\n",
                bridge->bus, bridge->slot, bridge->func, br_pm);
    }

    int ep_pm = iwl_force_pm_d0(dev->bus, dev->slot, dev->func);
    iwl_disable_pcie_aspm(dev->bus, dev->slot, dev->func);
    iwl_enable_pci_cmd_bits(dev->bus, dev->slot, dev->func);
    iwl_dbg("IWL9260: endpoint pm=%d allow_flr=%d\n", ep_pm, allow_flr);

    if (allow_flr) {
        int flr = iwl_try_flr(dev->bus, dev->slot, dev->func);
        iwl_enable_pci_cmd_bits(dev->bus, dev->slot, dev->func);
        iwl_dbg("IWL9260: endpoint flr=%d\n", flr);
    }
}

/* Enable MSI for the WiFi device (same pattern as NVMe driver) */
static int iwl_enable_msi(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t cap = iwl_find_pci_cap(bus, slot, func, 0x05); /* MSI cap ID = 0x05 */
    if (!cap) {
        kprint("IWL9260: no MSI capability found\n");
        return -1;
    }

    int vec = msi_alloc_vector();
    if (vec < 0) {
        kprint("IWL9260: MSI vector alloc failed\n");
        return -2;
    }

    if (request_irq((uint32_t)vec, iwl_irq_handler, 0, "iwl9260", (void *)0) != 0) {
        msi_free_vector(vec);
        kprint("IWL9260: request_irq failed for vec=%d\n", vec);
        return -3;
    }

    /* Read MSI control to check 64-bit capability */
    uint16_t ctrl = (uint16_t)(pci_ecam_read32(0, bus, slot, func, cap) >> 16);
    uint8_t is_64 = (ctrl & (1u << 7)) ? 1u : 0u;

    /* Disable MSI while configuring */
    ctrl &= (uint16_t)~1u;
    pci_ecam_write8(0, bus, slot, func, (uint16_t)(cap + 2), (uint8_t)(ctrl & 0xFF));
    pci_ecam_write8(0, bus, slot, func, (uint16_t)(cap + 3), (uint8_t)(ctrl >> 8));

    /* Program MSI address (LAPIC destination) */
    uint32_t msg_addr = 0xFEE00000u | ((uint32_t)lapic_get_id() << 12);
    pci_ecam_write32(0, bus, slot, func, (uint16_t)(cap + 4), msg_addr);

    /* Program MSI data (vector number) */
    uint8_t data_off = (uint8_t)(cap + 8u);
    if (is_64) {
        pci_ecam_write32(0, bus, slot, func, (uint16_t)(cap + 8), 0); /* upper addr = 0 */
        data_off = (uint8_t)(cap + 12u);
    }
    /* Write data as two bytes since pci_ecam_write8 is available */
    pci_ecam_write8(0, bus, slot, func, data_off, (uint8_t)(vec & 0xFF));
    pci_ecam_write8(0, bus, slot, func, (uint16_t)(data_off + 1), 0);

    /* Enable MSI, single message */
    ctrl = (uint16_t)(pci_ecam_read32(0, bus, slot, func, cap) >> 16);
    ctrl &= (uint16_t)~(0x7u << 4); /* single message */
    ctrl |= 1u;                     /* MSI enable */
    pci_ecam_write8(0, bus, slot, func, (uint16_t)(cap + 2), (uint8_t)(ctrl & 0xFF));
    pci_ecam_write8(0, bus, slot, func, (uint16_t)(cap + 3), (uint8_t)(ctrl >> 8));

    g_iwl_msi_vector = vec;
    g_iwl_irq_fired = 0;

    kprint("IWL9260: MSI enabled vec=%d addr=0x%08x %s\n",
           vec, msg_addr, is_64 ? "64-bit" : "32-bit");
    return 0;
}

#define IWL_INIT_FW_BOOT_ATTEMPTS 2u
#define IWL_ALIVE_WAIT_BAD_STATUS_RC  -2
#define IWL_ALIVE_WAIT_SW_ERR_RC      -3
#define IWL_ALIVE_WAIT_HW_ERR_RC      -4
#define IWL_ALIVE_WAIT_TIMEOUT_RC     -5

static uint8_t iwl_alive_phase_select(int use_init, uint8_t init_phase, uint8_t rt_phase) {
    return use_init ? init_phase : rt_phase;
}

static int iwl_trans_pcie_start_fw(const struct pci_device_info *target,
                                   const struct iwl_fw_pieces *pieces,
                                   uint8_t *failed_step,
                                   int use_init) {
    int ret;

    ret = iwl_pcie_prepare_card_hw();
    if (ret)
        return ret;

    iwl_enable_rfkill_int();
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    iwl_disable_interrupts();

    g_iwl_bootdbg.rfkill_mask = 0x06u;
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    g_iwl_bootdbg.flags |= IWL_BOOTDBG_RFKILL_CLEARED;

    iwl_write32(CSR_INT, 0xFFFFFFFFu);

    ret = iwl_pcie_nic_init(pieces, failed_step);
    if (ret)
        return ret;

    g_iwl_init_step = WIFI_STEP_MSI;
    if (failed_step)
        *failed_step = WIFI_STEP_MSI;
    ret = iwl_enable_msi(target->bus, target->slot, target->func);
    if (ret != 0) {
        iwl_dbg("IWL9260: MSI failed rc=%d (non-fatal)\n", ret);
    } else {
        iwl_dbg("IWL9260: MSI OK\n");
    }
    iwl_trace_capture("msi-on");

    iwl_enable_fw_load_int();
    iwl_trace_capture("load-mask");

    iwl_write32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);

    g_iwl_init_step = WIFI_STEP_FW_UPLOAD;
    if (failed_step)
        *failed_step = WIFI_STEP_FW_UPLOAD;
    return iwl_pcie_load_given_ucode_8000(pieces, use_init);
}

static int iwl_fw_boot_attempt(const struct pci_device_info *target,
                               const struct iwl_fw_pieces *pieces,
                               uint32_t attempt_idx,
                               uint8_t *failed_step,
                               int use_init) {
    uint8_t step = WIFI_STEP_NIC_RESET;
    g_iwl_init_step = step;
    if (failed_step)
        *failed_step = step;

    if (attempt_idx > 0)
        iwl_dbg("IWL9260: retrying firmware bring-up (attempt %u/%u)\n",
                attempt_idx + 1u, IWL_INIT_FW_BOOT_ATTEMPTS);

    int rc = _iwl_trans_pcie_start_hw();
    if (rc != 0) {
        iwl_dbg("IWL9260: start_hw FAILED rc=%d, retrying after PCI recovery\n", rc);
        iwl_pci_prepare_link(target, 1);
        if (iwl_read32(0x0) == 0xFFFFFFFFu)
            return rc;
        rc = _iwl_trans_pcie_start_hw();
        if (rc != 0) {
            iwl_dbg("IWL9260: start_hw retry FAILED rc=%d\n", rc);
            return rc;
        }
    }
    iwl_dbg("IWL9260: start_hw OK\n");

    rc = iwl_trans_pcie_start_fw(target, pieces, failed_step, use_init);
    if (rc != 0) {
        iwl_dbg("IWL9260: start_fw FAILED rc=%d\n", rc);
        return rc;
    }
    iwl_dbg("IWL9260: FW uploaded (%s image)\n", use_init ? "INIT" : "RT");

    step = WIFI_STEP_ALIVE;
    g_iwl_init_step = step;
    if (failed_step)
        *failed_step = step;
    rc = iwl_wait_alive(use_init, attempt_idx);
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(use_init ? IWL_CMD_PHASE_INIT_ALIVE_FAIL
                                          : IWL_CMD_PHASE_RT_ALIVE_FAIL,
                                 rc, (uint16_t)(attempt_idx + 1u));
        iwl_dbg("IWL9260: ALIVE FAILED rc=%d\n", rc);
        return rc;
    }
    iwl_cmd_trace_mark_phase(use_init ? IWL_CMD_PHASE_INIT_ALIVE_OK
                                      : IWL_CMD_PHASE_RT_ALIVE_OK,
                             0, (uint16_t)(attempt_idx + 1u));
    iwl_dbg("IWL9260: *** FIRMWARE IS ALIVE ***\n");
    return 0;
}

/*
 * Poll the RX queue for an ALIVE notification from firmware.
 *
 * After firmware starts, it DMAs an ALIVE notification into the RX ring.
 * We poll the RX status writeback area and check RX buffers for the ALIVE
 * command response.
 *
 * Timeout: 2 seconds (firmware should respond within ~200ms).
 */
static int iwl_wait_alive(int use_init, uint32_t attempt_idx) {
    if (!g_iwl_dma.allocated) return -1;

    iwl_dbg("IWL9260: waiting for ALIVE...\n");

    /* Keep the full interrupt mask that iwl_upload_runtime_cpu_image set
     * before writing the done marker.  Linux never narrows the mask between
     * firmware upload and the ALIVE wait — CSR_INI_SET_MASK stays on.
     * Narrowing to ALIVE|FH_RX caused missed SW_ERR/HW_ERR notifications
     * and broke the handshake on some firmware versions.  (fry689) */
    uint32_t alive_mask = iwl_full_int_mask();
    iwl_write32(CSR_INT, 0xFFFFFFFFu);  /* clear any pending before we start */
    iwl_write32(CSR_INT_MASK, alive_mask);
    g_iwl_bootdbg.alive_int_mask = alive_mask;
    iwl_trace_capture("alive-wait");

    /* Re-doorbell the RX free-ring immediately before polling for ALIVE.
     * The runtime CPUs can be executing while RFH still shows zero progress,
     * so force the free-ring advertisement again at the handoff boundary. */
    iwl_rfh_kick_rx_free_ring("alive-kick");

    uint32_t timeout_us = 2000000u; /* 2 seconds */
    uint32_t elapsed = 0;
    uint32_t poll_interval = 1000u; /* check every 1ms */
    uint32_t saw_alive_bit = 0;
    uint32_t saw_sw_err = 0;
    uint32_t saw_hw_err = 0;
    uint32_t err_csr_int = 0;
    uint32_t err_fh_int = 0;
    uint16_t err_elapsed_ms = 0;
    uint32_t last_diag_slot = 0xFFFFFFFFu;
    uint32_t rx_rekick_done = 0;
    uint32_t alive_seen_restock = 0;
    uint32_t err_break_deadline_us = 50000u;

    while (elapsed < timeout_us) {
        /* Read and clear CSR_INT on EVERY poll iteration.
         * Use Linux ACK pattern: write (inta | ~mask) to also clear
         * masked-but-pending bits that could block future MSIs. */
        uint32_t csr_int = iwl_read32(CSR_INT);
        uint32_t fh_int = 0;
        if (csr_int && csr_int != 0xFFFFFFFFu) {
            fh_int = iwl_read32(CSR_FH_INT_STATUS);

            if (csr_int & CSR_INT_BIT_ALIVE) {
                saw_alive_bit = 1;
                if (!alive_seen_restock) {
                    /* Linux treats the first ALIVE interrupt as the point
                     * where the RX side can be restocked safely. Mirror that
                     * here before we conclude that the paired SW_ERR is fatal.
                     */
                    iwl_dbg("IWL9260: ALIVE seen, re-advertising RX free ring\n");
                    iwl_rfh_kick_rx_free_ring("alive-seen-kick");
                    alive_seen_restock = 1;
                }
            }
            if ((csr_int & CSR_INT_BIT_SW_ERR) && !saw_sw_err) {
                saw_sw_err = 1;
                err_csr_int = csr_int;
                err_fh_int = fh_int;
                err_elapsed_ms = (uint16_t)(elapsed / 1000u);
                iwl_trace_capture("alive-swerr-seen");
            }
            if ((csr_int & CSR_INT_BIT_HW_ERR) && !saw_hw_err) {
                saw_hw_err = 1;
                err_csr_int = csr_int;
                err_fh_int = fh_int;
                err_elapsed_ms = (uint16_t)(elapsed / 1000u);
                iwl_trace_capture("alive-hwerr-seen");
            }

            iwl_write32(CSR_INT, csr_int | ~alive_mask);
        }

        /* Log core NIC state + CPU status every 500ms. */
        uint32_t diag_slot = elapsed / 500000u;
        if (diag_slot != last_diag_slot) {
            uint32_t csr_reset = iwl_read32(CSR_RESET);
            uint32_t gp_cntrl = iwl_read32(CSR_GP_CNTRL);
            iwl_dbg("IWL9260: @%ums INT=%08x RST=%08x GP=%08x irq=%u\n",
                    elapsed / 1000u, csr_int, csr_reset, gp_cntrl, g_iwl_irq_fired);
            /* fry663: Read CPU status and PC to determine if FW is executing */
            if (iwl_grab_nic_access() == 0) {
                uint32_t cpu1_st = iwl_read_prph(SB_CPU_1_STATUS);
                uint32_t cpu2_st = iwl_read_prph(SB_CPU_2_STATUS);
                uint32_t umac_pc = iwl_read_prph(UREG_UMAC_CURRENT_PC);
                uint32_t lmac_pc = iwl_read_prph(UREG_LMAC1_CURRENT_PC);
                iwl_dbg("IWL9260:   CPU1=%08x CPU2=%08x UMAC_PC=%08x LMAC_PC=%08x\n",
                        cpu1_st, cpu2_st, umac_pc, lmac_pc);
                /* fry718: SRAM checkpoint during ALIVE wait — detect when SRAM gets wiped */
                uint32_t s0 = iwl_read_sram(0x00000000u);
                uint32_t s1 = iwl_read_sram(0x00000004u);
                uint32_t s2 = iwl_read_sram(0x00404000u);
                iwl_dbg("IWL9260:   SRAM[0x00]=%08x [0x04]=%08x [0x404000]=%08x\n",
                        s0, s1, s2);
            }
            last_diag_slot = diag_slot;
        }

        /* If we saw SW_ERR or HW_ERR, firmware has crashed — break early. */
        if ((saw_sw_err || saw_hw_err) && elapsed > err_break_deadline_us) {
            iwl_dbg("IWL9260: %s detected at %ums, breaking early\n",
                    saw_hw_err ? "HW_ERR" : "SW_ERR", elapsed / 1000u);
            break;
        }

        /* Invalidate cache on RX status to see device writes */
        iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));

        /* 9000-series: closed_rb_num[11:0] = write index into used ring */
        uint16_t closed_rb = g_iwl_dma.rx_status->closed_rb_num & 0x0FFF;

        if (closed_rb >= IWL_RX_QUEUE_SIZE) {
            iwl_dbg("IWL9260: RX invalid closed_rb=%u\n", closed_rb);
            closed_rb = 0;
        }

        if (!rx_rekick_done && elapsed >= 50000u && closed_rb == 0) {
            iwl_dbg("IWL9260: ALIVE wait sees no RX progress, re-kicking RFH\n");
            iwl_cmd_trace_mark_phase(
                iwl_alive_phase_select(use_init,
                                       IWL_CMD_PHASE_INIT_ALIVE_REKICK,
                                       IWL_CMD_PHASE_RT_ALIVE_REKICK),
                0, (uint16_t)(attempt_idx + 1u));
            iwl_rfh_kick_rx_free_ring("alive-rekick");
            rx_rekick_done = 1;
            /* Give the re-kick a bounded chance to produce an RX completion
             * before the saved SW_ERR/HW_ERR path aborts the wait loop.
             */
            err_break_deadline_us = elapsed + 10000u;
        }

        while (g_iwl_dma.rx_read_idx != closed_rb) {
            uint16_t idx = g_iwl_dma.rx_read_idx;

            /* Used-ring completions carry the 12-bit VID, not the ring slot. */
            iwl_cache_flush(&g_iwl_dma.rx_used_ring[idx], sizeof(uint32_t));
            uint32_t used_entry = g_iwl_dma.rx_used_ring[idx];
            int rb_idx = iwl_rx_used_entry_to_idx(used_entry);
            if (rb_idx < 0) {
                iwl_dbg("IWL9260: RX[%u] invalid used entry=%08x\n", idx, used_entry);
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                continue;
            }
            uint16_t rb_id = (uint16_t)rb_idx;

            iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], 64);

            const struct iwl_rx_packet *pkt =
                (const struct iwl_rx_packet *)g_iwl_dma.rx_buf_virt[rb_id];
            uint32_t raw_len = iwl_rx_packet_total_len(pkt);
            uint32_t payload_len = iwl_rx_packet_payload_len(pkt);
            uint8_t cmd = pkt->cmd_id;
            uint8_t group = pkt->group_id;

            if (raw_len >= sizeof(struct iwl_rx_packet) && raw_len <= IWL_RX_BUF_SIZE)
                iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], raw_len);
            else
                raw_len = 64;

            if (group == IWL_LEGACY_GROUP && cmd == IWL_ALIVE_CMD_ID) {
                uint16_t status = 0;
                uint16_t flags = 0;
                const struct iwl_lmac_alive *lmac = 0;

                if (payload_len >= sizeof(struct iwl_alive_ntf_v4)) {
                    const struct iwl_alive_ntf_v4 *alive =
                        (const struct iwl_alive_ntf_v4 *)pkt->data;
                    status = alive->status;
                    flags = alive->flags;
                    lmac = &alive->lmac_data[0];
                } else if (payload_len >= sizeof(struct iwl_alive_ntf_v3)) {
                    const struct iwl_alive_ntf_v3 *alive =
                        (const struct iwl_alive_ntf_v3 *)pkt->data;
                    status = alive->status;
                    flags = alive->flags;
                    lmac = &alive->lmac_data;
                } else {
                    iwl_dbg("IWL9260: ALIVE too short (%u bytes)\n",
                            (unsigned)payload_len);
                    iwl_trace_capture("alive-bad-status");
                    return IWL_ALIVE_WAIT_BAD_STATUS_RC;
                }

                if (lmac) {
                    iwl_dbg("IWL9260: ALIVE st=%04x fl=%04x ucode=%u.%u\n",
                            status, flags,
                            lmac->ucode_major, lmac->ucode_minor);
                    iwl_dbg("IWL9260: err_tbl=%08x log_tbl=%08x\n",
                            lmac->dbg_ptrs.error_event_table_ptr,
                            lmac->dbg_ptrs.log_event_table_ptr);
                }

                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                if (status != IWL_ALIVE_STATUS_OK) {
                    iwl_trace_capture("alive-bad-status");
                    iwl_cmd_trace_mark_phase(
                        iwl_alive_phase_select(use_init,
                                               IWL_CMD_PHASE_INIT_ALIVE_BAD_STATUS,
                                               IWL_CMD_PHASE_RT_ALIVE_BAD_STATUS),
                        IWL_ALIVE_WAIT_BAD_STATUS_RC,
                        (uint16_t)(attempt_idx + 1u));
                    iwl_dbg("IWL9260: ALIVE bad status (want %04x)\n",
                            IWL_ALIVE_STATUS_OK);
                    return IWL_ALIVE_WAIT_BAD_STATUS_RC;
                }
                iwl_trace_capture("alive-rx");
                return 0;
            }

            iwl_dbg("IWL9260: RX[%u] cmd=%02x grp=%02x len=%u\n",
                    idx, cmd, group, (unsigned)payload_len);
            g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
        }

        iwl_udelay(poll_interval);
        elapsed += poll_interval;
    }

    /* Timeout / SW_ERR / HW_ERR — dump diagnostic state */
    uint32_t gp = iwl_read32(CSR_GP_CNTRL);
    uint32_t csr_int_final = iwl_read32(CSR_INT);
    uint32_t csr_reset = iwl_read32(CSR_RESET);
    uint32_t fw_st = iwl_read32(FH_UCODE_LOAD_STATUS);
    uint16_t closed_rb = g_iwl_dma.rx_status->closed_rb_num & 0x0FFF;
    uint16_t finished_rb = g_iwl_dma.rx_status->finished_rb_num & 0x0FFF;
    iwl_dbg("IWL9260: ALIVE %s! (alive=%u sw_err=%u hw_err=%u)\n",
            saw_hw_err ? "HW_ERR" : (saw_sw_err ? "SW_ERR" : "TIMEOUT"),
            saw_alive_bit, saw_sw_err, saw_hw_err);
    iwl_dbg("GP=%08x INT=%08x RESET=%08x FW=%08x\n", gp, csr_int_final, csr_reset, fw_st);
    iwl_dbg("RX: rd=%u cl=%u fin=%u irq=%u\n",
            g_iwl_dma.rx_read_idx, closed_rb, finished_rb, g_iwl_irq_fired);

    /* fry663: Read CPU status + PC at timeout for definitive FW state */
    if (iwl_grab_nic_access() == 0) {
        uint32_t cpu1_a = iwl_read_prph(SB_CPU_1_STATUS);
        uint32_t cpu2_a = iwl_read_prph(SB_CPU_2_STATUS);
        uint32_t pc1_a  = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t pc2_a  = iwl_read_prph(UREG_LMAC1_CURRENT_PC);
        iwl_udelay(10000); /* 10ms delay */
        uint32_t pc1_b  = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t pc2_b  = iwl_read_prph(UREG_LMAC1_CURRENT_PC);
        iwl_dbg("CPU1_ST=%08x CPU2_ST=%08x\n", cpu1_a, cpu2_a);
        iwl_dbg("UMAC_PC=%08x->%08x LMAC_PC=%08x->%08x %s\n",
                pc1_a, pc1_b, pc2_a, pc2_b,
                (pc1_a != pc1_b || pc2_a != pc2_b) ? "RUNNING" : "STUCK");
    }

    /* Dump first RX buffer to see if anything landed */
    iwl_cache_flush(g_iwl_dma.rx_buf_virt[0], 64);
    const struct iwl_rx_packet *rx0 = (const struct iwl_rx_packet *)g_iwl_dma.rx_buf_virt[0];
    iwl_dbg("RX[0] len=%u cmd=%02x grp=%02x\n",
            (unsigned)iwl_rx_packet_total_len(rx0),
            rx0->cmd_id, rx0->group_id);
    /* Hex dump first 32 bytes of RX[0] for deep inspection */
    {
        const uint8_t *p = (const uint8_t *)g_iwl_dma.rx_buf_virt[0];
        iwl_dbg("RX[0] hex: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
        iwl_dbg("         %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                p[16],p[17],p[18],p[19], p[20],p[21],p[22],p[23],
                p[24],p[25],p[26],p[27], p[28],p[29],p[30],p[31]);
    }

    /* Dump RFH queue state to see if HW consumed any RBDs */
    if (iwl_grab_nic_access() == 0) {
        uint32_t rfh_widx = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        uint32_t rfh_ridx = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        uint32_t rfh_uwid = iwl_read_prph(RFH_Q0_URBDCB_WIDX);
        uint32_t rfh_act  = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        uint32_t rfh_dma  = iwl_read_prph(RFH_RXF_DMA_CFG);
        uint32_t rfh_gen  = iwl_read_prph(RFH_GEN_STATUS);
        iwl_dbg("RFH: WIDX=%u RIDX=%u UWID=%u ACT=%08x DMA=%08x GEN=%08x\n",
                rfh_widx, rfh_ridx, rfh_uwid, rfh_act, rfh_dma, rfh_gen);
    }

    /* Dump UMAC error table at 0x800000 with more fields */
    if (iwl_grab_nic_access() == 0) {
        iwl_dbg("IWL9260: UMAC error table @0x800000:\n");
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t v = iwl_read_sram(0x00800000u + i * 4u);
            iwl_dbg("  +%02x: %08x\n", i * 4u, v);
        }
        /* Also dump SRAM at 0x800200 (secondary error region) */
        iwl_dbg("IWL9260: UMAC @0x800200:\n");
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t v = iwl_read_sram(0x00800200u + i * 4u);
            iwl_dbg("  +%02x: %08x\n", i * 4u, v);
        }
    }

    /* Always try to dump SRAM error table (firmware may have crashed) */
    iwl_dbg("IWL9260: dumping SRAM error table...\n");
    iwl_dump_fw_error_table();
    iwl_log_fw_verify_summary();
    iwl_trace_capture(saw_hw_err ? "alive-hwerr"
                                 : (saw_sw_err ? "alive-swerr" : "alive-timeout"));

    int fail_rc = IWL_ALIVE_WAIT_TIMEOUT_RC;
    uint8_t fail_phase = iwl_alive_phase_select(use_init,
                                                IWL_CMD_PHASE_INIT_ALIVE_TIMEOUT,
                                                IWL_CMD_PHASE_RT_ALIVE_TIMEOUT);
    if (saw_hw_err) {
        fail_rc = IWL_ALIVE_WAIT_HW_ERR_RC;
        fail_phase = iwl_alive_phase_select(use_init,
                                            IWL_CMD_PHASE_INIT_ALIVE_HW_ERR,
                                            IWL_CMD_PHASE_RT_ALIVE_HW_ERR);
    } else if (saw_sw_err) {
        fail_rc = IWL_ALIVE_WAIT_SW_ERR_RC;
        fail_phase = iwl_alive_phase_select(use_init,
                                            IWL_CMD_PHASE_INIT_ALIVE_SW_ERR,
                                            IWL_CMD_PHASE_RT_ALIVE_SW_ERR);
    }
    iwl_cmd_trace_mark_phase_state(fail_phase, fail_rc, (uint16_t)(attempt_idx + 1u),
                                   err_elapsed_ms, err_csr_int, err_fh_int);

    return fail_rc;
}

static int iwl_phy_db_store_notif(const struct iwl_rx_packet *pkt);
static int iwl_wait_resp_ex(uint8_t expect_cmd_id, uint8_t expect_group_id,
                            uint16_t seq, uint32_t timeout_ms,
                            void *resp_buf, uint32_t resp_max);

static int iwl_wait_init_complete(void) {
    if (!g_iwl_dma.allocated)
        return -1;

    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_WAIT_ENTER, 0, 0);
    iwl_dbg("IWL9260: waiting for INIT_COMPLETE/PHY_DB notifications...\n");
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    iwl_write32(CSR_INT_MASK, iwl_full_int_mask());
    iwl_rfh_kick_rx_free_ring("init-wait");

    uint32_t timeout_us = 2000000u;
    uint32_t elapsed = 0;
    uint32_t poll_interval = 1000u;
    uint32_t phy_db_count = 0;

    while (elapsed < timeout_us) {
        iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
        uint16_t closed_rb = g_iwl_dma.rx_status->closed_rb_num & 0x0FFFu;

        if (closed_rb >= IWL_RX_QUEUE_SIZE)
            closed_rb = 0;

        while (g_iwl_dma.rx_read_idx != closed_rb) {
            uint16_t idx = g_iwl_dma.rx_read_idx;

            iwl_cache_flush(&g_iwl_dma.rx_used_ring[idx], sizeof(uint32_t));
            uint32_t used_entry = g_iwl_dma.rx_used_ring[idx];
            int rb_idx = iwl_rx_used_entry_to_idx(used_entry);
            if (rb_idx < 0) {
                iwl_dbg("IWL9260: init wait invalid used entry=%08x idx=%u\n",
                        used_entry, idx);
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                continue;
            }

            uint16_t rb_id = (uint16_t)rb_idx;
            iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], 64);

            const struct iwl_rx_packet *pkt =
                (const struct iwl_rx_packet *)g_iwl_dma.rx_buf_virt[rb_id];
            uint32_t raw_len = iwl_rx_packet_total_len(pkt);
            if (raw_len >= sizeof(struct iwl_rx_packet) && raw_len <= IWL_RX_BUF_SIZE)
                iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], raw_len);

            if (pkt->cmd_id == IWL_CALIB_RES_NOTIF_PHY_DB) {
                int rc = iwl_phy_db_store_notif(pkt);
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                if (rc != 0) {
                    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_WAIT_FAIL, rc,
                                             (uint16_t)phy_db_count);
                    iwl_dbg("IWL9260: PHY DB notif parse failed rc=%d\n", rc);
                    return rc;
                }
                phy_db_count++;
                continue;
            }

            if (pkt->cmd_id == IWL_INIT_COMPLETE_NOTIF) {
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_WAIT_OK, 0,
                                         (uint16_t)phy_db_count);
                iwl_dbg("IWL9260: INIT_COMPLETE received after %u PHY DB notifications\n",
                        phy_db_count);
                return 0;
            }

            g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
        }

        iwl_udelay(poll_interval);
        elapsed += poll_interval;
    }

    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_WAIT_FAIL, -2,
                             (uint16_t)phy_db_count);
    iwl_dbg("IWL9260: INIT_COMPLETE timeout\n");
    return -2;
}

/* ---- Step 6: Host Command Interface ---- */

/* TX doorbell register — write (queue_id << 16 | write_idx) to ring */
#define HBUS_TARG_WRPTR         0x460

/* Host command header (sent to firmware via TFD ring) */
struct iwl_host_cmd_hdr {
    uint8_t  cmd_id;     /* command opcode */
    uint8_t  group_id;   /* command group (0 = legacy, 1 = long, 0xFF = system) */
    uint16_t sequence;   /* sequence number for matching responses */
    uint16_t length;     /* payload length (after this header) */
    uint8_t  reserved;
    uint8_t  flags;      /* 0 = no response expected, 1 = response expected */
} __attribute__((packed));

#define IWL_CMD_HDR_SIZE    sizeof(struct iwl_host_cmd_hdr)  /* 8 bytes */

/* Command flags */
#define IWL_CMD_FLAG_RESP   0x01  /* firmware should send a response */

/* Sequence counter */
static uint16_t g_iwl_cmd_seq;
static uint32_t g_iwl_fw_phy_cfg;

enum iwl_phy_db_section_type {
    IWL_PHY_DB_CFG = 1,
    IWL_PHY_DB_CALIB_NCH = 2,
    IWL_PHY_DB_UNUSED = 3,
    IWL_PHY_DB_CALIB_CHG_PAPD = 4,
    IWL_PHY_DB_CALIB_CHG_TXP = 5,
    IWL_PHY_DB_MAX = 6
};

/* NVM access command payload */
struct iwl_nvm_access_cmd {
    uint8_t  op_code;   /* 0 = read */
    uint8_t  target;    /* 0 = NVM */
    uint16_t type;      /* section type */
    uint16_t offset;    /* offset within section */
    uint16_t length;    /* bytes to read */
} __attribute__((packed));

/* NVM section types for 9000 series */
#define NVM_SECTION_TYPE_HW             0
#define NVM_SECTION_TYPE_SW             1
#define NVM_SECTION_TYPE_CALIBRATION    4
#define NVM_SECTION_TYPE_PRODUCTION     5
#define NVM_SECTION_TYPE_REGULATORY     6
#define NVM_SECTION_TYPE_MAC_OVERRIDE   11
#define NVM_SECTION_TYPE_PHY_SKU        12

/* PHY configuration command payload (simplified) */
struct iwl_phy_cfg_cmd {
    uint32_t phy_cfg;
    uint32_t calib_control_tx;
    uint32_t calib_control_rx;
} __attribute__((packed));

struct iwl_tx_ant_cfg_cmd {
    uint32_t valid;
} __attribute__((packed));

struct iwl_calib_res_notif_phy_db {
    uint16_t type;
    uint16_t length;
    uint8_t  data[];
} __attribute__((packed));

struct iwl_phy_db_cmd {
    uint16_t type;
    uint16_t length;
    uint8_t  data[];
} __attribute__((packed));

struct iwl_phy_db_entry {
    uint16_t size;
    uint8_t *data;
};

static struct {
    struct iwl_phy_db_entry cfg;
    struct iwl_phy_db_entry calib_nch;
    uint16_t n_group_papd;
    struct iwl_phy_db_entry *calib_ch_group_papd;
    uint16_t n_group_txp;
    struct iwl_phy_db_entry *calib_ch_group_txp;
} g_iwl_phy_db;

/*
 * Send a host command to the firmware via TFD ring queue 0.
 *
 * cmd_id:    command opcode
 * group_id:  command group (0 for most commands)
 * data:      command payload (NULL if no payload)
 * data_len:  payload length in bytes
 * want_resp: 1 if we expect a response
 *
 * Returns the sequence number used (for matching response), or -1 on error.
 */
int iwl_send_cmd(uint8_t cmd_id, uint8_t group_id,
                  const void *data, uint16_t data_len, int want_resp) {
    if (!g_iwl_dma.allocated) return -1;
    if (data_len + IWL_CMD_HDR_SIZE > 4096) return -2; /* too large */

    /* Pick a command buffer (round-robin) */
    uint8_t buf_idx = g_iwl_dma.cmd_buf_idx;
    g_iwl_dma.cmd_buf_idx = (uint8_t)((buf_idx + 1) % IWL_DMA_CMDBUF_PAGES);

    uint8_t *cmd_buf = g_iwl_dma.cmd_buf_virt[buf_idx];
    uint64_t cmd_phys = g_iwl_dma.cmd_buf_phys[buf_idx];

    /* Build command header */
    struct iwl_host_cmd_hdr *hdr = (struct iwl_host_cmd_hdr *)cmd_buf;
    iwl_zero(cmd_buf, IWL_CMD_HDR_SIZE + data_len);
    hdr->cmd_id = cmd_id;
    hdr->group_id = group_id;
    hdr->sequence = g_iwl_cmd_seq++;
    hdr->length = data_len;
    hdr->reserved = 0;
    hdr->flags = want_resp ? IWL_CMD_FLAG_RESP : 0;

    /* Copy payload after header */
    if (data && data_len > 0) {
        iwl_copy(cmd_buf + IWL_CMD_HDR_SIZE, data, data_len);
    }

    uint16_t total_len = (uint16_t)(IWL_CMD_HDR_SIZE + data_len);

    /* Flush command buffer from CPU cache */
    iwl_cache_flush(cmd_buf, total_len);

    /* Build TFD entry */
    uint16_t tfd_idx = g_iwl_dma.tfd_write_idx;
    struct iwl_tfd *tfd = &g_iwl_dma.tfd_ring[tfd_idx];
    iwl_zero(tfd, sizeof(*tfd));
    tfd->tbs[0].lo = (uint32_t)(cmd_phys & 0xFFFFFFFFu);
    tfd->tbs[0].hi = (uint16_t)((cmd_phys >> 32) & 0xFFFFu);
    tfd->tbs[0].len = total_len;
    tfd->num_tbs = 1;

    /* Update byte-count table */
    g_iwl_dma.bc_table[tfd_idx].bc = total_len;

    /* Flush TFD and BC entries */
    iwl_cache_flush(tfd, sizeof(*tfd));
    iwl_cache_flush(&g_iwl_dma.bc_table[tfd_idx], sizeof(struct iwl_bc_entry));

    /* Advance write index */
    g_iwl_dma.tfd_write_idx = (uint16_t)((tfd_idx + 1) % IWL_TFD_QUEUE_SIZE);

    /* Memory fence before doorbell */
    __asm__ volatile("mfence" : : : "memory");

    /* Ring the TX doorbell: (queue_id << 16) | write_index */
    iwl_write32(HBUS_TARG_WRPTR, (uint32_t)(g_iwl_dma.tfd_write_idx) |
                                 ((uint32_t)IWL_CMD_QUEUE << 16));

    iwl_cmd_trace_add(IWL_CMD_TRACE_SEND,
                      want_resp ? IWL_CMD_TRACE_F_WANT_RESP : 0,
                      0,
                      cmd_id, group_id, hdr->sequence,
                      data_len, 0,
                      0, 0, 0, 0, 0, 0, 0,
                      iwl_read32(CSR_INT));

    return (int)hdr->sequence;
}

/*
 * Wait for a response from firmware matching a given sequence number.
 * Polls the RX queue for up to timeout_ms milliseconds.
 *
 * If resp_buf is non-NULL, copies up to resp_max bytes of the RX buffer
 * (including the response header) into resp_buf.
 *
 * Returns 0 on success, -1 on timeout.
 */
static int iwl_wait_resp_ex(uint8_t expect_cmd_id, uint8_t expect_group_id,
                            uint16_t seq, uint32_t timeout_ms,
                            void *resp_buf, uint32_t resp_max) {
    uint32_t timeout_us = timeout_ms * 1000u;
    uint32_t elapsed = 0;
    uint32_t poll_interval = 500u; /* 0.5 ms */
    uint16_t async_seen = 0;
    uint16_t last_closed_rb = 0;

    while (elapsed < timeout_us) {
        iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
        uint16_t closed_rb = g_iwl_dma.rx_status->closed_rb_num & 0x0FFF;

        if (closed_rb >= IWL_RX_QUEUE_SIZE)
            closed_rb = 0;
        last_closed_rb = closed_rb;

        while (g_iwl_dma.rx_read_idx != closed_rb) {
            uint16_t idx = g_iwl_dma.rx_read_idx;

            /* Used-ring completions carry the 12-bit VID, not the ring slot. */
            iwl_cache_flush(&g_iwl_dma.rx_used_ring[idx], sizeof(uint32_t));
            uint32_t used_entry = g_iwl_dma.rx_used_ring[idx];
            int rb_idx = iwl_rx_used_entry_to_idx(used_entry);
            if (rb_idx < 0) {
                iwl_dbg("IWL9260: wait_resp invalid used entry=%08x idx=%u\n",
                        used_entry, idx);
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                continue;
            }
            uint16_t rb_id = (uint16_t)rb_idx;

            iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], 64);

            const struct iwl_rx_packet *pkt =
                (const struct iwl_rx_packet *)g_iwl_dma.rx_buf_virt[rb_id];
            uint32_t raw_len = iwl_rx_packet_total_len(pkt);
            uint16_t rx_seq = pkt->sequence;
            uint32_t payload_len = iwl_rx_packet_payload_len(pkt);

            if (raw_len >= sizeof(struct iwl_rx_packet) && raw_len <= IWL_RX_BUF_SIZE)
                iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], raw_len);
            else
                raw_len = 64;

            if (seq == 0xFFFFu || rx_seq == seq) {
                if (resp_buf && resp_max > 0) {
                    uint32_t copy_len = resp_max < raw_len ? resp_max : raw_len;
                    iwl_copy(resp_buf, pkt, copy_len);
                }
                iwl_cmd_trace_add(IWL_CMD_TRACE_RESP,
                                  IWL_CMD_TRACE_F_WANT_RESP,
                                  0,
                                  expect_cmd_id, expect_group_id, seq,
                                  0, timeout_ms,
                                  pkt->cmd_id, pkt->group_id, rx_seq,
                                  (uint16_t)payload_len, idx, closed_rb, async_seen,
                                  iwl_read32(CSR_INT));
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                return 0; /* matched packet */
            }

            async_seen++;
            iwl_cmd_trace_add(IWL_CMD_TRACE_ASYNC,
                              IWL_CMD_TRACE_F_WANT_RESP,
                              0,
                              expect_cmd_id, expect_group_id, seq,
                              0, timeout_ms,
                              pkt->cmd_id, pkt->group_id, rx_seq,
                              (uint16_t)payload_len, idx, closed_rb, async_seen,
                              iwl_read32(CSR_INT));
            g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
        }

        iwl_udelay(poll_interval);
        elapsed += poll_interval;
    }

    iwl_cmd_trace_add(IWL_CMD_TRACE_TIMEOUT,
                      IWL_CMD_TRACE_F_WANT_RESP,
                      -1,
                      expect_cmd_id, expect_group_id, seq,
                      0, timeout_ms,
                      0, 0, 0, 0, 0, last_closed_rb, async_seen,
                      iwl_read32(CSR_INT));
    return -1; /* timeout */
}

int iwl_wait_resp(uint16_t seq, uint32_t timeout_ms,
                   void *resp_buf, uint32_t resp_max) {
    return iwl_wait_resp_ex(0xFFu, 0xFFu, seq, timeout_ms, resp_buf, resp_max);
}

/*
 * Send a host command and wait for response.
 * Convenience wrapper around iwl_send_cmd + iwl_wait_resp.
 */
int iwl_send_cmd_sync(uint8_t cmd_id, uint8_t group_id,
                       const void *data, uint16_t data_len,
                       void *resp_buf, uint32_t resp_max,
                       uint32_t timeout_ms) {
    int seq = iwl_send_cmd(cmd_id, group_id, data, data_len, resp_buf ? 1 : 0);
    if (seq < 0) {
        iwl_cmd_trace_add(IWL_CMD_TRACE_SEND_FAIL,
                          resp_buf ? IWL_CMD_TRACE_F_WANT_RESP : 0,
                          seq,
                          cmd_id, group_id, 0,
                          data_len, timeout_ms,
                          0, 0, 0, 0, 0, 0, 0,
                          iwl_read32(CSR_INT));
        kprint("IWL9260: send_cmd failed cmd=0x%02x rc=%d\n", cmd_id, seq);
        return seq;
    }

    if (!resp_buf) {
        /* Fire-and-forget: no response expected, small delay */
        iwl_udelay(1000);
        iwl_cmd_trace_add(IWL_CMD_TRACE_FIRE, 0, 0,
                          cmd_id, group_id, (uint16_t)seq,
                          data_len, timeout_ms,
                          0, 0, 0, 0, 0, 0, 0,
                          iwl_read32(CSR_INT));
        return 0;
    }

    int rc = iwl_wait_resp_ex(cmd_id, group_id, (uint16_t)seq,
                              timeout_ms, resp_buf, resp_max);
    if (rc != 0) {
        kprint("IWL9260: cmd 0x%02x seq=%u response timeout\n", cmd_id, (uint16_t)seq);
    }
    return rc;
}

static void iwl_phy_db_free_entries(struct iwl_phy_db_entry *entries, uint16_t count) {
    if (!entries)
        return;

    for (uint16_t i = 0; i < count; i++) {
        if (entries[i].data)
            kfree(entries[i].data);
    }
    kfree(entries);
}

static void iwl_phy_db_reset(void) {
    if (g_iwl_phy_db.cfg.data)
        kfree(g_iwl_phy_db.cfg.data);
    if (g_iwl_phy_db.calib_nch.data)
        kfree(g_iwl_phy_db.calib_nch.data);

    iwl_phy_db_free_entries(g_iwl_phy_db.calib_ch_group_papd,
                            g_iwl_phy_db.n_group_papd);
    iwl_phy_db_free_entries(g_iwl_phy_db.calib_ch_group_txp,
                            g_iwl_phy_db.n_group_txp);

    iwl_zero(&g_iwl_phy_db, sizeof(g_iwl_phy_db));
}

static int iwl_phy_db_ensure_groups(struct iwl_phy_db_entry **entries,
                                    uint16_t *count,
                                    uint16_t needed) {
    if (needed <= *count)
        return 0;

    struct iwl_phy_db_entry *new_entries =
        (struct iwl_phy_db_entry *)kmalloc((uint32_t)needed * sizeof(**entries));
    if (!new_entries)
        return -1;

    iwl_zero(new_entries, (uint32_t)needed * sizeof(*new_entries));
    if (*entries && *count > 0) {
        iwl_copy(new_entries, *entries, (uint32_t)(*count) * sizeof(*new_entries));
        kfree(*entries);
    }

    *entries = new_entries;
    *count = needed;
    return 0;
}

static struct iwl_phy_db_entry *iwl_phy_db_get_section(enum iwl_phy_db_section_type type,
                                                       uint16_t chg_id) {
    switch (type) {
        case IWL_PHY_DB_CFG:
            return &g_iwl_phy_db.cfg;
        case IWL_PHY_DB_CALIB_NCH:
            return &g_iwl_phy_db.calib_nch;
        case IWL_PHY_DB_CALIB_CHG_PAPD:
            if (chg_id >= g_iwl_phy_db.n_group_papd)
                return 0;
            return &g_iwl_phy_db.calib_ch_group_papd[chg_id];
        case IWL_PHY_DB_CALIB_CHG_TXP:
            if (chg_id >= g_iwl_phy_db.n_group_txp)
                return 0;
            return &g_iwl_phy_db.calib_ch_group_txp[chg_id];
        default:
            return 0;
    }
}

static int iwl_phy_db_store_notif(const struct iwl_rx_packet *pkt) {
    uint32_t pkt_len = iwl_rx_packet_payload_len(pkt);
    const struct iwl_calib_res_notif_phy_db *notif =
        (const struct iwl_calib_res_notif_phy_db *)pkt->data;
    if (pkt_len < sizeof(*notif))
        return -1;

    uint16_t type = notif->type;
    uint16_t size = notif->length;
    if (pkt_len < sizeof(*notif) + size)
        return -2;

    uint16_t chg_id = 0;
    if (type == IWL_PHY_DB_CALIB_CHG_PAPD || type == IWL_PHY_DB_CALIB_CHG_TXP) {
        if (size < sizeof(uint16_t))
            return -3;
        chg_id = (uint16_t)(notif->data[0] | ((uint16_t)notif->data[1] << 8));
        if (type == IWL_PHY_DB_CALIB_CHG_PAPD) {
            if (iwl_phy_db_ensure_groups(&g_iwl_phy_db.calib_ch_group_papd,
                                         &g_iwl_phy_db.n_group_papd,
                                         (uint16_t)(chg_id + 1u)) != 0)
                return -4;
        } else {
            if (iwl_phy_db_ensure_groups(&g_iwl_phy_db.calib_ch_group_txp,
                                         &g_iwl_phy_db.n_group_txp,
                                         (uint16_t)(chg_id + 1u)) != 0)
                return -5;
        }
    }

    struct iwl_phy_db_entry *entry =
        iwl_phy_db_get_section((enum iwl_phy_db_section_type)type, chg_id);
    if (!entry)
        return -6;

    if (entry->data) {
        kfree(entry->data);
        entry->data = 0;
    }
    entry->size = 0;

    if (size > 0) {
        entry->data = (uint8_t *)kmalloc(size);
        if (!entry->data)
            return -7;
        iwl_copy(entry->data, notif->data, size);
        entry->size = size;
    }

    iwl_dbg("IWL9260: PHY DB stored type=%u chg=%u size=%u\n", type, chg_id, size);
    return 0;
}

static int iwl_send_phy_db_section(uint16_t type, const struct iwl_phy_db_entry *entry) {
    if (!entry || !entry->data || entry->size == 0)
        return -1;

    uint16_t cmd_len = (uint16_t)(sizeof(struct iwl_phy_db_cmd) + entry->size);
    uint8_t *cmd = (uint8_t *)kmalloc(cmd_len);
    if (!cmd)
        return -2;

    struct iwl_phy_db_cmd *phy_db_cmd = (struct iwl_phy_db_cmd *)cmd;
    phy_db_cmd->type = type;
    phy_db_cmd->length = entry->size;
    iwl_copy(phy_db_cmd->data, entry->data, entry->size);

    int rc = iwl_send_cmd_sync(IWL_PHY_DB_CMD, 0, cmd, cmd_len, 0, 0, 0);
    kfree(cmd);
    if (rc != 0)
        return rc;

    iwl_dbg("IWL9260: PHY DB sent type=%u size=%u\n", type, entry->size);
    return 0;
}

static int iwl_send_phy_db_data(void) {
    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_PHY_DB_ENTER, 0, 0);

    int rc = iwl_send_phy_db_section(IWL_PHY_DB_CFG, &g_iwl_phy_db.cfg);
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_PHY_DB_FAIL, rc, IWL_PHY_DB_CFG);
        iwl_dbg("IWL9260: PHY DB CFG missing/send failed rc=%d\n", rc);
        return rc;
    }

    rc = iwl_send_phy_db_section(IWL_PHY_DB_CALIB_NCH, &g_iwl_phy_db.calib_nch);
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_PHY_DB_FAIL, rc, IWL_PHY_DB_CALIB_NCH);
        iwl_dbg("IWL9260: PHY DB CALIB_NCH missing/send failed rc=%d\n", rc);
        return rc;
    }

    for (uint16_t i = 0; i < g_iwl_phy_db.n_group_papd; i++) {
        if (!g_iwl_phy_db.calib_ch_group_papd[i].data ||
            g_iwl_phy_db.calib_ch_group_papd[i].size == 0)
            continue;
        rc = iwl_send_phy_db_section(IWL_PHY_DB_CALIB_CHG_PAPD,
                                     &g_iwl_phy_db.calib_ch_group_papd[i]);
        if (rc != 0) {
            iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_PHY_DB_FAIL, rc,
                                     IWL_PHY_DB_CALIB_CHG_PAPD);
            return rc;
        }
    }

    for (uint16_t i = 0; i < g_iwl_phy_db.n_group_txp; i++) {
        if (!g_iwl_phy_db.calib_ch_group_txp[i].data ||
            g_iwl_phy_db.calib_ch_group_txp[i].size == 0)
            continue;
        rc = iwl_send_phy_db_section(IWL_PHY_DB_CALIB_CHG_TXP,
                                     &g_iwl_phy_db.calib_ch_group_txp[i]);
        if (rc != 0) {
            iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_PHY_DB_FAIL, rc,
                                     IWL_PHY_DB_CALIB_CHG_TXP);
            return rc;
        }
    }

    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_PHY_DB_OK, 0, 0);
    iwl_dbg("IWL9260: PHY DB send complete (papd=%u txp=%u)\n",
            g_iwl_phy_db.n_group_papd, g_iwl_phy_db.n_group_txp);
    return 0;
}

/*
 * Read NVM section. Returns bytes read, or negative on error.
 * Data is written to out_data (up to max_len bytes).
 */
static int iwl_nvm_read_section(uint16_t section_type, uint16_t offset,
                                 uint8_t *out_data, uint16_t max_len) {
    struct iwl_nvm_access_cmd nvm_cmd;
    iwl_zero(&nvm_cmd, sizeof(nvm_cmd));
    nvm_cmd.op_code = 0;  /* read */
    nvm_cmd.target = 0;   /* NVM */
    nvm_cmd.type = section_type;
    nvm_cmd.offset = offset;
    nvm_cmd.length = max_len;

    uint8_t resp[512];
    iwl_zero(resp, sizeof(resp));

    int rc = iwl_send_cmd_sync(IWL_NVM_ACCESS_CMD, 0,
                                &nvm_cmd, sizeof(nvm_cmd),
                                resp, sizeof(resp), 2000);
    if (rc != 0) {
        kprint("IWL9260: NVM read section=%u failed rc=%d\n", section_type, rc);
        return -1;
    }

    const struct iwl_rx_packet *pkt = (const struct iwl_rx_packet *)resp;
    const uint8_t *payload = pkt->data;
    uint32_t payload_len = iwl_rx_packet_payload_len(pkt);
    if (payload_len < 8u) {
        kprint("IWL9260: NVM read section=%u short payload=%u\n",
               section_type, (unsigned)payload_len);
        return -3;
    }

    uint16_t resp_length = (uint16_t)(payload[2] | ((uint16_t)payload[3] << 8));
    uint16_t resp_status = (uint16_t)(payload[6] | ((uint16_t)payload[7] << 8));

    if (resp_status != 0) {
        kprint("IWL9260: NVM read section=%u status=%u (error)\n",
               section_type, resp_status);
        return -2;
    }

    if ((uint32_t)resp_length + 8u > payload_len)
        resp_length = payload_len > 8u ? (uint16_t)(payload_len - 8u) : 0u;
    if (resp_length > max_len) resp_length = max_len;
    if (resp_length > 0 && out_data) {
        iwl_copy(out_data, payload + 8, resp_length);
    }

    return (int)resp_length;
}

/*
 * Read MAC address from NVM.
 * Tries MAC_OVERRIDE section first, falls back to HW section.
 */
static int iwl_read_mac_address(uint8_t mac_out[6]) {
    uint8_t nvm_data[256];

    /* Try MAC_OVERRIDE section (section 11) at offset 0 */
    int len = iwl_nvm_read_section(NVM_SECTION_TYPE_MAC_OVERRIDE, 0,
                                    nvm_data, 32);
    if (len >= 6) {
        /* MAC override: first 6 bytes are the MAC address (little-endian pairs) */
        mac_out[0] = nvm_data[1]; mac_out[1] = nvm_data[0];
        mac_out[2] = nvm_data[3]; mac_out[3] = nvm_data[2];
        mac_out[4] = nvm_data[5]; mac_out[5] = nvm_data[4];
        return 0;
    }

    /* Fallback: HW section (section 0) — MAC at offset 0x15 (21) for 9000 */
    len = iwl_nvm_read_section(NVM_SECTION_TYPE_HW, 0, nvm_data, 64);
    if (len >= 27) {  /* need at least offset 21 + 6 bytes */
        mac_out[0] = nvm_data[22]; mac_out[1] = nvm_data[21];
        mac_out[2] = nvm_data[24]; mac_out[3] = nvm_data[23];
        mac_out[4] = nvm_data[26]; mac_out[5] = nvm_data[25];
        return 0;
    }

    kprint("IWL9260: MAC address read failed\n");
    return -1;
}

static uint32_t iwl_fw_valid_tx_ant(void) {
    uint32_t valid_tx_ant = (g_iwl_fw_phy_cfg & FW_PHY_CFG_TX_CHAIN) >>
                            FW_PHY_CFG_TX_CHAIN_POS;
    return valid_tx_ant ? valid_tx_ant : IWL_DEFAULT_CHAIN_MASK;
}

static uint32_t iwl_fw_valid_rx_ant(void) {
    uint32_t valid_rx_ant = (g_iwl_fw_phy_cfg & FW_PHY_CFG_RX_CHAIN) >>
                            FW_PHY_CFG_RX_CHAIN_POS;
    return valid_rx_ant ? valid_rx_ant : iwl_fw_valid_tx_ant();
}

static void iwl_read_nvm_diagnostics(const char *phase, uint8_t mac_out[6]) {
    uint8_t mac[6] = {0};
    int rc = iwl_read_mac_address(mac);
    if (rc == 0) {
        kprint("IWL9260: %s MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               phase, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (mac_out)
            iwl_copy(mac_out, mac, 6);
    } else {
        kprint("IWL9260: %s MAC address read failed\n", phase);
    }

    uint8_t reg_data[32];
    int reg_len = iwl_nvm_read_section(NVM_SECTION_TYPE_REGULATORY, 0,
                                       reg_data, sizeof(reg_data));
    if (reg_len > 0)
        kprint("IWL9260: %s regulatory NVM: %d bytes read\n", phase, reg_len);

    uint8_t phy_data[32];
    int phy_len = iwl_nvm_read_section(NVM_SECTION_TYPE_PHY_SKU, 0,
                                       phy_data, sizeof(phy_data));
    if (phy_len > 0)
        kprint("IWL9260: %s PHY SKU NVM: %d bytes read\n", phase, phy_len);
}

static int iwl_send_tx_ant_cfg(uint32_t valid_tx_ant) {
    struct iwl_tx_ant_cfg_cmd tx_ant = {
        .valid = valid_tx_ant,
    };

    int rc = iwl_send_cmd_sync(IWL_TX_ANT_CONFIGURATION_CMD, 0,
                               &tx_ant, sizeof(tx_ant), 0, 0, 0);
    if (rc != 0) {
        kprint("IWL9260: TX_ANT_CONFIGURATION_CMD failed rc=%d\n", rc);
        return rc;
    }

    kprint("IWL9260: TX_ANT_CONFIGURATION_CMD sent valid=0x%x\n",
           (unsigned)valid_tx_ant);
    return 0;
}

/*
 * Send PHY configuration command.
 */
static int iwl_phy_cfg(void) {
    struct iwl_phy_cfg_cmd phy;
    uint32_t valid_tx_ant = iwl_fw_valid_tx_ant();
    uint32_t valid_rx_ant = iwl_fw_valid_rx_ant();
    iwl_zero(&phy, sizeof(phy));
    /* Use the firmware TLV PHY configuration and preserve sane chain masks. */
    phy.phy_cfg = g_iwl_fw_phy_cfg & ~(FW_PHY_CFG_TX_CHAIN | FW_PHY_CFG_RX_CHAIN);
    phy.phy_cfg |= valid_tx_ant << FW_PHY_CFG_TX_CHAIN_POS;
    phy.phy_cfg |= valid_rx_ant << FW_PHY_CFG_RX_CHAIN_POS;
    phy.calib_control_tx = 0xFFFFFFFFu; /* enable all TX calibrations */
    phy.calib_control_rx = 0xFFFFFFFFu; /* enable all RX calibrations */

    int rc = iwl_send_cmd_sync(IWL_PHY_CFG_CMD, 0,
                                &phy, sizeof(phy), 0, 0, 0);
    if (rc != 0) {
        kprint("IWL9260: PHY_CFG_CMD failed rc=%d\n", rc);
        return rc;
    }

    kprint("IWL9260: PHY_CFG_CMD sent phy=0x%08x tx=0x%x rx=0x%x\n",
           phy.phy_cfg, (unsigned)valid_tx_ant, (unsigned)valid_rx_ant);
    return 0;
}

static int iwl_prepare_init_phase(uint8_t mac_out[6]) {
    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_PREP_ENTER, 0, 0);
    kprint("IWL9260: preparing INIT-phase NVM/PHY work...\n");

    iwl_read_nvm_diagnostics("INIT", mac_out);

    int rc = iwl_send_tx_ant_cfg(iwl_fw_valid_tx_ant());
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_PREP_FAIL, rc, 0);
        return rc;
    }

    rc = iwl_phy_cfg();
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_PREP_FAIL, rc, 0);
        return rc;
    }

    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_INIT_PREP_OK, 0, 0);
    return 0;
}

static int iwl_hcmd_init(uint8_t mac_out[6], int init_phase_prepared) {
    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_HCMD_ENTER, 0, 0);
    kprint("IWL9260: initializing host command interface...\n");

    int rc = iwl_send_tx_ant_cfg(iwl_fw_valid_tx_ant());
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_HCMD_FAIL, rc, 0);
        return rc;
    }

    rc = iwl_phy_cfg();
    if (rc != 0) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_HCMD_FAIL, rc, 0);
        return rc;
    }

    if (!init_phase_prepared) {
        /* Small delay for PHY calibration to settle when INIT firmware is absent. */
        iwl_udelay(50000); /* 50 ms */
        iwl_read_nvm_diagnostics("runtime", mac_out);
    } else if (iwl_mac_is_zero(mac_out)) {
        /* Preserve the new INIT ordering, but still recover a MAC if INIT reads failed. */
        iwl_read_nvm_diagnostics("runtime", mac_out);
    } else {
        kprint("IWL9260: runtime PHY configured after INIT calibration/PHY_DB replay\n");
    }

    kprint("IWL9260: host command interface ready\n");
    iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_HCMD_OK, 0, 0);
    return 0;
}

/* ---- Step 9: Data Path ---- */

/* Global WPA2 security context */
static struct wpa2_ctx g_wpa2;
static int g_wpa2_active;   /* 1 if WPA2 handshake completed */
static uint8_t g_our_mac[6];
static uint8_t g_ap_bssid[6];
static int g_iwl_ready;
static int g_iwl_have_mac;
static int g_iwl_connected;
static uint8_t g_iwl_init_step;  /* WIFI_STEP_* */
static int16_t g_iwl_init_rc;    /* rc from failed step */
static uint32_t g_iwl_sec_rt_count;  /* number of SEC_RT sections parsed */
static uint8_t g_iwl_link_secure;
static uint8_t g_iwl_link_channel;
static int8_t g_iwl_link_rssi;
static char g_iwl_ssid[IWL_MAX_SSID_LEN + 1];

static void iwl_trace_capture(const char *tag) {
    if (g_iwl_boot_trace_count >= IWL_BOOT_TRACE_MAX)
        return;

    struct iwl_boot_trace_entry *e = &g_iwl_boot_trace[g_iwl_boot_trace_count++];
    iwl_zero(e, sizeof(*e));
    e->tag = tag;
    e->step = g_iwl_init_step;
    e->rc = g_iwl_init_rc;
    e->irq_count = g_iwl_irq_fired;

    if (!g_iwl_mmio)
        return;

    e->int_mask = iwl_read32(CSR_INT_MASK);
    e->csr_int = iwl_read32(CSR_INT);
    e->fh_int = iwl_read32(CSR_FH_INT_STATUS);
    e->gp_cntrl = iwl_read32(CSR_GP_CNTRL);
    e->csr_reset = iwl_read32(CSR_RESET);
    e->fh_load = iwl_read32(FH_UCODE_LOAD_STATUS);

    if (iwl_grab_nic_access() == 0) {
        e->wfpm_gp2 = iwl_read_prph(WFPM_GP2);
        e->cpu_reset = iwl_read_prph(RELEASE_CPU_RESET);
        e->rfh_act = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        e->rfh_w = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        e->rfh_r = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        e->rfh_u = iwl_read_prph(RFH_Q0_URBDCB_WIDX);
        e->cpu1 = iwl_read_prph(SB_CPU_1_STATUS);
        e->cpu2 = iwl_read_prph(SB_CPU_2_STATUS);
    }
}

/* Network RX callback — set by netcore */
typedef void (*wifi_rx_callback_t)(const uint8_t *data, uint16_t len);
static wifi_rx_callback_t g_wifi_rx_cb;

void wifi_set_rx_callback(wifi_rx_callback_t cb) {
    g_wifi_rx_cb = cb;
}

static void iwl_copy_ssid(char dst[IWL_MAX_SSID_LEN + 1],
                          const uint8_t *src, uint32_t len) {
    if (!dst) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    if (len > IWL_MAX_SSID_LEN) len = IWL_MAX_SSID_LEN;
    for (uint32_t i = 0; i < len; i++) dst[i] = (char)src[i];
    dst[len] = 0;
}

static void iwl_clear_link_state(void) {
    g_iwl_connected = 0;
    g_iwl_link_secure = 0;
    g_iwl_link_channel = 0;
    g_iwl_link_rssi = 0;
    g_wpa2_active = 0;
    iwl_zero(g_ap_bssid, sizeof(g_ap_bssid));
    iwl_zero(g_iwl_ssid, sizeof(g_iwl_ssid));
}

static void iwl_set_link_state(const struct iwl_bss_entry *bss) {
    if (!bss) return;
    g_iwl_connected = 1;
    g_iwl_link_secure = bss->has_rsn ? 1 : 0;
    g_iwl_link_channel = bss->channel;
    g_iwl_link_rssi = bss->rssi;
    iwl_copy(g_ap_bssid, bss->bssid, 6);
    iwl_copy_ssid(g_iwl_ssid, bss->ssid, bss->ssid_len);
}

/* ADD_STA command — tells firmware about the AP station for data traffic */
struct iwl_add_sta_cmd {
    uint8_t  addr[6];        /* station MAC address */
    uint16_t reserved1;
    uint32_t sta_id;         /* station ID (0 for AP) */
    uint32_t modify_mask;    /* which fields to modify */
    uint32_t station_flags;  /* station flags */
    uint32_t station_flags_msk;
    uint8_t  add_modify;     /* 0=add, 1=modify */
    uint8_t  rx_bandwidth;
    uint8_t  sp_length;
    uint8_t  uapsd_acs;
    uint16_t assoc_id;       /* association ID */
    uint16_t beamform_flags;
    uint32_t tfd_queue_msk;  /* bitmask of TX queues this STA can use */
    uint8_t  mac_id_n_color[4];
    uint8_t  reserved2[12];
} __attribute__((packed));

static int iwl_add_sta(const uint8_t bssid[6]) {
    struct iwl_add_sta_cmd cmd;
    iwl_zero(&cmd, sizeof(cmd));

    iwl_copy(cmd.addr, bssid, 6);
    cmd.sta_id = 0;          /* station ID 0 = AP */
    cmd.add_modify = 0;      /* add */
    cmd.tfd_queue_msk = 1;   /* queue 0 */

    int rc = iwl_send_cmd_sync(IWL_ADD_STA_CMD, 0,
                                &cmd, sizeof(cmd), 0, 0, 0);
    if (rc != 0) {
        kprint("IWL9260: ADD_STA_CMD failed rc=%d\n", rc);
        return rc;
    }

    kprint("IWL9260: STA added for AP %02x:%02x:%02x:%02x:%02x:%02x\n",
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return 0;
}

/*
 * Process a single RX buffer from the firmware.
 * Classifies the frame and dispatches to the appropriate handler.
 *
 * Returns: 0 = handled, 1 = command response (caller should check), -1 = unknown
 */
static int iwl_rx_process_one(const uint8_t *rxdata, uint16_t rxlen) {
    if (rxlen < sizeof(struct iwl_rx_packet)) return -1;

    const struct iwl_rx_packet *pkt = (const struct iwl_rx_packet *)rxdata;
    const uint8_t *payload = pkt->data;
    uint32_t payload_len = iwl_rx_packet_payload_len(pkt);
    uint8_t group = pkt->group_id;
    uint8_t cmd = pkt->cmd_id;

    /* ALIVE notification */
    if (group == IWL_LEGACY_GROUP && cmd == IWL_ALIVE_CMD_ID) return 1;

    /* Scan complete notification */
    if (cmd == IWL_SCAN_COMPLETE_UMAC) return 1;

    /* Try to find an 802.11 frame at the typical MVM MPDU offset (28) */
    if (payload_len < 28 + 24) return 1; /* too short for MPDU + 802.11 header */

    const uint8_t *frame = payload + 28;
    uint16_t frame_len = (uint16_t)(payload_len - 28);
    uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    uint8_t type = (uint8_t)((fc >> 2) & 0x03);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0x0F);

    /* Management frame */
    if (type == 0) {
        /* Deauth (subtype 12) or Disassoc (subtype 10) */
        if (subtype == 12 || subtype == 10) {
            kprint("IWL9260: received %s from AP\n",
                   subtype == 12 ? "deauth" : "disassoc");
            iwl_clear_link_state();
            net_reset_config();
        }
        return 1; /* let caller handle too (for auth/assoc resp matching) */
    }

    /* Data frame */
    if (type == 2) {
        /* Minimum 802.11 data header = 24 bytes */
        if (frame_len < 24 + 8) return 0; /* too short */

        /* Determine header length */
        uint16_t hdr_len = 24;
        if ((fc & 0x0300) == 0x0300) hdr_len = 30; /* 4-address */
        if (fc & 0x0080) hdr_len += 2; /* QoS */

        /* Check for Protected Frame bit */
        int is_encrypted = (frame[1] & 0x40) != 0;

        uint8_t decrypted_buf[2048];
        const uint8_t *payload;
        uint16_t payload_len;

        if (is_encrypted && g_wpa2_active) {
            /* CCMP encrypted: hdr + 8 (CCMP hdr) + data + 8 (MIC) */
            if (frame_len < hdr_len + 16) return 0;

            int pt_len = wpa2_decrypt_data(&g_wpa2, frame, frame_len,
                                           decrypted_buf, sizeof(decrypted_buf));
            if (pt_len < 0) {
                kprint("IWL9260: CCMP decrypt failed\n");
                return 0;
            }

            payload = decrypted_buf;
            payload_len = (uint16_t)pt_len;
        } else {
            /* Unencrypted data (or no WPA2 context) */
            payload = frame + hdr_len;
            payload_len = frame_len - hdr_len;
        }

        /* Check LLC/SNAP header */
        if (payload_len < 8) return 0;
        if (payload[0] != 0xAA || payload[1] != 0xAA || payload[2] != 0x03)
            return 0; /* not LLC/SNAP */

        uint16_t ethertype = (uint16_t)((uint16_t)payload[6] << 8 | payload[7]);

        /* EAPOL frame (0x888E) → WPA2 handshake */
        if (ethertype == 0x888E) {
            const uint8_t *eapol_body = payload + 8;
            uint16_t eapol_len = payload_len - 8;

            kprint("IWL9260: received EAPOL frame (%u bytes)\n", eapol_len);

            int rc = wpa2_process_eapol(&g_wpa2, eapol_body, eapol_len);
            if (rc != 0) {
                kprint("IWL9260: EAPOL processing failed rc=%d\n", rc);
                return 0;
            }

            /* Check if we need to send a response */
            if (g_wpa2.hs_state == 2) {
                /* Just processed message 1 → send message 2 */
                uint8_t msg2[256];
                int msg2_len = wpa2_build_msg2(&g_wpa2, msg2, sizeof(msg2));
                if (msg2_len > 0) {
                    iwl_tx_eapol(g_ap_bssid, g_our_mac, g_ap_bssid,
                                  msg2, (uint16_t)msg2_len);
                    kprint("IWL9260: sent EAPOL message 2\n");
                }
            } else if (g_wpa2.hs_state == 3) {
                /* Just processed message 3 → send message 4 */
                uint8_t msg4[256];
                int msg4_len = wpa2_build_msg4(&g_wpa2, msg4, sizeof(msg4));
                if (msg4_len > 0) {
                    iwl_tx_eapol(g_ap_bssid, g_our_mac, g_ap_bssid,
                                  msg4, (uint16_t)msg4_len);
                    kprint("IWL9260: sent EAPOL message 4\n");
                    g_wpa2.hs_state = 4; /* handshake complete */
                    g_wpa2_active = 1;
                    kprint("IWL9260: *** WPA2 HANDSHAKE COMPLETE — CCMP ACTIVE ***\n");
                }
            }

            return 0;
        }

        /* Regular data frame (IP/ARP/etc.) — deliver to network stack */
        if (g_wifi_rx_cb && payload_len > 8) {
            /* Pass the Ethernet payload (after LLC/SNAP) to the callback.
             * We build a pseudo-Ethernet header: DA(6) + SA(6) + EtherType(2) + data.
             * DA = frame address 1 or 3 depending on ToDS/FromDS.
             * SA = frame address 2 or 3.
             * For FromDS=1, ToDS=0 (AP → STA): DA=A1, SA=A3. */
            uint8_t eth_frame[2048];
            uint16_t eth_len = 0;

            /* Extract DA and SA from 802.11 header based on ToDS/FromDS */
            uint8_t to_ds = (fc & 0x0100) ? 1 : 0;
            uint8_t from_ds = (fc & 0x0200) ? 1 : 0;

            if (!to_ds && from_ds) {
                /* FromDS: DA=A1, SA=A3 */
                iwl_copy(eth_frame, frame + 4, 6);      /* DA = A1 */
                iwl_copy(eth_frame + 6, frame + 16, 6);  /* SA = A3 */
            } else if (to_ds && !from_ds) {
                /* ToDS: DA=A3, SA=A2 */
                iwl_copy(eth_frame, frame + 16, 6);      /* DA = A3 */
                iwl_copy(eth_frame + 6, frame + 10, 6);  /* SA = A2 */
            } else {
                /* IBSS or WDS — use A1/A2 */
                iwl_copy(eth_frame, frame + 4, 6);
                iwl_copy(eth_frame + 6, frame + 10, 6);
            }

            /* EtherType + payload data (skip LLC/SNAP) */
            eth_frame[12] = payload[6];
            eth_frame[13] = payload[7];
            eth_len = 14;
            uint16_t data_len = payload_len - 8;
            if (data_len > sizeof(eth_frame) - 14) data_len = sizeof(eth_frame) - 14;
            iwl_copy(eth_frame + 14, payload + 8, data_len);
            eth_len += data_len;

            g_wifi_rx_cb(eth_frame, eth_len);
        }

        return 0;
    }

    return 1; /* unhandled frame type */
}

/*
 * Poll the RX queue and process all pending frames.
 * This is the main RX processing loop, called periodically or from the IRQ handler.
 *
 * timeout_ms: how long to poll (0 = check once and return)
 * Returns number of frames processed.
 */
int iwl_rx_poll(uint32_t timeout_ms) {
    if (!g_iwl_dma.allocated) return 0;

    uint32_t timeout_us = timeout_ms * 1000u;
    uint32_t elapsed = 0;
    int processed = 0;

    do {
        iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
        uint16_t closed_rb = g_iwl_dma.rx_status->closed_rb_num & 0x0FFF;

        if (closed_rb >= IWL_RX_QUEUE_SIZE)
            closed_rb = 0;

        while (g_iwl_dma.rx_read_idx != closed_rb) {
            uint16_t idx = g_iwl_dma.rx_read_idx;

            /* Used-ring completions carry the 12-bit VID, not the ring slot. */
            iwl_cache_flush(&g_iwl_dma.rx_used_ring[idx], sizeof(uint32_t));
            uint32_t used_entry = g_iwl_dma.rx_used_ring[idx];
            int rb_idx = iwl_rx_used_entry_to_idx(used_entry);
            if (rb_idx < 0) {
                iwl_dbg("IWL9260: RX poll invalid used entry=%08x idx=%u\n",
                        used_entry, idx);
                g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
                continue;
            }
            uint16_t rb_id = (uint16_t)rb_idx;

            iwl_cache_flush(g_iwl_dma.rx_buf_virt[rb_id], 4096);

            iwl_rx_process_one(g_iwl_dma.rx_buf_virt[rb_id], 4096);
            processed++;

            g_iwl_dma.rx_read_idx = iwl_rx_next_rb(idx);
        }

        if (timeout_us == 0) break;
        iwl_udelay(1000); /* 1 ms */
        elapsed += 1000;
    } while (elapsed < timeout_us);

    return processed;
}

/*
 * Send a data packet (Ethernet-like: DA + SA + EtherType + payload).
 * Wraps in 802.11 data header, encrypts with CCMP if WPA2 is active.
 *
 * eth_frame: DA(6) + SA(6) + EtherType(2) + data
 * eth_len: total Ethernet frame length
 *
 * Returns 0 on success, -1 on error.
 */
int wifi_tx_packet(const uint8_t *eth_frame, uint16_t eth_len) {
    if (eth_len < 14) return -1;
    if (!g_iwl_dma.allocated) return -1;

    const uint8_t *da = eth_frame;
    const uint8_t *sa = eth_frame + 6;
    uint16_t ethertype_hi = eth_frame[12];
    uint16_t ethertype_lo = eth_frame[13];
    const uint8_t *payload = eth_frame + 14;
    uint16_t payload_len = eth_len - 14;

    /* Build 802.11 data header (ToDS=1, FromDS=0: STA → AP) */
    uint8_t frame[2300];
    iwl_zero(frame, sizeof(frame));

    /* Frame Control: Data + ToDS */
    uint16_t fc = 0x0108; /* type=data(0x08) + ToDS(0x01 in byte 1) */
    frame[0] = (uint8_t)(fc);
    frame[1] = (uint8_t)(fc >> 8);

    /* A1 = BSSID (AP), A2 = SA (us), A3 = DA (destination) */
    iwl_copy(frame + 4, g_ap_bssid, 6);
    iwl_copy(frame + 10, sa, 6);
    iwl_copy(frame + 16, da, 6);

    /* Sequence control */
    static uint16_t data_seq;
    frame[22] = (uint8_t)((data_seq & 0xFFF) << 4);
    frame[23] = (uint8_t)(((data_seq & 0xFFF) << 4) >> 8);
    data_seq++;

    uint16_t hdr_len = 24;

    /* Build LLC/SNAP + payload */
    uint8_t llc_payload[2048];
    llc_payload[0] = 0xAA; /* DSAP */
    llc_payload[1] = 0xAA; /* SSAP */
    llc_payload[2] = 0x03; /* Control */
    llc_payload[3] = 0x00; /* OUI */
    llc_payload[4] = 0x00;
    llc_payload[5] = 0x00;
    llc_payload[6] = (uint8_t)ethertype_hi;
    llc_payload[7] = (uint8_t)ethertype_lo;
    if (payload_len > sizeof(llc_payload) - 8) payload_len = sizeof(llc_payload) - 8;
    iwl_copy(llc_payload + 8, payload, payload_len);
    uint16_t llc_len = 8 + payload_len;

    if (g_wpa2_active) {
        /* Encrypt with CCMP */
        uint8_t encrypted_frame[2300];
        int total = wpa2_encrypt_data(&g_wpa2, frame, hdr_len,
                                       llc_payload, llc_len,
                                       encrypted_frame);
        if (total < 0) {
            kprint("IWL9260: CCMP encrypt failed\n");
            return -1;
        }
        return iwl_tx_mgmt(encrypted_frame, (uint16_t)total) == 0 ? 0 : -1;
    } else {
        /* Unencrypted: copy LLC/SNAP payload after header */
        iwl_copy(frame + hdr_len, llc_payload, llc_len);
        return iwl_tx_mgmt(frame, hdr_len + llc_len) == 0 ? 0 : -1;
    }
}

/*
 * Complete WPA2 connect flow:
 * 1. Scan for the target SSID
 * 2. Derive PMK from passphrase
 * 3. Associate with RSN IE
 * 4. Add STA entry
 * 5. Wait for EAPOL four-way handshake (polling RX)
 *
 * Returns 0 on success (connected + encrypted).
 */
int iwl_connect_wpa2(const char *ssid, const char *passphrase,
                      const uint8_t mac[6]) {
    iwl_clear_link_state();

    /* Scan */
    struct iwl_bss_entry scan_results[IWL_MAX_SCAN_RESULTS];
    uint32_t scan_count = 0;
    int rc = iwl_scan(scan_results, IWL_MAX_SCAN_RESULTS, &scan_count);
    if (rc != 0) {
        kprint("IWL9260: scan failed rc=%d\n", rc);
        return -1;
    }

    /* Find the target SSID */
    uint32_t ssid_len = 0;
    while (ssid[ssid_len]) ssid_len++;

    const struct iwl_bss_entry *target_bss = 0;
    for (uint32_t i = 0; i < scan_count; i++) {
        if (scan_results[i].ssid_len == ssid_len) {
            int match = 1;
            for (uint32_t j = 0; j < ssid_len; j++) {
                if (scan_results[i].ssid[j] != (uint8_t)ssid[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                target_bss = &scan_results[i];
                break;
            }
        }
    }

    if (!target_bss) {
        kprint("IWL9260: SSID \"%s\" not found in scan results\n", ssid);
        return -2;
    }

    kprint("IWL9260: found \"%s\" ch=%u %s\n", target_bss->ssid,
           target_bss->channel, target_bss->has_rsn ? "[WPA2]" : "[OPEN]");

    /* Initialize WPA2 context */
    iwl_copy(g_our_mac, mac, 6);
    iwl_copy(g_ap_bssid, target_bss->bssid, 6);
    wpa2_init(&g_wpa2, g_our_mac, g_ap_bssid);

    /* Derive PMK from passphrase (slow — ~4096 HMAC iterations) */
    wpa2_set_passphrase(&g_wpa2, passphrase,
                         target_bss->ssid, target_bss->ssid_len);

    /* Associate with WPA2 (includes RSN IE) */
    if (target_bss->has_rsn) {
        rc = iwl_associate_wpa2(target_bss, mac);
    } else {
        rc = iwl_associate(target_bss, mac);
    }
    if (rc != 0) {
        kprint("IWL9260: association failed rc=%d\n", rc);
        return -3;
    }

    /* Add STA for AP in firmware */
    rc = iwl_add_sta(target_bss->bssid);
    if (rc != 0) {
        kprint("IWL9260: ADD_STA failed rc=%d\n", rc);
        /* Non-fatal: some firmware versions don't require explicit ADD_STA */
    }

    if (!target_bss->has_rsn) {
        iwl_set_link_state(target_bss);
        kprint("IWL9260: connected to open network \"%s\"\n", target_bss->ssid);
        return 0;
    }

    /* Wait for EAPOL four-way handshake (up to 10 seconds) */
    kprint("IWL9260: waiting for EAPOL handshake...\n");
    uint32_t hs_timeout_us = 10000000u; /* 10 seconds */
    uint32_t hs_elapsed = 0;

    while (hs_elapsed < hs_timeout_us) {
        iwl_rx_poll(0); /* process any pending RX frames */

        if (g_wpa2_active) {
            iwl_set_link_state(target_bss);
            kprint("IWL9260: *** CONNECTED to \"%s\" (WPA2-CCMP) ***\n",
                   target_bss->ssid);
            return 0;
        }

        iwl_udelay(5000); /* 5 ms */
        hs_elapsed += 5000;
    }

    kprint("IWL9260: EAPOL handshake timeout (state=%u)\n", g_wpa2.hs_state);
    iwl_clear_link_state();
    return -4;
}

int wifi_9260_get_user_status(struct fry_wifi_status *out) {
    if (!out) return -1;

    for (uint32_t i = 0; i < sizeof(*out); i++) {
        ((uint8_t *)out)[i] = 0;
    }

    out->ready = g_iwl_ready ? 1 : 0;
    out->have_mac = g_iwl_have_mac ? 1 : 0;
    out->connected = g_iwl_connected ? 1 : 0;
    out->secure = g_iwl_link_secure;
    out->channel = g_iwl_link_channel;
    out->rssi = g_iwl_link_rssi;
    out->init_step = g_iwl_init_step;
    out->init_rc = g_iwl_init_rc;
    iwl_copy(out->mac, g_our_mac, 6);
    iwl_copy(out->bssid, g_ap_bssid, 6);
    iwl_copy_ssid(out->ssid, (const uint8_t *)g_iwl_ssid, IWL_MAX_SSID_LEN);

    const struct net_config *cfg = net_get_config();
    if (cfg) {
        out->configured = cfg->configured ? 1 : 0;
        out->ip = cfg->ip;
        out->netmask = cfg->netmask;
        out->gateway = cfg->gateway;
        out->dns_server = cfg->dns_server;
        if (!g_iwl_have_mac) iwl_copy(out->mac, cfg->mac, 6);
    }

    return 0;
}

int wifi_9260_get_debug_log(char *buf, uint32_t bufsz) {
    if (!buf || bufsz == 0) return -1;

    uint32_t pos = 0;

    /* macro helpers: append string / hex32 / decimal / newline */
#define DBGSTR(s) do { \
    const char *_s = (s); \
    while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; \
} while(0)
#define DBGHEX(val) do { \
    for (int _s = 28; _s >= 0 && pos < bufsz - 1; _s -= 4) \
        buf[pos++] = "0123456789abcdef"[((val) >> _s) & 0xF]; \
} while(0)
#define DBGDEC(v) do { \
    uint32_t _v = (v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)
#define DBGNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)
#define DBGREG(label, val) do { DBGSTR(label); DBGHEX(val); DBGNL(); } while(0)

    /* --- Init step + error code summary (always first) --- */
    DBGSTR("--- INIT STATE ---\n");
    DBGSTR("step="); DBGDEC(g_iwl_init_step);
    DBGSTR(" (");
    switch (g_iwl_init_step) {
        case 0:  DBGSTR("none"); break;
        case 1:  DBGSTR("pci"); break;
        case 2:  DBGSTR("nic-reset"); break;
        case 3:  DBGSTR("fw-load"); break;
        case 4:  DBGSTR("tlv-parse"); break;
        case 5:  DBGSTR("dma-alloc"); break;
        case 6:  DBGSTR("tfh-init"); break;
        case 7:  DBGSTR("msi"); break;
        case 8:  DBGSTR("fw-upload"); break;
        case 9:  DBGSTR("alive"); break;
        case 10: DBGSTR("hcmd"); break;
        case 11: DBGSTR("mac-init"); break;
        case 12: DBGSTR("scan"); break;
        case 13: DBGSTR("done"); break;
        default: DBGSTR("?"); break;
    }
    DBGSTR(") rc=");
    if (g_iwl_init_rc < 0) {
        DBGSTR("-"); DBGDEC((uint32_t)(-(int)g_iwl_init_rc));
    } else {
        DBGDEC((uint32_t)g_iwl_init_rc);
    }
    DBGNL();
    DBGSTR("irq_count="); DBGDEC(g_iwl_irq_fired); DBGNL();

    /* --- DMA addresses (verify rings are sane) --- */
    if (g_iwl_dma.allocated) {
        DBGSTR("--- DMA ADDRS ---\n");
        DBGSTR("rxbd=0x");  DBGHEX((uint32_t)g_iwl_dma.rxbd_ring_phys); DBGNL();
        DBGSTR("rx_used=0x"); DBGHEX((uint32_t)g_iwl_dma.rx_used_ring_phys); DBGNL();
        DBGSTR("rx_stat=0x"); DBGHEX((uint32_t)g_iwl_dma.rx_status_phys); DBGNL();
        DBGSTR("tfd=0x");   DBGHEX((uint32_t)g_iwl_dma.tfd_ring_phys); DBGNL();
        DBGSTR("ctxt_info=0x"); DBGHEX((uint32_t)g_iwl_ctxt_info_phys); DBGNL();
        DBGSTR("tfd_wr="); DBGDEC(g_iwl_dma.tfd_write_idx);
        DBGSTR(" rx_rd="); DBGDEC(g_iwl_dma.rx_read_idx); DBGNL();
        if (g_iwl_ctxt_info) {
            iwl_cache_flush(g_iwl_ctxt_info, 4096u);
            DBGSTR("ctxt_canary0=0x");
            DBGHEX(iwl_read32_mem(g_iwl_ctxt_info, IWL_CTXT_CANARY0_OFF));
            DBGSTR(" ctxt_canary1=0x");
            DBGHEX(iwl_read32_mem(g_iwl_ctxt_info, IWL_CTXT_CANARY1_OFF));
            DBGNL();
        }

        /* RX status writeback (device-written) */
        if (g_iwl_dma.rx_status) {
            iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
            uint16_t cl = g_iwl_dma.rx_status->closed_rb_num;
            uint16_t fi = g_iwl_dma.rx_status->finished_rb_num;
            DBGSTR("rx_closed=0x"); DBGHEX(cl);
            DBGSTR(" rx_finished=0x"); DBGHEX(fi); DBGNL();
        }
    }

    DBGSTR("--- BOOT PROBES ---\n");
    DBGSTR("sec_rt_count="); DBGDEC(g_iwl_sec_rt_count); DBGNL();
    DBGREG("boot_flags=0x", g_iwl_bootdbg.flags);
    DBGREG("rfkill_clr=0x", g_iwl_bootdbg.rfkill_mask);
    DBGREG("shadow_ctrl=0x", g_iwl_bootdbg.shadow_ctrl);
    DBGREG("fw_upload=gen1-DMA", 0);
    DBGREG("wfpm_gp2_wr=0x", g_iwl_bootdbg.wfpm_gp2_written);
    DBGREG("wfpm_gp2_rd=0x", g_iwl_bootdbg.wfpm_gp2_readback);
    DBGREG("cpu_reset_bit=24", (1u << 24));
    DBGREG("cpu_reset_rd=0x", g_iwl_bootdbg.release_cpu_reset_readback);
    DBGREG("chicken_before=0x", g_iwl_bootdbg.chicken_before);
    DBGREG("chicken_after=0x", g_iwl_bootdbg.chicken_after);
    DBGREG("chicken_want=0x", g_iwl_bootdbg.chicken_want);
    DBGREG("upload_fh_st=0x", g_iwl_bootdbg.fh_load_status_after_upload);
    DBGREG("last_irq_csr=0x", g_iwl_last_csr_int);
    DBGREG("last_irq_fh=0x", g_iwl_last_fh_int);

    /* fry664: Live regs/SRAM moved to wd2 to avoid terminal truncation */
    DBGSTR("(use 'wd2' for live regs/SRAM, 'wc' for CPU)\n");

#undef DBGSTR
#undef DBGHEX
#undef DBGDEC
#undef DBGNL
#undef DBGREG

    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

/*
 * wd2 — live registers, CPU status, SRAM verify (split from wd for terminal fit).
 * fry664: moved here from wifi_9260_get_debug_log to avoid truncation.
 */
int wifi_9260_get_debug_log2(char *buf, uint32_t bufsz) {
    if (!buf || bufsz == 0) return -1;
    uint32_t pos = 0;

#define DBGSTR(s) do { \
    const char *_s = (s); \
    while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; \
} while(0)
#define DBGHEX(val) do { \
    for (int _s = 28; _s >= 0 && pos < bufsz - 1; _s -= 4) \
        buf[pos++] = "0123456789abcdef"[((val) >> _s) & 0xF]; \
} while(0)
#define DBGDEC(v) do { \
    uint32_t _v = (v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)
#define DBGNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)
#define DBGREG(label, val) do { DBGSTR(label); DBGHEX(val); DBGNL(); } while(0)

    if (!g_iwl_mmio) {
        DBGSTR("no MMIO mapped\n");
        goto done;
    }

    uint32_t hw_if = iwl_read32(CSR_HW_IF_CONFIG_REG);
    uint32_t gp = iwl_read32(CSR_GP_CNTRL);
    uint32_t csr_int = iwl_read32(CSR_INT);
    uint32_t fw_st = iwl_read32(FH_UCODE_LOAD_STATUS);
    uint32_t hw_rev = iwl_read32(CSR_HW_REV);
    uint32_t csr_rst = iwl_read32(CSR_RESET);
    int nic_access = (iwl_grab_nic_access() == 0);
    uint32_t rfh_cfg = 0, rfh_st = 0, rfh_act = 0;
    uint32_t rfh_widx = 0, rfh_ridx = 0, rfh_uwdx = 0;

    if (nic_access) {
        rfh_cfg = iwl_read_prph(RFH_RXF_DMA_CFG);
        rfh_st  = iwl_read_prph(RFH_GEN_STATUS);
        rfh_act = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        rfh_widx = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        rfh_ridx = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        rfh_uwdx = iwl_read_prph(RFH_Q0_URBDCB_WIDX);
    }

    DBGSTR("--- LIVE REGS ---\n");
    DBGREG("HW_IF=0x", hw_if);
    DBGREG("GP_CNTRL=0x", gp);
    DBGREG("CSR_RST=0x", csr_rst);
    DBGREG("CSR_INT=0x", csr_int);
    DBGREG("FH_LOAD=0x", fw_st);
    DBGREG("HW_REV=0x", hw_rev);
    if (nic_access) {
        DBGREG("RFH_DMA_CFG=0x", rfh_cfg);
        DBGREG("RFH_STATUS=0x", rfh_st);
        DBGREG("RFH_ACT=0x", rfh_act);
        DBGREG("RFH_W=0x", rfh_widx);
        DBGREG("RFH_R=0x", rfh_ridx);
        DBGREG("RFH_U=0x", rfh_uwdx);
        /* Runtime readback is expected to be 0 unless an extended-range DMA
         * chunk is actively being armed. */
        DBGREG("LMPM_NOW=0x", iwl_read_prph(LMPM_CHICK));
    } else {
        DBGSTR("RFH_PRPH=unavailable\n");
    }

    /* CPU status + PC */
    if (nic_access) {
        DBGSTR("--- CPU STATUS ---\n");
        uint32_t cpu1_st = iwl_read_prph(SB_CPU_1_STATUS);
        uint32_t cpu2_st = iwl_read_prph(SB_CPU_2_STATUS);
        uint32_t upc1 = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t lpc1 = iwl_read_prph(UREG_LMAC1_CURRENT_PC);
        DBGREG("CPU1_ST=0x", cpu1_st);
        DBGREG("CPU2_ST=0x", cpu2_st);
        DBGREG("UMAC_PC=0x", upc1);
        DBGREG("LMAC_PC=0x", lpc1);
    }

    /* SRAM verify */
    if (nic_access) {
        DBGSTR("--- SRAM VERIFY ---\n");
        DBGSTR("SRAM[00000000]: ");
        for (uint32_t si = 0; si < 4; si++) {
            DBGHEX(iwl_read_sram(si * 4u));
            if (si < 3) DBGSTR(" ");
        }
        DBGNL();
        DBGSTR("SRAM[00404000]: ");
        for (uint32_t si = 0; si < 4; si++) {
            DBGHEX(iwl_read_sram(0x00404000u + si * 4u));
            if (si < 3) DBGSTR(" ");
        }
        DBGNL();
        DBGSTR("SRAM[00800000]: ");
        for (uint32_t si = 0; si < 4; si++) {
            DBGHEX(iwl_read_sram(0x00800000u + si * 4u));
            if (si < 3) DBGSTR(" ");
        }
        DBGNL();
        /* fry664: Read SRAM error table at default UMAC offset */
        DBGSTR("SRAM[00800020]: ");
        for (uint32_t si = 0; si < 4; si++) {
            DBGHEX(iwl_read_sram(0x00800020u + si * 4u));
            if (si < 3) DBGSTR(" ");
        }
        DBGNL();
    }

    /* RX buf 0 hex dump (first 16 bytes) */
    if (g_iwl_dma.allocated && g_iwl_dma.rx_buf_virt[0]) {
        iwl_cache_flush(g_iwl_dma.rx_buf_virt[0], 32);
        const uint8_t *p = (const uint8_t *)g_iwl_dma.rx_buf_virt[0];
        DBGSTR("RX[0]: ");
        for (uint32_t bi = 0; bi < 16 && pos + 2 < bufsz; bi++) {
            buf[pos++] = "0123456789abcdef"[(p[bi] >> 4) & 0xF];
            buf[pos++] = "0123456789abcdef"[p[bi] & 0xF];
        }
        DBGNL();
    }

done:
#undef DBGSTR
#undef DBGHEX
#undef DBGDEC
#undef DBGNL
#undef DBGREG

    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

/*
 * wd3 — deep boot trace + raw ring state.
 * Intended for stubborn ALIVE-stage failures where wd/wh/wc are too coarse.
 */
int wifi_9260_get_debug_log3(char *buf, uint32_t bufsz) {
    if (!buf || bufsz == 0) return -1;
    uint32_t pos = 0;

#define DBGSTR(s) do { \
    const char *_s = (s); \
    while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; \
} while(0)
#define DBGHEX(val) do { \
    uint32_t _v = (val); \
    for (int _s = 28; _s >= 0 && pos < bufsz - 1; _s -= 4) \
        buf[pos++] = "0123456789abcdef"[(_v >> _s) & 0xF]; \
} while(0)
#define DBGDEC(v) do { \
    uint32_t _v = (v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)
#define DBGNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)
#define DBGREG(label, val) do { DBGSTR(label); DBGHEX(val); DBGNL(); } while(0)

    DBGSTR("--- WiFi Trace ---\n");
    DBGSTR("step="); DBGDEC(g_iwl_init_step);
    DBGSTR(" rc=");
    if (g_iwl_init_rc < 0) { DBGSTR("-"); DBGDEC((uint32_t)(-(int)g_iwl_init_rc)); }
    else DBGDEC((uint32_t)g_iwl_init_rc);
    DBGSTR(" irq="); DBGDEC(g_iwl_irq_fired); DBGNL();

    DBGSTR("--- BOOT TRACE ---\n");
    DBGREG("secure_cpu1_hdr=0x", g_iwl_bootdbg.secure_cpu1_hdr_readback);
    DBGREG("secure_cpu2_hdr=0x", g_iwl_bootdbg.secure_cpu2_hdr_readback);
    DBGREG("load_int_mask=0x", g_iwl_bootdbg.load_int_mask);
    DBGREG("alive_int_mask=0x", g_iwl_bootdbg.alive_int_mask);
    if (g_iwl_bootdbg.fw_sec_total) {
        DBGSTR("fw_img=");
        DBGSTR(g_iwl_bootdbg.fw_upload_is_init ? "init" : "rt");
        DBGSTR(" fw_sec_total="); DBGDEC(g_iwl_bootdbg.fw_sec_total); DBGNL();
        DBGSTR("fw_cpu1 start="); DBGDEC(g_iwl_bootdbg.fw_cpu1.start_idx);
        DBGSTR(" load="); DBGDEC(g_iwl_bootdbg.fw_cpu1.loaded);
        DBGSTR(" break="); DBGDEC(g_iwl_bootdbg.fw_cpu1.break_idx);
        DBGSTR(" off=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu1.break_off);
        DBGSTR(" first=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu1.first_off);
        DBGSTR(" last=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu1.last_off);
        DBGSTR(" done=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu1.done_status); DBGNL();
        DBGSTR("fw_cpu2 start="); DBGDEC(g_iwl_bootdbg.fw_cpu2.start_idx);
        DBGSTR(" load="); DBGDEC(g_iwl_bootdbg.fw_cpu2.loaded);
        DBGSTR(" break="); DBGDEC(g_iwl_bootdbg.fw_cpu2.break_idx);
        DBGSTR(" off=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu2.break_off);
        DBGSTR(" first=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu2.first_off);
        DBGSTR(" last=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu2.last_off);
        DBGSTR(" done=0x"); DBGHEX(g_iwl_bootdbg.fw_cpu2.done_status); DBGNL();
    }
    DBGREG("rfh_frbdcb_ba=0x", g_iwl_bootdbg.rfh_frbdcb_ba_lsb);
    DBGREG("rfh_urbdcb_ba=0x", g_iwl_bootdbg.rfh_urbdcb_ba_lsb);
    DBGREG("rfh_status_wptr=0x", g_iwl_bootdbg.rfh_status_wptr_lsb);
    DBGREG("rfh_widx_trg=0x", g_iwl_bootdbg.rfh_widx_trg_written);

    DBGSTR("--- PHASE TRACE ---\n");
    for (uint32_t i = 0; i < g_iwl_boot_trace_count; i++) {
        const struct iwl_boot_trace_entry *e = &g_iwl_boot_trace[i];
        DBGSTR("#"); DBGDEC(i); DBGSTR(" ");
        DBGSTR(e->tag ? e->tag : "?");
        DBGSTR(" step="); DBGDEC(e->step);
        DBGSTR(" rc=");
        if (e->rc < 0) { DBGSTR("-"); DBGDEC((uint32_t)(-(int)e->rc)); }
        else DBGDEC((uint32_t)e->rc);
        DBGSTR(" irq="); DBGDEC(e->irq_count); DBGNL();
        DBGSTR("  im=0x"); DBGHEX(e->int_mask);
        DBGSTR(" int=0x"); DBGHEX(e->csr_int);
        DBGSTR(" fh=0x"); DBGHEX(e->fh_int);
        DBGSTR(" gp=0x"); DBGHEX(e->gp_cntrl);
        DBGSTR(" rst=0x"); DBGHEX(e->csr_reset);
        DBGSTR(" load=0x"); DBGHEX(e->fh_load); DBGNL();
        DBGSTR("  wfpm=0x"); DBGHEX(e->wfpm_gp2);
        DBGSTR(" cpu_rst=0x"); DBGHEX(e->cpu_reset);
        DBGSTR(" c1=0x"); DBGHEX(e->cpu1);
        DBGSTR(" c2=0x"); DBGHEX(e->cpu2); DBGNL();
        DBGSTR("  rfh act=0x"); DBGHEX(e->rfh_act);
        DBGSTR(" w=0x"); DBGHEX(e->rfh_w);
        DBGSTR(" r=0x"); DBGHEX(e->rfh_r);
        DBGSTR(" u=0x"); DBGHEX(e->rfh_u); DBGNL();
    }

    if (!g_iwl_mmio)
        goto done;

    if (iwl_grab_nic_access() == 0) {
        DBGSTR("--- LIVE PRPH ---\n");
        DBGREG("UREG_UCODE_LOAD_STATUS=0x", iwl_read_prph(UREG_UCODE_LOAD_STATUS));
        DBGREG("LMPM_SECURE_CPU1_HDR=0x", iwl_read_prph(LMPM_SECURE_UCODE_LOAD_CPU1_HDR_ADDR));
        DBGREG("LMPM_SECURE_CPU2_HDR=0x", iwl_read_prph(LMPM_SECURE_UCODE_LOAD_CPU2_HDR_ADDR));
        DBGREG("RFH_FRBDCB_BA_LSB=0x", iwl_read_prph(RFH_Q0_FRBDCB_BA_LSB));
        DBGREG("RFH_URBDCB_BA_LSB=0x", iwl_read_prph(RFH_Q0_URBDCB_BA_LSB));
        DBGREG("RFH_STTS_WPTR_LSB=0x", iwl_read_prph(RFH_Q0_URBD_STTS_WPTR_LSB));
        DBGREG("RFH_DMA_CFG=0x", iwl_read_prph(RFH_RXF_DMA_CFG));
        DBGREG("RFH_GEN_CFG=0x", iwl_read_prph(RFH_GEN_CFG));
        DBGREG("RFH_GEN_STATUS=0x", iwl_read_prph(RFH_GEN_STATUS));
    }

    if (g_iwl_dma.allocated && g_iwl_dma.rx_status) {
        iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
        DBGSTR("--- RX STATUS RAW ---\n");
        DBGSTR("closed_rb=0x"); DBGHEX(g_iwl_dma.rx_status->closed_rb_num);
        DBGSTR(" closed_fr=0x"); DBGHEX(g_iwl_dma.rx_status->closed_fr_num); DBGNL();
        DBGSTR("finished_rb=0x"); DBGHEX(g_iwl_dma.rx_status->finished_rb_num);
        DBGSTR(" finished_fr=0x"); DBGHEX(g_iwl_dma.rx_status->finished_fr_num); DBGNL();
        DBGREG("spare=0x", g_iwl_dma.rx_status->spare);
    }

    if (g_iwl_dma.allocated && g_iwl_dma.rxbd_ring) {
        DBGSTR("--- RXBD[0..7] ---\n");
        for (uint32_t i = 0; i < 8 && i < IWL_RX_QUEUE_SIZE; i++) {
            uint64_t v = g_iwl_dma.rxbd_ring[i].addr;
            DBGSTR("["); DBGDEC(i); DBGSTR("] 0x");
            DBGHEX((uint32_t)(v >> 32));
            DBGHEX((uint32_t)v);
            DBGNL();
        }
    }

    if (g_iwl_dma.allocated && g_iwl_dma.rx_used_ring) {
        DBGSTR("--- RXUSED[0..7] ---\n");
        for (uint32_t i = 0; i < 8 && i < IWL_RX_QUEUE_SIZE; i++) {
            iwl_cache_flush(&g_iwl_dma.rx_used_ring[i], sizeof(uint32_t));
            DBGSTR("["); DBGDEC(i); DBGSTR("] 0x");
            DBGHEX(g_iwl_dma.rx_used_ring[i]);
            DBGNL();
        }
    }

done:
#undef DBGSTR
#undef DBGHEX
#undef DBGDEC
#undef DBGNL
#undef DBGREG
    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

/*
 * wl (wifi log) — just the init debug log (iwl_dbg messages from boot).
 * Separated from wd to avoid truncation.
 */
int wifi_9260_get_init_log(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 2) return -1;
    uint32_t log_len = g_iwl_dbg_pos;
    if (log_len == 0 || !g_iwl_dbg_buf) {
        buf[0] = '\0';
        return 0;
    }
    uint32_t copy_len = (log_len < bufsz - 1) ? log_len : bufsz - 1;
    iwl_copy(buf, g_iwl_dbg_buf, copy_len);
    buf[copy_len] = '\0';
    return (int)copy_len;
}

int wifi_9260_get_cmd_trace(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 128) return -1;
    uint32_t pos = 0;
    uint32_t start = (g_iwl_cmd_trace_count < IWL_CMD_TRACE_MAX) ? 0u : g_iwl_cmd_trace_next;
    uint32_t hostcmd_count = 0;

#define CMDSTR(s) do { const char *_s = (s); while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; } while(0)
#define CMDHEX8(val) do { \
    uint32_t _v = (uint32_t)(val) & 0xFFu; \
    if (pos < bufsz - 1) buf[pos++] = "0123456789abcdef"[(_v >> 4) & 0xF]; \
    if (pos < bufsz - 1) buf[pos++] = "0123456789abcdef"[_v & 0xF]; \
} while(0)
#define CMDHEX16(val) do { \
    uint32_t _v = (uint32_t)(val) & 0xFFFFu; \
    for (int _s = 12; _s >= 0 && pos < bufsz - 1; _s -= 4) \
        buf[pos++] = "0123456789abcdef"[(_v >> _s) & 0xF]; \
} while(0)
#define CMDHEX32(val) do { \
    uint32_t _v = (uint32_t)(val); \
    for (int _s = 28; _s >= 0 && pos < bufsz - 1; _s -= 4) \
        buf[pos++] = "0123456789abcdef"[(_v >> _s) & 0xF]; \
} while(0)
#define CMDDEC(v) do { \
    uint32_t _v = (uint32_t)(v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)
#define CMDSDEC(v) do { \
    int32_t _sv = (int32_t)(v); \
    uint32_t _uv = (_sv < 0) ? (uint32_t)(-_sv) : (uint32_t)_sv; \
    if (_sv < 0) CMDSTR("-"); \
    CMDDEC(_uv); \
} while(0)
#define CMDNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)

    for (uint32_t i = 0; i < g_iwl_cmd_trace_count; i++) {
        uint32_t idx = (start + i) % IWL_CMD_TRACE_MAX;
        if (g_iwl_cmd_trace[idx].kind != IWL_CMD_TRACE_PHASE)
            hostcmd_count++;
    }

    CMDSTR("--- WiFi Cmd Trace ---"); CMDNL();
    CMDSTR("retained="); CMDDEC(g_iwl_cmd_trace_count);
    CMDSTR(" total="); CMDDEC(g_iwl_cmd_trace_ord);
    CMDSTR(" hostcmd="); CMDDEC(hostcmd_count);
    CMDSTR(" step="); CMDDEC(g_iwl_init_step);
    CMDSTR(" rc="); CMDSDEC(g_iwl_init_rc); CMDNL();

    if (g_iwl_cmd_trace_count == 0) {
        CMDSTR("no host-command trace recorded"); CMDNL();
        goto done;
    }

    if (hostcmd_count == 0) {
        CMDSTR("no host-command trace recorded; retained phase markers only");
        CMDNL();
    }

    for (uint32_t i = 0; i < g_iwl_cmd_trace_count; i++) {
        uint32_t idx = (start + i) % IWL_CMD_TRACE_MAX;
        const struct iwl_cmd_trace_entry *e = &g_iwl_cmd_trace[idx];
        const char *kind = "?";
        switch (e->kind) {
            case IWL_CMD_TRACE_SEND: kind = "send"; break;
            case IWL_CMD_TRACE_SEND_FAIL: kind = "send-fail"; break;
            case IWL_CMD_TRACE_FIRE: kind = "fire"; break;
            case IWL_CMD_TRACE_RESP: kind = "resp"; break;
            case IWL_CMD_TRACE_ASYNC: kind = "async"; break;
            case IWL_CMD_TRACE_TIMEOUT: kind = "timeout"; break;
            case IWL_CMD_TRACE_PHASE: kind = "phase"; break;
        }

        CMDSTR("#"); CMDDEC(i);
        CMDSTR(" ord="); CMDDEC(e->ord);
        CMDSTR(" "); CMDSTR(kind);
        CMDSTR(" step="); CMDDEC(e->step);
        if (e->kind == IWL_CMD_TRACE_PHASE) {
            CMDSTR(" tag="); CMDSTR(iwl_cmd_trace_phase_name(e->cmd_id));
            if (e->seq) {
                CMDSTR(" arg="); CMDDEC(e->seq);
            }
            if (e->rc) {
                CMDSTR(" rc="); CMDSDEC(e->rc);
            }
            if (e->timeout_ms) {
                CMDSTR(" ms="); CMDDEC(e->timeout_ms);
            }
            CMDSTR(" cl=0x"); CMDHEX16(e->closed_rb);
            CMDSTR(" csr=0x"); CMDHEX32(e->csr_int);
            CMDSTR(" fh=0x"); CMDHEX32(e->fh_int);
            CMDNL();
            continue;
        }
        CMDSTR(" cmd=0x"); CMDHEX8(e->cmd_id);
        CMDSTR(" grp=0x"); CMDHEX8(e->group_id);
        CMDSTR(" seq=0x"); CMDHEX16(e->seq);
        if (e->tx_len) {
            CMDSTR(" len="); CMDDEC(e->tx_len);
        }
        if (e->flags & IWL_CMD_TRACE_F_WANT_RESP) {
            CMDSTR(" wait="); CMDDEC(e->timeout_ms);
        }
        if (e->kind == IWL_CMD_TRACE_SEND_FAIL || e->kind == IWL_CMD_TRACE_TIMEOUT) {
            CMDSTR(" rc="); CMDSDEC(e->rc);
        }
        if (e->kind == IWL_CMD_TRACE_RESP || e->kind == IWL_CMD_TRACE_ASYNC) {
            CMDSTR(" rx=0x"); CMDHEX8(e->rx_cmd);
            CMDSTR("/0x"); CMDHEX8(e->rx_group);
            CMDSTR("/0x"); CMDHEX16(e->rx_seq);
            CMDSTR(" rxlen="); CMDDEC(e->rx_len);
            CMDSTR(" slot="); CMDDEC(e->rx_slot);
        }
        if (e->kind == IWL_CMD_TRACE_RESP ||
            e->kind == IWL_CMD_TRACE_ASYNC ||
            e->kind == IWL_CMD_TRACE_TIMEOUT) {
            CMDSTR(" async="); CMDDEC(e->async_seen);
            CMDSTR(" cl=0x"); CMDHEX16(e->closed_rb);
            CMDSTR(" csr=0x"); CMDHEX32(e->csr_int);
        }
        CMDNL();
    }

done:
#undef CMDSTR
#undef CMDHEX8
#undef CMDHEX16
#undef CMDHEX32
#undef CMDDEC
#undef CMDSDEC
#undef CMDNL
    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

struct iwl_umac_error_event_table {
    uint32_t valid;
    uint32_t error_id;
    uint32_t blink1;
    uint32_t blink2;
    uint32_t ilink1;
    uint32_t ilink2;
    uint32_t data1;
    uint32_t data2;
    uint32_t data3;
    uint32_t umac_major;
    uint32_t umac_minor;
    uint32_t frame_pointer;
    uint32_t stack_pointer;
    uint32_t cmd_header;
    uint32_t nic_isr_pref;
};

struct iwl_diag_reg_desc {
    uint32_t addr;
    const char *name;
};

static const struct iwl_diag_reg_desc g_iwl_fseq_diag_regs[] = {
    { FSEQ_ERROR_CODE, "FSEQ_ERROR_CODE" },
    { FSEQ_TOP_INIT_VERSION, "FSEQ_TOP_INIT_VERSION" },
    { FSEQ_CNVIO_INIT_VERSION, "FSEQ_CNVIO_INIT_VERSION" },
    { FSEQ_OTP_VERSION, "FSEQ_OTP_VERSION" },
    { FSEQ_TOP_CONTENT_VERSION, "FSEQ_TOP_CONTENT_VERSION" },
    { FSEQ_ALIVE_TOKEN, "FSEQ_ALIVE_TOKEN" },
    { FSEQ_CNVI_ID, "FSEQ_CNVI_ID" },
    { FSEQ_CNVR_ID, "FSEQ_CNVR_ID" },
    { FSEQ_PREV_CNVIO_INIT_VERSION, "FSEQ_PREV_CNVIO_INIT_VERSION" },
    { FSEQ_WIFI_FSEQ_VERSION, "FSEQ_WIFI_FSEQ_VERSION" },
    { FSEQ_BT_FSEQ_VERSION, "FSEQ_BT_FSEQ_VERSION" },
    { FSEQ_CLASS_TP_VERSION, "FSEQ_CLASS_TP_VERSION" },
};

static const struct iwl_diag_reg_desc g_iwl_pd_diag_regs[] = {
    { WFPM_ARC1_PD_NOTIFICATION, "WFPM_ARC1_PD_NOTIFICATION" },
    { WFPM_LMAC1_PD_NOTIFICATION, "WFPM_LMAC1_PD_NOTIFICATION" },
    { HPM_SECONDARY_DEVICE_STATE, "HPM_SECONDARY_DEVICE_STATE" },
    { WFPM_MAC_OTP_CFG7_ADDR, "WFPM_MAC_OTP_CFG7_ADDR" },
    { WFPM_MAC_OTP_CFG7_DATA, "WFPM_MAC_OTP_CFG7_DATA" },
};

/* fry716 Step 2: CPU program counter and status registers for crash diagnosis */
#define SB_CPU_1_STATUS         0xA01E30u
#define SB_CPU_2_STATUS         0xA01E34u
#define UMAG_SB_CPU_1_STATUS    0xA038C0u
#define UMAG_SB_CPU_2_STATUS    0xA038C4u
#define UREG_LMAC2_CURRENT_PC   0xA05C20u
#define MON_DMARB_RD_CTL_ADDR   0xA03C60u
#define MON_DMARB_RD_DATA_ADDR  0xA03C5Cu

static const struct iwl_diag_reg_desc g_iwl_cpu_pc_regs[] = {
    { UREG_UMAC_CURRENT_PC,    "UMAC_PC" },
    { UREG_LMAC1_CURRENT_PC,   "LMAC1_PC" },
    { UREG_LMAC2_CURRENT_PC,   "LMAC2_PC" },
    { SB_CPU_1_STATUS,         "SB_CPU1_STATUS" },
    { SB_CPU_2_STATUS,         "SB_CPU2_STATUS" },
    { UMAG_SB_CPU_1_STATUS,    "UMAG_SB_CPU1_STATUS" },
    { UMAG_SB_CPU_2_STATUS,    "UMAG_SB_CPU2_STATUS" },
};

static uint32_t iwl_diag_buf_puts(char *buf, uint32_t pos, uint32_t bufsz,
                                  const char *s) {
    while (s && *s && pos < bufsz - 1u)
        buf[pos++] = *s++;
    return pos;
}

static uint32_t iwl_diag_buf_put_hex32(char *buf, uint32_t pos, uint32_t bufsz,
                                       uint32_t value) {
    for (int shift = 28; shift >= 0 && pos < bufsz - 1u; shift -= 4)
        buf[pos++] = "0123456789abcdef"[(value >> shift) & 0xFu];
    return pos;
}

static uint32_t iwl_diag_buf_put_dec32(char *buf, uint32_t pos, uint32_t bufsz,
                                       uint32_t value) {
    char tmp[12];
    int idx = 0;
    if (value == 0u) {
        tmp[idx++] = '0';
    } else {
        while (value && idx < (int)sizeof(tmp)) {
            tmp[idx++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
    }
    for (int j = idx - 1; j >= 0 && pos < bufsz - 1u; j--)
        buf[pos++] = tmp[j];
    return pos;
}

static uint32_t iwl_diag_buf_put_nl(char *buf, uint32_t pos, uint32_t bufsz) {
    if (pos < bufsz - 1u)
        buf[pos++] = '\n';
    return pos;
}

static uint32_t iwl_diag_buf_put_word_list(char *buf, uint32_t pos, uint32_t bufsz,
                                           const uint32_t *words, uint32_t word_count) {
    if (!words || word_count == 0u)
        return iwl_diag_buf_puts(buf, pos, bufsz, "(none)");

    for (uint32_t i = 0; i < word_count; i++) {
        if (i != 0u)
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " ");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, words[i]);
    }
    return pos;
}

static const char *iwl_fw_lookup_assert_desc_local(uint32_t error_id) {
    static const struct {
        const char *name;
        uint32_t num;
    } assert_descs[] = {
        { "NMI_INTERRUPT_WDG", 0x34u },
        { "SYSASSERT", 0x35u },
        { "UCODE_VERSION_MISMATCH", 0x37u },
        { "BAD_COMMAND", 0x38u },
        { "BAD_COMMAND", 0x39u },
        { "NMI_INTERRUPT_DATA_ACTION_PT", 0x3Cu },
        { "FATAL_ERROR", 0x3Du },
        { "NMI_TRM_HW_ERR", 0x46u },
        { "NMI_INTERRUPT_TRM", 0x4Cu },
        { "NMI_INTERRUPT_BREAK_POINT", 0x54u },
        { "NMI_INTERRUPT_WDG_RXF_FULL", 0x5Cu },
        { "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL", 0x64u },
        { "NMI_INTERRUPT_HOST", 0x66u },
        { "NMI_INTERRUPT_LMAC_FATAL", 0x70u },
        { "NMI_INTERRUPT_UMAC_FATAL", 0x71u },
        { "NMI_INTERRUPT_OTHER_LMAC_FATAL", 0x73u },
        { "NMI_INTERRUPT_ACTION_PT", 0x7Cu },
        { "NMI_INTERRUPT_UNKNOWN", 0x84u },
        { "NMI_INTERRUPT_INST_ACTION_PT", 0x86u },
        { "NMI_INTERRUPT_PREG", 0x88u },
        { "PNVM_MISSING", FW_SYSASSERT_PNVM_MISSING },
    };
    uint32_t masked = error_id & ~FW_SYSASSERT_CPU_MASK;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(assert_descs) / sizeof(assert_descs[0])); i++) {
        if (assert_descs[i].num == masked)
            return assert_descs[i].name;
    }
    return "ADVANCED_SYSASSERT";
}

static int iwl_read_umac_error_table_at(uint32_t base,
                                        struct iwl_umac_error_event_table *out,
                                        uint32_t *valid_out) {
    if (!out || !g_iwl_mmio)
        return -1;
    uint32_t valid = iwl_read_sram(base);
    if (valid_out)
        *valid_out = valid;
    if (!iwl_umac_error_marker_matches(valid))
        return -2;
    uint32_t *dst = (uint32_t *)out;
    for (uint32_t i = 0; i < 15u; i++)
        dst[i] = iwl_read_sram(base + i * 4u);
    return 0;
}

static int iwl_read_umac_error_table(struct iwl_umac_error_event_table *out) {
    return iwl_read_umac_error_table_at(IWL_UMAC_ERROR_TABLE_BASE, out, 0);
}

static uint32_t iwl_append_umac_error_decoded(char *buf, uint32_t pos, uint32_t bufsz,
                                              const struct iwl_umac_error_event_table *tbl) {
    if (!tbl)
        return pos;
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- UMAC ERR DECODED ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "valid=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->valid);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " error_id=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->error_id);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " assert=");
    pos = iwl_diag_buf_puts(buf, pos, bufsz,
                            iwl_fw_lookup_assert_desc_local(tbl->error_id));
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "branchlink1=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->blink1);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " branchlink2=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->blink2);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "ilink1=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->ilink1);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " ilink2=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->ilink2);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "data1=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->data1);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " data2=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->data2);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " data3=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->data3);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "umac_major=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->umac_major);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " umac_minor=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->umac_minor);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "frame_pointer=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->frame_pointer);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " stack_pointer=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->stack_pointer);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "last_host_cmd=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->cmd_header);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " nic_isr_pref=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl->nic_isr_pref);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    return pos;
}

static uint32_t iwl_append_named_prph_regs(char *buf, uint32_t pos, uint32_t bufsz,
                                           const char *header,
                                           const struct iwl_diag_reg_desc *regs,
                                           uint32_t count) {
    pos = iwl_diag_buf_puts(buf, pos, bufsz, header);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    for (uint32_t i = 0; i < count; i++) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, regs[i].name);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, iwl_read_prph(regs[i].addr));
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    }
    return pos;
}

static void iwl_log_fw_verify_summary(void) {
    uint32_t staged_bad = 0;
    uint32_t live_bad = 0;
    uint32_t hunt_count = 0;

    if (!g_iwl_mmio) {
        iwl_dbg("IWL9260: verify summary unavailable (MMIO not mapped)\n");
        return;
    }

    int nic_access = (iwl_grab_nic_access() == 0);
    for (uint32_t i = 0; i < g_iwl_bootdbg.fw_sram_probe_count; i++) {
        const struct iwl_sram_probe_entry *probe = &g_iwl_bootdbg.fw_sram_probe[i];
        uint32_t mismatch_word = probe->word_count;

        if (probe->staged_valid &&
            !iwl_sram_probe_words_match(probe, probe->staged, &mismatch_word)) {
            uint32_t mask = iwl_sram_probe_word_mask(probe->size, mismatch_word);
            iwl_dbg("IWL9260: verify staged mismatch sec=%u cpu=%u addr=0x%08x +0x%02x exp=0x%08x got=0x%08x mask=0x%08x\n",
                    probe->sec_idx, probe->cpu_id, probe->addr,
                    mismatch_word * 4u,
                    probe->expected[mismatch_word], probe->staged[mismatch_word], mask);
            staged_bad++;
        }

        if (nic_access) {
            uint32_t live_words[IWL_SRAM_PROBE_WORDS];
            for (uint32_t w = 0; w < probe->word_count; w++)
                live_words[w] = iwl_read_sram(probe->addr + w * 4u);
            mismatch_word = probe->word_count;
            if (!iwl_sram_probe_words_match(probe, live_words, &mismatch_word)) {
                uint32_t mask = iwl_sram_probe_word_mask(probe->size, mismatch_word);
                iwl_dbg("IWL9260: verify live mismatch sec=%u cpu=%u addr=0x%08x +0x%02x exp=0x%08x got=0x%08x mask=0x%08x\n",
                        probe->sec_idx, probe->cpu_id, probe->addr,
                        mismatch_word * 4u,
                        probe->expected[mismatch_word], live_words[mismatch_word], mask);
                live_bad++;
            }
        }
    }

    iwl_dbg("IWL9260: verify summary probes=%u staged_bad=%u live_bad=%u\n",
            g_iwl_bootdbg.fw_sram_probe_count, staged_bad, live_bad);

    if (!nic_access) {
        iwl_dbg("IWL9260: verify DEAD scan unavailable (NIC access failed)\n");
        return;
    }

    for (uint32_t addr = IWL_VERIFY_SCAN_START;
         addr < IWL_VERIFY_SCAN_END && hunt_count < IWL_VERIFY_SCAN_MAX;
         addr += 4u) {
        uint32_t valid = iwl_read_sram(addr);
        if (!iwl_umac_error_marker_matches(valid))
            continue;

        uint32_t error_id = iwl_read_sram(addr + 0x04u);
        iwl_dbg("IWL9260: verify DEAD candidate addr=0x%08x valid=0x%08x err=0x%08x assert=%s\n",
                addr, valid, error_id, iwl_fw_lookup_assert_desc_local(error_id));
        hunt_count++;
    }

    if (!hunt_count) {
        iwl_dbg("IWL9260: verify DEAD scan found no candidates in 0x%08x-0x%08x\n",
                IWL_VERIFY_SCAN_START, IWL_VERIFY_SCAN_END - 1u);
    }
}

int wifi_9260_get_verify_result(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 256u)
        return -1;

    uint32_t pos = 0;
    uint32_t staged_ok = 0;
    uint32_t staged_bad = 0;
    uint32_t staged_unavail = 0;
    uint32_t live_ok = 0;
    uint32_t live_bad = 0;
    uint32_t live_unavail = 0;
    uint32_t hunt_count = 0;
    int nic_access = 0;
    uint32_t best_addr = 0;
    uint32_t best_valid = 0;
    struct iwl_umac_error_event_table best_tbl;
    int best_have = 0;

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- WiFi Verify ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "step=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, g_iwl_init_step);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " rc=");
    if (g_iwl_init_rc < 0) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "-");
        pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, (uint32_t)(-(int32_t)g_iwl_init_rc));
    } else {
        pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, (uint32_t)g_iwl_init_rc);
    }
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " irq=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, g_iwl_irq_fired);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "img=");
    pos = iwl_diag_buf_puts(buf, pos, bufsz,
                            g_iwl_bootdbg.fw_upload_is_init ? "init" : "rt");
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " sec_total=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, g_iwl_bootdbg.fw_sec_total);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " probes=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, g_iwl_bootdbg.fw_sram_probe_count);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "compare_bytes=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, IWL_SRAM_PROBE_WORDS * 4u);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " scan=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, IWL_VERIFY_SCAN_START);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "-0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, IWL_VERIFY_SCAN_END - 1u);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    if (!g_iwl_mmio) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "MMIO not mapped");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        goto done;
    }

    nic_access = (iwl_grab_nic_access() == 0);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- SECTION VERIFY ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    if (g_iwl_bootdbg.fw_sram_probe_count == 0u) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "(no retained probes)");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    } else {
        for (uint32_t i = 0; i < g_iwl_bootdbg.fw_sram_probe_count; i++) {
            const struct iwl_sram_probe_entry *probe = &g_iwl_bootdbg.fw_sram_probe[i];
            uint32_t live_words[IWL_SRAM_PROBE_WORDS];
            uint32_t staged_mismatch = probe->word_count;
            uint32_t live_mismatch = probe->word_count;
            int staged_match = 0;
            int live_match = 0;

            pos = iwl_diag_buf_puts(buf, pos, bufsz, "[");
            pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, i);
            pos = iwl_diag_buf_puts(buf, pos, bufsz, "] cpu=");
            pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, probe->cpu_id);
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " sec=");
            if (probe->sec_idx == IWL_FW_SINGLE_SECTION_IDX)
                pos = iwl_diag_buf_puts(buf, pos, bufsz, "legacy");
            else
                pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, probe->sec_idx);
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " addr=0x");
            pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, probe->addr);
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " size=0x");
            pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, probe->size);
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " words=");
            pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, probe->word_count);
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

            pos = iwl_diag_buf_puts(buf, pos, bufsz, "  staged=");
            if (!probe->staged_valid) {
                pos = iwl_diag_buf_puts(buf, pos, bufsz, "unavailable");
                staged_unavail++;
            } else {
                staged_match = iwl_sram_probe_words_match(probe, probe->staged,
                                                          &staged_mismatch);
                if (staged_match) {
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, "ok");
                    staged_ok++;
                } else {
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, "mismatch@+0x");
                    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, staged_mismatch * 4u);
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, " exp=0x");
                    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz,
                                                 probe->expected[staged_mismatch]);
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, " got=0x");
                    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz,
                                                 probe->staged[staged_mismatch]);
                    staged_bad++;
                }
            }
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

            pos = iwl_diag_buf_puts(buf, pos, bufsz, "  live=");
            if (!nic_access) {
                pos = iwl_diag_buf_puts(buf, pos, bufsz, "unavailable");
                live_unavail++;
            } else {
                for (uint32_t w = 0; w < probe->word_count; w++)
                    live_words[w] = iwl_read_sram(probe->addr + w * 4u);
                live_match = iwl_sram_probe_words_match(probe, live_words,
                                                        &live_mismatch);
                if (live_match) {
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, "ok");
                    live_ok++;
                } else {
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, "mismatch@+0x");
                    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, live_mismatch * 4u);
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, " exp=0x");
                    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz,
                                                 probe->expected[live_mismatch]);
                    pos = iwl_diag_buf_puts(buf, pos, bufsz, " got=0x");
                    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz,
                                                 live_words[live_mismatch]);
                    live_bad++;
                }
            }
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

            pos = iwl_diag_buf_puts(buf, pos, bufsz, "  exp=");
            pos = iwl_diag_buf_put_word_list(buf, pos, bufsz,
                                             probe->expected, probe->word_count);
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

            pos = iwl_diag_buf_puts(buf, pos, bufsz, "  stg=");
            if (!probe->staged_valid) {
                pos = iwl_diag_buf_puts(buf, pos, bufsz, "unavailable");
            } else {
                pos = iwl_diag_buf_put_word_list(buf, pos, bufsz,
                                                 probe->staged, probe->word_count);
            }
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

            pos = iwl_diag_buf_puts(buf, pos, bufsz, "  now=");
            if (!nic_access) {
                pos = iwl_diag_buf_puts(buf, pos, bufsz, "unavailable");
            } else {
                pos = iwl_diag_buf_put_word_list(buf, pos, bufsz,
                                                 live_words, probe->word_count);
            }
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        }
    }

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- SUMMARY ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "staged ok=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, staged_ok);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " bad=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, staged_bad);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " unavailable=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, staged_unavail);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "live   ok=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, live_ok);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " bad=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, live_bad);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " unavailable=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, live_unavail);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- UMAC HUNT ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    if (!nic_access) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "NIC access unavailable");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        goto done;
    }

    for (uint32_t addr = IWL_VERIFY_SCAN_START;
         addr < IWL_VERIFY_SCAN_END && hunt_count < IWL_VERIFY_SCAN_MAX;
         addr += 4u) {
        struct iwl_umac_error_event_table tbl;
        uint32_t valid = 0;
        if (iwl_read_umac_error_table_at(addr, &tbl, &valid) != 0)
            continue;

        pos = iwl_diag_buf_puts(buf, pos, bufsz, "addr=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, addr);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, " valid=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, valid);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, " err=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, tbl.error_id);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, " assert=");
        pos = iwl_diag_buf_puts(buf, pos, bufsz,
                                iwl_fw_lookup_assert_desc_local(tbl.error_id));
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

        if (!best_have ||
            (valid == IWL_UMAC_ERROR_TABLE_VALID &&
             best_valid != IWL_UMAC_ERROR_TABLE_VALID)) {
            best_have = 1;
            best_addr = addr;
            best_valid = valid;
            best_tbl = tbl;
        }
        hunt_count++;
    }

    if (!hunt_count) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "(none found)");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    } else if (best_have) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- BEST CANDIDATE ---");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "base=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, best_addr);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, " valid=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, best_valid);
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        pos = iwl_append_umac_error_decoded(buf, pos, bufsz, &best_tbl);
    }

done:
    if (pos < bufsz)
        buf[pos] = '\0';
    else
        buf[bufsz - 1u] = '\0';
    return (int)pos;
}

int wifi_9260_get_deep_diag(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 256u)
        return -1;

    uint32_t pos = 0;
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- WiFi Deep Diag ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "step=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, g_iwl_init_step);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, " irq=");
    pos = iwl_diag_buf_put_dec32(buf, pos, bufsz, g_iwl_irq_fired);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    if (!g_iwl_mmio) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "MMIO not mapped");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        goto done;
    }

    if (iwl_grab_nic_access() != 0) {
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "NIC access unavailable");
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        goto done;
    }

    {
        struct iwl_umac_error_event_table umac_err;
        if (iwl_read_umac_error_table(&umac_err) == 0) {
            pos = iwl_append_umac_error_decoded(buf, pos, bufsz, &umac_err);
        } else {
            pos = iwl_diag_buf_puts(buf, pos, bufsz,
                                    "--- UMAC ERR DECODED ---");
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
            pos = iwl_diag_buf_puts(buf, pos, bufsz,
                                    "UMAC error table unavailable");
            pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
        }
    }

    pos = iwl_append_named_prph_regs(buf, pos, bufsz, "--- FSEQ ---",
                                     g_iwl_fseq_diag_regs,
                                     (uint32_t)(sizeof(g_iwl_fseq_diag_regs) /
                                                sizeof(g_iwl_fseq_diag_regs[0])));
    pos = iwl_append_named_prph_regs(buf, pos, bufsz, "--- PD/OTP ---",
                                     g_iwl_pd_diag_regs,
                                     (uint32_t)(sizeof(g_iwl_pd_diag_regs) /
                                                sizeof(g_iwl_pd_diag_regs[0])));

    /* fry716 Step 2: CPU program counters — read twice to detect if stuck */
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- CPU PC (read twice) ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_iwl_cpu_pc_regs) /
                                         sizeof(g_iwl_cpu_pc_regs[0])); i++) {
        uint32_t val_a = iwl_read_prph(g_iwl_cpu_pc_regs[i].addr);
        uint32_t val_b = iwl_read_prph(g_iwl_cpu_pc_regs[i].addr);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, g_iwl_cpu_pc_regs[i].name);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, val_a);
        if (val_a != val_b) {
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " -> 0x");
            pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, val_b);
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " (MOVING)");
        } else {
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " (stuck)");
        }
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    }

    /* fry716 Step 2: DMA arbiter monitor */
    iwl_write_prph(MON_DMARB_RD_CTL_ADDR, 0x1u);
    uint32_t dmarb_val = iwl_read_prph(MON_DMARB_RD_DATA_ADDR);
    iwl_write_prph(MON_DMARB_RD_CTL_ADDR, 0x0u);
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "MON_DMARB_RD_DATA=0x");
    pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, dmarb_val);
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);

    /* fry716 Step 1: Read back error table sentinel region (SRAM 0x00-0x3C) */
    /* fry718: Sentinel writes removed (they corrupted section [1]).
     * This section now just reads SRAM 0x00-0x3C to show error table state. */
    pos = iwl_diag_buf_puts(buf, pos, bufsz, "--- SRAM ERR TABLE (0x00-0x3C) ---");
    pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    for (uint32_t j = 0; j < 15u; j++) {
        iwl_write32(HBUS_TARG_MEM_RADDR, j * 4u);
        uint32_t sval = iwl_read32(HBUS_TARG_MEM_RDAT);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "+0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, j * 4u);
        pos = iwl_diag_buf_puts(buf, pos, bufsz, "=0x");
        pos = iwl_diag_buf_put_hex32(buf, pos, bufsz, sval);
        if (sval == 0x0000DEADu && j == 0) {
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " DEAD(crash)");
        } else if (sval == 0xa5a5a5a2u) {
            pos = iwl_diag_buf_puts(buf, pos, bufsz, " UNINIT");
        }
        pos = iwl_diag_buf_put_nl(buf, pos, bufsz);
    }

done:
    if (pos < bufsz)
        buf[pos] = '\0';
    else
        buf[bufsz - 1u] = '\0';
    return (int)pos;
}

/*
 * sram — comprehensive SRAM-focused dump for WiFi bring-up.
 * Combines fixed live probes, retained staged section readback, and
 * common error-table markers into one shell-friendly command.
 */
int wifi_9260_get_sram_dump(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 256) return -1;
    uint32_t pos = 0;
    int nic_access = 0;

    static const uint32_t fixed_probes[] = {
        0x00000000u, 0x00000200u, 0x00000400u,
        0x00404000u, 0x00456000u,
        0x00800000u, 0x00800020u, 0x00800200u, 0x00800400u
    };

#define SRSTR(s) do { const char *_s = (s); while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; } while(0)
#define SRHEX(val) do { \
    uint32_t _v = (val); \
    for (int _s = 28; _s >= 0 && pos < bufsz - 1; _s -= 4) \
        buf[pos++] = "0123456789abcdef"[(_v >> _s) & 0xF]; \
} while(0)
#define SRDEC(v) do { \
    uint32_t _v = (v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)
#define SRNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)

    SRSTR("--- WiFi SRAM ---"); SRNL();
    SRSTR("step="); SRDEC(g_iwl_init_step);
    SRSTR(" rc=");
    if (g_iwl_init_rc < 0) { SRSTR("-"); SRDEC((uint32_t)(-(int)g_iwl_init_rc)); }
    else SRDEC((uint32_t)g_iwl_init_rc);
    SRSTR(" irq="); SRDEC(g_iwl_irq_fired); SRNL();

    if (!g_iwl_mmio) {
        SRSTR("MMIO not mapped"); SRNL();
        goto done;
    }

    nic_access = (iwl_grab_nic_access() == 0);

    SRSTR("--- UPLOAD ---"); SRNL();
    SRSTR("img=");
    SRSTR(g_iwl_bootdbg.fw_upload_is_init ? "init" : "rt");
    SRSTR(" sec_total="); SRDEC(g_iwl_bootdbg.fw_sec_total);
    SRSTR(" probes="); SRDEC(g_iwl_bootdbg.fw_sram_probe_count); SRNL();
    SRSTR("cpu1=");
    SRDEC(g_iwl_bootdbg.fw_cpu1.start_idx); SRSTR("/");
    SRDEC(g_iwl_bootdbg.fw_cpu1.loaded); SRSTR("/");
    SRDEC(g_iwl_bootdbg.fw_cpu1.break_idx);
    SRSTR(" first=0x"); SRHEX(g_iwl_bootdbg.fw_cpu1.first_off);
    SRSTR(" last=0x"); SRHEX(g_iwl_bootdbg.fw_cpu1.last_off);
    SRSTR(" done=0x"); SRHEX(g_iwl_bootdbg.fw_cpu1.done_status); SRNL();
    SRSTR("cpu2=");
    SRDEC(g_iwl_bootdbg.fw_cpu2.start_idx); SRSTR("/");
    SRDEC(g_iwl_bootdbg.fw_cpu2.loaded); SRSTR("/");
    SRDEC(g_iwl_bootdbg.fw_cpu2.break_idx);
    SRSTR(" first=0x"); SRHEX(g_iwl_bootdbg.fw_cpu2.first_off);
    SRSTR(" last=0x"); SRHEX(g_iwl_bootdbg.fw_cpu2.last_off);
    SRSTR(" done=0x"); SRHEX(g_iwl_bootdbg.fw_cpu2.done_status); SRNL();

    SRSTR("--- FIXED PROBES ---"); SRNL();
    if (!nic_access) {
        SRSTR("NIC access unavailable"); SRNL();
    } else {
        for (uint32_t i = 0; i < sizeof(fixed_probes) / sizeof(fixed_probes[0]); i++) {
            SRSTR("["); SRHEX(fixed_probes[i]); SRSTR("] ");
            for (uint32_t w = 0; w < IWL_SRAM_PROBE_WORDS; w++) {
                if (w) SRSTR(" ");
                SRHEX(iwl_read_sram(fixed_probes[i] + w * 4u));
            }
            SRNL();
        }
    }

    SRSTR("--- SECTION PROBES ---"); SRNL();
    if (g_iwl_bootdbg.fw_sram_probe_count == 0) {
        SRSTR("(none retained)"); SRNL();
    } else {
        for (uint32_t i = 0; i < g_iwl_bootdbg.fw_sram_probe_count; i++) {
            const struct iwl_sram_probe_entry *probe = &g_iwl_bootdbg.fw_sram_probe[i];
            SRSTR("["); SRDEC(i); SRSTR("] cpu="); SRDEC(probe->cpu_id);
            SRSTR(" sec=");
            if (probe->sec_idx == IWL_FW_SINGLE_SECTION_IDX) SRSTR("legacy");
            else SRDEC(probe->sec_idx);
            SRSTR(" addr=0x"); SRHEX(probe->addr);
            SRSTR(" size=0x"); SRHEX(probe->size);
            SRSTR(" ext="); SRDEC(probe->extended);
            SRSTR(" words="); SRDEC(probe->word_count); SRNL();

            SRSTR("  exp=");
            if (probe->word_count == 0) {
                SRSTR("(none)");
            } else {
                for (uint32_t w = 0; w < probe->word_count; w++) {
                    if (w) SRSTR(" ");
                    SRHEX(probe->expected[w]);
                }
            }
            SRNL();

            SRSTR("  stg=");
            if (!probe->staged_valid) {
                SRSTR("unavailable");
            } else {
                for (uint32_t w = 0; w < probe->word_count; w++) {
                    if (w) SRSTR(" ");
                    SRHEX(probe->staged[w]);
                }
            }
            SRNL();

            SRSTR("  now=");
            if (!nic_access) {
                SRSTR("unavailable");
            } else {
                for (uint32_t w = 0; w < probe->word_count; w++) {
                    if (w) SRSTR(" ");
                    SRHEX(iwl_read_sram(probe->addr + w * 4u));
                }
            }
            SRNL();
        }
    }

    SRSTR("--- DEAD SCAN ---"); SRNL();
    if (!nic_access) {
        SRSTR("unavailable"); SRNL();
    } else {
        uint32_t found = 0;
        for (uint32_t addr = 0; addr < 0x080000u && found < 8u; addr += 0x400u) {
            uint32_t valid = iwl_read_sram(addr);
            if (!iwl_umac_error_marker_matches(valid))
                continue;
            SRSTR("addr=0x"); SRHEX(addr);
            SRSTR(" valid=0x"); SRHEX(valid);
            SRSTR(" err=0x"); SRHEX(iwl_read_sram(addr + 0x04u));
            SRSTR(" pc=0x"); SRHEX(iwl_read_sram(addr + 0x08u)); SRNL();
            found++;
        }
        if (!found) {
            SRSTR("(none in 0x000000-0x07ffff)"); SRNL();
        }
    }

    SRSTR("--- UMAC ERR @00800000 ---"); SRNL();
    if (!nic_access) {
        SRSTR("unavailable"); SRNL();
    } else {
        uint32_t marker = iwl_read_sram(IWL_UMAC_ERROR_TABLE_BASE);
        SRSTR("marker=0x"); SRHEX(marker); SRNL();
        if (marker == IWL_UMAC_ERROR_TABLE_VALID) {
            for (uint32_t i = 0; i < 15u; i++) {
                SRSTR("+"); SRHEX(i * 4u);
                SRSTR("=0x"); SRHEX(iwl_read_sram(IWL_UMAC_ERROR_TABLE_BASE + i * 4u)); SRNL();
            }
        }
    }

done:
#undef SRSTR
#undef SRHEX
#undef SRDEC
#undef SRNL
    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

/*
 * wc (wifi cpu) — compact CPU status dump for shell.
 * Small buffer, won't get truncated like wd.
 */
int wifi_9260_get_cpu_status(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 128) return -1;
    uint32_t pos = 0;

#define CPUSTR(s) do { const char *_s = (s); while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; } while(0)
#define CPUHEX(val) do { \
    uint32_t _v = (val); \
    for (int _s = 28; _s >= 0; _s -= 4) \
        if (pos < bufsz - 1) buf[pos++] = "0123456789abcdef"[(_v >> _s) & 0xF]; \
} while(0)
#define CPUNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)
#define CPUDEC(v) do { \
    uint32_t _v = (v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)

    CPUSTR("--- WiFi CPU Status ---"); CPUNL();
    CPUSTR("step="); CPUDEC(g_iwl_init_step);
    CPUSTR(" rc=");
    if (g_iwl_init_rc < 0) { CPUSTR("-"); CPUDEC((uint32_t)(-(int)g_iwl_init_rc)); }
    else CPUDEC((uint32_t)g_iwl_init_rc);
    CPUSTR(" irq="); CPUDEC(g_iwl_irq_fired); CPUNL();

    if (!g_iwl_mmio) {
        CPUSTR("MMIO not mapped"); CPUNL();
        goto done;
    }

    CPUSTR("CSR_INT=0x"); CPUHEX(iwl_read32(CSR_INT)); CPUNL();
    CPUSTR("CSR_RESET=0x"); CPUHEX(iwl_read32(CSR_RESET)); CPUNL();
    CPUSTR("GP_CNTRL=0x"); CPUHEX(iwl_read32(CSR_GP_CNTRL)); CPUNL();
    CPUSTR("FH_LOAD=0x"); CPUHEX(iwl_read32(FH_UCODE_LOAD_STATUS)); CPUNL();
    CPUSTR("last_isr_csr=0x"); CPUHEX(g_iwl_last_csr_int); CPUNL();
    CPUSTR("last_isr_fh=0x"); CPUHEX(g_iwl_last_fh_int); CPUNL();

    if (iwl_grab_nic_access() == 0) {
        uint32_t cpu1 = iwl_read_prph(SB_CPU_1_STATUS);
        uint32_t cpu2 = iwl_read_prph(SB_CPU_2_STATUS);
        uint32_t pc1a = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t pc2a = iwl_read_prph(UREG_LMAC1_CURRENT_PC);
        iwl_udelay(10000); /* 10ms */
        uint32_t pc1b = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t pc2b = iwl_read_prph(UREG_LMAC1_CURRENT_PC);

        CPUSTR("CPU1_ST=0x"); CPUHEX(cpu1); CPUNL();
        CPUSTR("CPU2_ST=0x"); CPUHEX(cpu2); CPUNL();
        CPUSTR("UMAC_PC=0x"); CPUHEX(pc1a); CPUSTR("->0x"); CPUHEX(pc1b); CPUNL();
        CPUSTR("LMAC_PC=0x"); CPUHEX(pc2a); CPUSTR("->0x"); CPUHEX(pc2b); CPUNL();
        if (pc1a != pc1b || pc2a != pc2b)
            CPUSTR("VERDICT: FW IS RUNNING");
        else if (cpu1 == 0 && cpu2 == 0)
            CPUSTR("VERDICT: CPU NEVER STARTED");
        else
            CPUSTR("VERDICT: CPU STUCK");
        CPUNL();

        /* RFH queue state */
        uint32_t rfh_act = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        uint32_t rfh_widx = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        uint32_t rfh_ridx = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        uint32_t rfh_uwdx = iwl_read_prph(RFH_Q0_URBDCB_WIDX);
        CPUSTR("RFH_ACT=0x"); CPUHEX(rfh_act);
        CPUSTR(" W=0x"); CPUHEX(rfh_widx);
        CPUSTR(" R=0x"); CPUHEX(rfh_ridx);
        CPUSTR(" U=0x"); CPUHEX(rfh_uwdx); CPUNL();
    } else {
        CPUSTR("NIC ACCESS FAILED"); CPUNL();
    }

    if (g_iwl_dma.allocated && g_iwl_dma.rx_status) {
        iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
        uint16_t cl = g_iwl_dma.rx_status->closed_rb_num;
        uint16_t fi = g_iwl_dma.rx_status->finished_rb_num;
        CPUSTR("rx_closed=0x"); CPUHEX(cl);
        CPUSTR(" rx_fin=0x"); CPUHEX(fi); CPUNL();
    }

    /* RFH doorbell diagnostics (from boot) */
    CPUSTR("--- RFH DOORBELL ---"); CPUNL();
    CPUSTR("widx_trg_written=0x"); CPUHEX(g_iwl_bootdbg.rfh_widx_trg_written); CPUNL();
    CPUSTR("frbdcb_ba=0x"); CPUHEX(g_iwl_bootdbg.rfh_frbdcb_ba_lsb); CPUNL();
    CPUSTR("urbdcb_ba=0x"); CPUHEX(g_iwl_bootdbg.rfh_urbdcb_ba_lsb); CPUNL();
    CPUSTR("stts_wptr=0x"); CPUHEX(g_iwl_bootdbg.rfh_status_wptr_lsb); CPUNL();
    CPUSTR("fh_load_after=0x"); CPUHEX(g_iwl_bootdbg.fh_load_status_after_upload); CPUNL();
    if (g_iwl_bootdbg.fw_sec_total) {
        CPUSTR("--- FW UPLOAD ---"); CPUNL();
        CPUSTR("img="); CPUSTR(g_iwl_bootdbg.fw_upload_is_init ? "init" : "rt");
        CPUSTR(" sec_total="); CPUDEC(g_iwl_bootdbg.fw_sec_total); CPUNL();
        CPUSTR("cpu1 start="); CPUDEC(g_iwl_bootdbg.fw_cpu1.start_idx);
        CPUSTR(" load="); CPUDEC(g_iwl_bootdbg.fw_cpu1.loaded);
        CPUSTR(" break="); CPUDEC(g_iwl_bootdbg.fw_cpu1.break_idx);
        CPUSTR(" off=0x"); CPUHEX(g_iwl_bootdbg.fw_cpu1.break_off);
        CPUSTR(" done=0x"); CPUHEX(g_iwl_bootdbg.fw_cpu1.done_status); CPUNL();
        CPUSTR("cpu2 start="); CPUDEC(g_iwl_bootdbg.fw_cpu2.start_idx);
        CPUSTR(" load="); CPUDEC(g_iwl_bootdbg.fw_cpu2.loaded);
        CPUSTR(" break="); CPUDEC(g_iwl_bootdbg.fw_cpu2.break_idx);
        CPUSTR(" off=0x"); CPUHEX(g_iwl_bootdbg.fw_cpu2.break_off);
        CPUSTR(" done=0x"); CPUHEX(g_iwl_bootdbg.fw_cpu2.done_status); CPUNL();
    }

    /* Live RFH config readback */
    if (g_iwl_mmio && iwl_grab_nic_access() == 0) {
        CPUSTR("--- RFH LIVE CFG ---"); CPUNL();
        CPUSTR("DMA_CFG=0x"); CPUHEX(iwl_read_prph(RFH_RXF_DMA_CFG)); CPUNL();
        CPUSTR("GEN_CFG=0x"); CPUHEX(iwl_read_prph(RFH_GEN_CFG)); CPUNL();
        CPUSTR("GEN_ST=0x"); CPUHEX(iwl_read_prph(RFH_GEN_STATUS)); CPUNL();
        CPUSTR("LOAD_ST=0x"); CPUHEX(iwl_read_prph(UREG_UCODE_LOAD_STATUS)); CPUNL();
        CPUSTR("BA_LSB=0x"); CPUHEX(iwl_read_prph(RFH_Q0_FRBDCB_BA_LSB)); CPUNL();
        CPUSTR("BA_MSB=0x"); CPUHEX(iwl_read_prph(RFH_Q0_FRBDCB_BA_MSB)); CPUNL();
    }

    /* UMAC error table (SRAM 0x800000) — full iwl_umac_error_event_table.
     * Layout (from Linux fw/dump.c):
     *   +00 valid       +04 error_id    +08 blink1      +0c blink2
     *   +10 ilink1      +14 ilink2      +18 data1       +1c data2
     *   +20 data3       +24 umac_major  +28 umac_minor  +2c frame_ptr
     *   +30 stack_ptr   +34 cmd_header  +38 nic_isr_pref
     */
    if (g_iwl_mmio && iwl_grab_nic_access() == 0) {
        uint32_t marker = iwl_read_sram(IWL_UMAC_ERROR_TABLE_BASE);
        if (marker == IWL_UMAC_ERROR_TABLE_VALID) {
            CPUSTR("--- UMAC ERR ---"); CPUNL();
            for (uint32_t i = 0; i < 15; i++) {
                uint32_t v = iwl_read_sram(IWL_UMAC_ERROR_TABLE_BASE + i * 4u);
                CPUSTR("+"); CPUHEX(i * 4u); CPUSTR("=0x"); CPUHEX(v); CPUNL();
            }
        }
    }

done:
#undef CPUSTR
#undef CPUHEX
#undef CPUNL
#undef CPUDEC
    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

/*
 * wh (wifi handoff) — minimal upload/ALIVE handoff summary.
 * Keeps only the signals needed to debug "FW uploaded but no ALIVE".
 */
int wifi_9260_get_handoff_status(char *buf, uint32_t bufsz) {
    if (!buf || bufsz < 192) return -1;
    uint32_t pos = 0;

#define WHSTR(s) do { const char *_s = (s); while (*_s && pos < bufsz - 1) buf[pos++] = *_s++; } while(0)
#define WHHEX(val) do { \
    uint32_t _v = (val); \
    for (int _s = 28; _s >= 0; _s -= 4) \
        if (pos < bufsz - 1) buf[pos++] = "0123456789abcdef"[(_v >> _s) & 0xF]; \
} while(0)
#define WHDEC(v) do { \
    uint32_t _v = (v); char _t[12]; int _i = 0; \
    if (_v == 0) { _t[_i++] = '0'; } \
    else { while (_v) { _t[_i++] = (char)('0' + _v % 10); _v /= 10; } } \
    for (int _j = _i - 1; _j >= 0 && pos < bufsz - 1; _j--) buf[pos++] = _t[_j]; \
} while(0)
#define WHNL() do { if (pos < bufsz - 1) buf[pos++] = '\n'; } while(0)

    WHSTR("--- WiFi Handoff ---"); WHNL();
    WHSTR("step="); WHDEC(g_iwl_init_step);
    WHSTR(" rc=");
    if (g_iwl_init_rc < 0) { WHSTR("-"); WHDEC((uint32_t)(-(int)g_iwl_init_rc)); }
    else WHDEC((uint32_t)g_iwl_init_rc);
    WHSTR(" irq="); WHDEC(g_iwl_irq_fired); WHNL();

    if (!g_iwl_mmio) {
        WHSTR("MMIO not mapped"); WHNL();
        goto done;
    }

    WHSTR("boot gp=0x"); WHHEX(iwl_read32(CSR_GP_CNTRL));
    WHSTR(" rst=0x"); WHHEX(iwl_read32(CSR_RESET));
    WHSTR(" fh=0x"); WHHEX(iwl_read32(FH_UCODE_LOAD_STATUS));
    WHSTR(" chk=0x"); WHHEX(iwl_read32(CSR_GIO_CHICKEN_BITS));
    WHSTR(" uld=0x");
    if (iwl_grab_nic_access() == 0)
        WHHEX(iwl_read_prph(UREG_UCODE_LOAD_STATUS));
    else
        WHSTR("????????");
    WHNL();
    WHSTR("chk  before=0x"); WHHEX(g_iwl_bootdbg.chicken_before);
    WHSTR(" after=0x"); WHHEX(g_iwl_bootdbg.chicken_after);
    WHSTR(" want=0x"); WHHEX(g_iwl_bootdbg.chicken_want); WHNL();
    if (g_iwl_bootdbg.fw_sec_total) {
        WHSTR("fw   img="); WHSTR(g_iwl_bootdbg.fw_upload_is_init ? "init" : "rt");
        WHSTR(" sec="); WHDEC(g_iwl_bootdbg.fw_sec_total); WHNL();
        WHSTR("fwup c1=");
        WHDEC(g_iwl_bootdbg.fw_cpu1.start_idx);
        WHSTR("/"); WHDEC(g_iwl_bootdbg.fw_cpu1.loaded);
        WHSTR("/"); WHDEC(g_iwl_bootdbg.fw_cpu1.break_idx);
        WHSTR(" off=0x"); WHHEX(g_iwl_bootdbg.fw_cpu1.break_off);
        WHSTR(" c2=");
        WHDEC(g_iwl_bootdbg.fw_cpu2.start_idx);
        WHSTR("/"); WHDEC(g_iwl_bootdbg.fw_cpu2.loaded);
        WHSTR("/"); WHDEC(g_iwl_bootdbg.fw_cpu2.break_idx);
        WHSTR(" off=0x"); WHHEX(g_iwl_bootdbg.fw_cpu2.break_off);
        WHNL();
    }

    if (iwl_grab_nic_access() == 0) {
        uint32_t cpu1 = iwl_read_prph(SB_CPU_1_STATUS);
        uint32_t cpu2 = iwl_read_prph(SB_CPU_2_STATUS);
        uint32_t umac_a = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t lmac_a = iwl_read_prph(UREG_LMAC1_CURRENT_PC);
        uint32_t rfh_act = iwl_read_prph(RFH_RXF_RXQ_ACTIVE);
        uint32_t rfh_frbd = iwl_read_prph(RFH_Q0_FRBDCB_BA_LSB);
        uint32_t rfh_urbd = iwl_read_prph(RFH_Q0_URBDCB_BA_LSB);
        uint32_t rfh_stts = iwl_read_prph(RFH_Q0_URBD_STTS_WPTR_LSB);
        uint32_t rfh_widx = iwl_read_prph(RFH_Q0_FRBDCB_WIDX);
        uint32_t rfh_ridx = iwl_read_prph(RFH_Q0_FRBDCB_RIDX);
        uint32_t rfh_uwdx = iwl_read_prph(RFH_Q0_URBDCB_WIDX);
        iwl_udelay(10000); /* 10ms */
        uint32_t umac_b = iwl_read_prph(UREG_UMAC_CURRENT_PC);
        uint32_t lmac_b = iwl_read_prph(UREG_LMAC1_CURRENT_PC);

        WHSTR("cpu  c1=0x"); WHHEX(cpu1);
        WHSTR(" c2=0x"); WHHEX(cpu2); WHNL();

        WHSTR("pc   u=0x"); WHHEX(umac_a);
        WHSTR("->0x"); WHHEX(umac_b);
        WHSTR(" l=0x"); WHHEX(lmac_a);
        WHSTR("->0x"); WHHEX(lmac_b); WHNL();

        WHSTR("rfh  act=0x"); WHHEX(rfh_act);
        WHSTR(" w=0x"); WHHEX(rfh_widx);
        WHSTR(" r=0x"); WHHEX(rfh_ridx);
        WHSTR(" u=0x"); WHHEX(rfh_uwdx); WHNL();

        if (g_iwl_dma.allocated) {
            WHSTR("rfhba fr=0x"); WHHEX(rfh_frbd);
            WHSTR("/0x"); WHHEX((uint32_t)g_iwl_dma.rxbd_ring_phys);
            WHSTR(" ur=0x"); WHHEX(rfh_urbd);
            WHSTR("/0x"); WHHEX((uint32_t)g_iwl_dma.rx_used_ring_phys);
            WHSTR(" st=0x"); WHHEX(rfh_stts);
            WHSTR("/0x"); WHHEX((uint32_t)g_iwl_dma.rx_status_phys); WHNL();
        }

        if (g_iwl_dma.allocated && g_iwl_dma.rx_status) {
            iwl_cache_flush(g_iwl_dma.rx_status, sizeof(struct iwl_rx_status));
            WHSTR("rx   cl=0x"); WHHEX(g_iwl_dma.rx_status->closed_rb_num);
            WHSTR(" fin=0x"); WHHEX(g_iwl_dma.rx_status->finished_rb_num); WHNL();
        }

        WHSTR("diag ");
        if (umac_a != umac_b || lmac_a != lmac_b) {
            WHSTR("fw-running");
        } else if (cpu1 == 0 && cpu2 == 0) {
            WHSTR("cpu-never-started");
        } else if (rfh_ridx == 0 && rfh_uwdx == 0) {
            WHSTR("cpu-stuck-no-rx-progress");
        } else if (rfh_ridx != 0 && rfh_uwdx == 0) {
            WHSTR("cpu-stuck-rx-no-used");
        } else {
            WHSTR("cpu-stuck");
        }
        WHNL();
    } else {
        WHSTR("NIC ACCESS FAILED"); WHNL();
    }

done:
#undef WHSTR
#undef WHHEX
#undef WHDEC
#undef WHNL
    if (pos < bufsz) buf[pos] = '\0';
    else buf[bufsz - 1] = '\0';
    return (int)pos;
}

int wifi_9260_get_scan_entries(struct fry_wifi_scan_entry *out,
                               uint32_t max_entries, uint32_t *count_out) {
    if (!out || !count_out) return -1;
    if (!g_iwl_ready) return -2;

    struct iwl_bss_entry scan_results[IWL_MAX_SCAN_RESULTS];
    uint32_t count = 0;
    int rc = iwl_scan(scan_results, IWL_MAX_SCAN_RESULTS, &count);
    if (rc != 0) return rc;

    if (count > max_entries) count = max_entries;
    if (count > FRY_WIFI_MAX_SCAN) count = FRY_WIFI_MAX_SCAN;

    for (uint32_t i = 0; i < count; i++) {
        struct fry_wifi_scan_entry *dst = &out[i];
        iwl_zero(dst, sizeof(*dst));
        iwl_copy(dst->bssid, scan_results[i].bssid, 6);
        iwl_copy_ssid(dst->ssid, scan_results[i].ssid, scan_results[i].ssid_len);
        dst->channel = scan_results[i].channel;
        dst->rssi = scan_results[i].rssi;
        dst->secure = scan_results[i].has_rsn ? 1 : 0;
        dst->connected = 0;
        if (g_iwl_connected) {
            int same_bssid = 1;
            for (int j = 0; j < 6; j++) {
                if (scan_results[i].bssid[j] != g_ap_bssid[j]) {
                    same_bssid = 0;
                    break;
                }
            }
            dst->connected = same_bssid ? 1 : 0;
        }
    }

    *count_out = count;
    return 0;
}

int wifi_9260_connect_user(const char *ssid, const char *passphrase) {
    if (!g_iwl_ready) return -1;
    if (!g_iwl_have_mac || iwl_mac_is_zero(g_our_mac)) return -2;
    if (!ssid || !ssid[0]) return -3;
    if (!passphrase) passphrase = "";

    net_reset_config();

    kprint("IWL9260: userspace connect request for \"%s\"\n", ssid);
    int rc = iwl_connect_wpa2(ssid, passphrase, g_our_mac);
    if (rc != 0) return rc;

    rc = dhcp_discover();
    if (rc != 0) return -10;

    return 0;
}

int wifi_9260_reinit_user(void) {
    kprint("IWL9260: userspace requested soft reset/reinit, but live reinit is disabled until teardown/recovery exists\n");
    return -95;
}

int wifi_9260_boot_smoke_test(void) {
#if !TATER_WIFI_BOOT_TEST_ENABLE
    kprint("IWL9260: boot smoke test disabled "
           "(edit src/drivers/net/wifi_boot_test_config.h to enable)\n");
    return 1;
#else
    const char *ssid = TATER_WIFI_BOOT_TEST_SSID;
    const char *passphrase = TATER_WIFI_BOOT_TEST_PASSPHRASE;
    const char *hostname = TATER_WIFI_BOOT_TEST_HOSTNAME;
    const uint16_t tcp_port = (uint16_t)TATER_WIFI_BOOT_TEST_TCP_PORT;
    char ip_buf[16];
    char gw_buf[16];
    char dns_buf[16];

    if (!g_iwl_ready) {
        kprint("IWL9260: boot smoke test aborted: driver not ready\n");
        return -1;
    }
    if (!g_iwl_have_mac || iwl_mac_is_zero(g_our_mac)) {
        kprint("IWL9260: boot smoke test aborted: NIC MAC unavailable\n");
        return -2;
    }
    if (!ssid[0]) {
        kprint("IWL9260: boot smoke test aborted: SSID is empty\n");
        return -3;
    }

    kprint("IWL9260: boot smoke test starting for SSID \"%s\"\n", ssid);

    int rc = iwl_connect_wpa2(ssid, passphrase, g_our_mac);
    if (rc != 0) {
        kprint("IWL9260: boot smoke test connect failed rc=%d\n", rc);
        return rc;
    }

    rc = dhcp_discover();
    if (rc != 0) {
        kprint("IWL9260: boot smoke test DHCP failed rc=%d\n", rc);
        return -10;
    }

    const struct net_config *cfg = net_get_config();
    net_ip_str(cfg->ip, ip_buf, sizeof(ip_buf));
    net_ip_str(cfg->gateway, gw_buf, sizeof(gw_buf));
    net_ip_str(cfg->dns_server, dns_buf, sizeof(dns_buf));
    kprint("IWL9260: boot smoke test DHCP OK ip=%s gw=%s dns=%s\n",
           ip_buf, gw_buf, dns_buf);

    if (!hostname[0]) {
        kprint("IWL9260: boot smoke test skipping DNS/TCP probe (hostname empty)\n");
        return 0;
    }

    uint32_t host_ip = dns_resolve(hostname);
    if (!host_ip) {
        kprint("IWL9260: boot smoke test DNS failed for \"%s\"\n", hostname);
        return -11;
    }

    net_ip_str(host_ip, ip_buf, sizeof(ip_buf));
    kprint("IWL9260: boot smoke test DNS OK %s -> %s\n", hostname, ip_buf);

    tcp_conn_t conn = tcp_connect(host_ip, tcp_port);
    if (conn < 0) {
        kprint("IWL9260: boot smoke test TCP connect failed host=%s port=%u\n",
               hostname, (unsigned)tcp_port);
        return -12;
    }

    char req[256];
    uint32_t req_len = 0;
    const char *prefix = "GET / HTTP/1.0\r\nHost: ";
    const char *suffix = "\r\nUser-Agent: TaterTOS64v3\r\n\r\n";

    while (prefix[req_len] && req_len + 1 < sizeof(req)) {
        req[req_len] = prefix[req_len];
        req_len++;
    }
    for (uint32_t i = 0; hostname[i] && req_len + 1 < sizeof(req); i++) {
        req[req_len++] = hostname[i];
    }
    for (uint32_t i = 0; suffix[i] && req_len + 1 < sizeof(req); i++) {
        req[req_len++] = suffix[i];
    }
    req[req_len] = '\0';

    int sent = tcp_send(conn, (const uint8_t *)req, (uint16_t)req_len);
    if (sent < 0) {
        kprint("IWL9260: boot smoke test TCP send failed\n");
        tcp_close(conn);
        return -13;
    }

    kprint("IWL9260: boot smoke test sent HTTP probe (%d bytes)\n", sent);

    uint8_t rx_buf[128];
    uint32_t elapsed = 0;
    int got_payload = 0;
    while (elapsed < 3000000u) {
        iwl_rx_poll(0);
        int n = tcp_recv(conn, rx_buf, (uint16_t)(sizeof(rx_buf) - 1));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (rx_buf[i] == '\r' || rx_buf[i] == '\n') {
                    rx_buf[i] = ' ';
                } else if (rx_buf[i] < 32 || rx_buf[i] > 126) {
                    rx_buf[i] = '.';
                }
            }
            rx_buf[n] = 0;
            kprint("IWL9260: boot smoke test received %d bytes: %s\n", n, rx_buf);
            got_payload = 1;
            break;
        }
        iwl_udelay(5000);
        elapsed += 5000;
    }

    tcp_close(conn);
    if (!got_payload) {
        kprint("IWL9260: boot smoke test TCP connected but no payload arrived before timeout\n");
        return -14;
    }

    kprint("IWL9260: boot smoke test PASSED\n");
    return 0;
#endif
}

/* ---- Main init ---- */
static void iwl_init_fail(uint8_t step, int16_t rc) {
    g_iwl_init_step = step;
    g_iwl_init_rc = rc;
}

void wifi_9260_init(void) {
    g_iwl_ready = 0;
    g_iwl_have_mac = 0;
    iwl_phy_db_reset();
    g_iwl_init_step = WIFI_STEP_NONE;
    g_iwl_init_rc = 0;
    g_iwl_dbg_pos = 0;
    if (g_iwl_dbg_buf) { kfree(g_iwl_dbg_buf); g_iwl_dbg_buf = 0; }
    g_iwl_dbg_buf = (char *)kmalloc(IWL_DBG_BUF_INIT);
    g_iwl_dbg_cap = g_iwl_dbg_buf ? IWL_DBG_BUF_INIT : 0;
    if (g_iwl_dbg_buf) g_iwl_dbg_buf[0] = '\0';
    g_iwl_bootdbg.flags = 0;
    g_iwl_bootdbg.rfkill_mask = 0;
    g_iwl_bootdbg.shadow_ctrl = 0;
    g_iwl_bootdbg.secure_cpu1_hdr_readback = 0;
    g_iwl_bootdbg.secure_cpu2_hdr_readback = 0;
    g_iwl_bootdbg.ctxt_info_write_lo = 0;
    g_iwl_bootdbg.ctxt_info_write_hi = 0;
    g_iwl_bootdbg.ctxt_info_read_lo = 0;
    g_iwl_bootdbg.ctxt_info_read_hi = 0;
    g_iwl_bootdbg.cpu_init_run_written = 0;
    g_iwl_bootdbg.cpu_init_run_readback = 0;
    g_iwl_bootdbg.load_int_mask = 0;
    g_iwl_bootdbg.alive_int_mask = 0;
    g_iwl_bootdbg.wfpm_gp2_written = 0;
    g_iwl_bootdbg.wfpm_gp2_readback = 0;
    g_iwl_bootdbg.release_cpu_reset_readback = 0;
    g_iwl_bootdbg.chicken_before = 0;
    g_iwl_bootdbg.chicken_after = 0;
    g_iwl_bootdbg.chicken_want = 0;
    g_iwl_bootdbg.fw_upload_is_init = 0;
    g_iwl_bootdbg.fw_sec_total = 0;
    iwl_zero(&g_iwl_bootdbg.fw_cpu1, sizeof(g_iwl_bootdbg.fw_cpu1));
    iwl_zero(&g_iwl_bootdbg.fw_cpu2, sizeof(g_iwl_bootdbg.fw_cpu2));
    g_iwl_bootdbg.rfh_frbdcb_ba_lsb = 0;
    g_iwl_bootdbg.rfh_urbdcb_ba_lsb = 0;
    g_iwl_bootdbg.rfh_status_wptr_lsb = 0;
    g_iwl_bootdbg.rfh_widx_trg_written = 0;
    g_iwl_bootdbg.fh_load_status_after_upload = 0;
    iwl_reset_sram_probes();
    g_iwl_boot_trace_count = 0;
    g_iwl_cmd_trace_count = 0;
    g_iwl_cmd_trace_next = 0;
    g_iwl_cmd_trace_ord = 0;
    g_iwl_cmd_seq = 0;
    g_iwl_irq_fired = 0;
    g_iwl_last_csr_int = 0;
    g_iwl_last_fh_int = 0;
    iwl_zero(g_our_mac, sizeof(g_our_mac));
    iwl_clear_link_state();
    net_reset_config();

    uint32_t count = 0;
    const struct pci_device_info *devs = pci_get_devices(&count);
    if (!devs || count == 0) {
        iwl_dbg("IWL9260: PCI table empty\n");
        iwl_init_fail(WIFI_STEP_PCI_SCAN, -1);
        return;
    }

    const struct pci_device_info *target = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (iwl_is_target_device(&devs[i])) {
            target = &devs[i];
            break;
        }
    }

    if (!target) {
        iwl_dbg("IWL9260: device 8086:2526 not found\n");
        iwl_init_fail(WIFI_STEP_PCI_SCAN, -2);
        return;
    }

    g_iwl_init_step = WIFI_STEP_PCI_SCAN;

    uint32_t subsys = pci_ecam_read32(0, target->bus, target->slot, target->func, 0x2C);
    iwl_pci_prepare_link(target, 0);

    uint64_t mmio_phys = iwl_bar0_phys(target);
    if (!mmio_phys) {
        iwl_dbg("IWL9260: BAR0 invalid\n");
        iwl_init_fail(WIFI_STEP_PCI_SCAN, -3);
        return;
    }

    vmm_ensure_physmap_uc(mmio_phys + IWL_MMIO_SIZE);
    g_iwl_mmio = (volatile uint8_t *)(uintptr_t)vmm_phys_to_virt(mmio_phys);
    uint32_t probe0 = iwl_read32(0x0);

    if (probe0 == 0xFFFFFFFFu) {
        iwl_dbg("IWL9260: dead MMIO probe, retrying after PCI recovery\n");
        iwl_pci_prepare_link(target, 1);
        vmm_ensure_physmap_uc(mmio_phys + IWL_MMIO_SIZE);
        g_iwl_mmio = (volatile uint8_t *)(uintptr_t)vmm_phys_to_virt(mmio_phys);
        probe0 = iwl_read32(0x0);
    }

    iwl_dbg("IWL9260: found did=%04x subsys=%08x probe0=%08x\n",
            target->device_id, subsys, probe0);
    if (probe0 == 0xFFFFFFFFu) {
        iwl_dbg("IWL9260: MMIO probe still dead after PCI recovery\n");
        iwl_init_fail(WIFI_STEP_PCI_SCAN, -4);
        return;
    }

    /* Load firmware from VFS */
    struct iwlwifi_fw_image fw;
    int fw_rc = iwlwifi_fw_load(&fw);
    if (fw_rc != 0) {
        iwl_dbg("IWL9260: firmware missing rc=%d\n", fw_rc);
        iwl_init_fail(WIFI_STEP_FW_LOAD, (int16_t)fw_rc);
        return;
    }
    g_iwl_init_step = WIFI_STEP_FW_LOAD;

    iwl_dbg("IWL9260: FW loaded size=%u\n", fw.size);

    /* Step 2: TLV Firmware Parsing */
    struct iwl_fw_pieces pieces;
    int rc;
    rc = iwlwifi_fw_parse_tlv(&fw, &pieces);
    if (rc != 0) {
        iwl_dbg("IWL9260: TLV parse FAILED rc=%d\n", rc);
        iwlwifi_fw_release(&fw);
        iwl_init_fail(WIFI_STEP_TLV_PARSE, (int16_t)rc);
        return;
    }
    g_iwl_init_step = WIFI_STEP_TLV_PARSE;

    g_iwl_sec_rt_count = pieces.sec_rt_count;
    g_iwl_fw_phy_cfg = pieces.phy_config;
    iwl_dbg("IWL9260: FW ver=%u.%u.%u.%u SEC_RT=%u ncpus=%u\n",
            pieces.ver_major, pieces.ver_minor, pieces.ver_api, pieces.ver_serial,
            pieces.sec_rt_count, pieces.num_cpus);

    int iommu_rc = iwl_disable_intel_iommu();
    if (iommu_rc < 0)
        iwl_dbg("IWL9260: VT-d disable incomplete, continuing with direct DMA anyway\n");

    rc = -1;
    uint32_t attempt = 0;
    uint8_t failed_step = WIFI_STEP_NONE;
    uint8_t mac[6] = {0};

    if (pieces.sec_init_count > 0) {
        iwl_dbg("IWL9260: starting INIT firmware phase (%u sections)\n",
                pieces.sec_init_count);
        iwl_phy_db_reset();

        for (attempt = 0; attempt < IWL_INIT_FW_BOOT_ATTEMPTS; attempt++) {
            rc = iwl_fw_boot_attempt(target, &pieces, attempt, &failed_step, 1);
            g_iwl_init_step = failed_step;
            if (rc == 0) {
                g_iwl_init_step = WIFI_STEP_HCMD_INIT;
                rc = iwl_prepare_init_phase(mac);
                if (rc == 0)
                    rc = iwl_wait_init_complete();
                if (rc == 0)
                    break;
            }
            if (failed_step != WIFI_STEP_FW_UPLOAD &&
                failed_step != WIFI_STEP_ALIVE &&
                g_iwl_init_step != WIFI_STEP_HCMD_INIT)
                break;
        }
        if (rc != 0) {
            iwlwifi_fw_release(&fw);
            iwl_init_fail(g_iwl_init_step, (int16_t)rc);
            return;
        }
    } else {
        iwl_dbg("IWL9260: no SEC_INIT image present, skipping init-fw phase\n");
    }

    rc = -1;
    failed_step = WIFI_STEP_NONE;
    for (attempt = 0; attempt < IWL_INIT_FW_BOOT_ATTEMPTS; attempt++) {
        iwl_cmd_trace_mark_phase(IWL_CMD_PHASE_RT_BOOT_ENTER, 0,
                                 (uint16_t)(attempt + 1u));
        rc = iwl_fw_boot_attempt(target, &pieces, attempt, &failed_step, 0);
        g_iwl_init_step = failed_step;
        if (rc == 0)
            break;
        if (failed_step != WIFI_STEP_FW_UPLOAD &&
            failed_step != WIFI_STEP_ALIVE)
            break;
    }
    if (rc != 0) {
        iwlwifi_fw_release(&fw);
        iwl_init_fail(g_iwl_init_step, (int16_t)rc);
        return;
    }

    /* Can release firmware image now - it's loaded into device SRAM */
    iwlwifi_fw_release(&fw);

    if (pieces.sec_init_count > 0) {
        rc = iwl_send_phy_db_data();
        if (rc != 0) {
            iwl_dbg("IWL9260: PHY DB send FAILED rc=%d\n", rc);
            iwl_init_fail(WIFI_STEP_HCMD_INIT, (int16_t)rc);
            return;
        }
    }

    /* Step 6: Host Command Interface */
    rc = iwl_hcmd_init(mac, pieces.sec_init_count > 0);
    if (rc != 0) {
        iwl_dbg("IWL9260: hcmd init FAILED rc=%d\n", rc);
        iwl_init_fail(WIFI_STEP_HCMD_INIT, (int16_t)rc);
        return;
    }
    g_iwl_init_step = WIFI_STEP_HCMD_INIT;
    if (!iwl_mac_is_zero(mac)) {
        iwl_copy(g_our_mac, mac, 6);
        g_iwl_have_mac = 1;
        net_set_mac(mac);
        iwl_dbg("IWL9260: MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        iwl_dbg("IWL9260: MAC unavailable\n");
    }

    /* Step 7: 802.11 MAC Layer — init contexts + scan */
    rc = iwl_mac_init(mac);
    if (rc != 0) {
        iwl_dbg("IWL9260: MAC init FAILED rc=%d\n", rc);
        iwl_init_fail(WIFI_STEP_MAC_INIT, (int16_t)rc);
        return;
    }
    g_iwl_init_step = WIFI_STEP_MAC_INIT;

    /* Perform initial scan */
    struct iwl_bss_entry scan_results[IWL_MAX_SCAN_RESULTS];
    uint32_t scan_count = 0;
    rc = iwl_scan(scan_results, IWL_MAX_SCAN_RESULTS, &scan_count);
    if (rc != 0) {
        iwl_dbg("IWL9260: scan FAILED rc=%d\n", rc);
    } else {
        iwl_dbg("IWL9260: scan found %u networks\n", scan_count);
    }
    g_iwl_init_step = WIFI_STEP_DONE;

    g_iwl_ready = 1;
    iwl_dbg("IWL9260: init complete\n");
}
