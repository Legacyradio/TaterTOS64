/*
 * TaterTOS64v3 — Intel I219-LM Ethernet Driver
 *
 * Target: Intel Ethernet Connection (2) I219-LM (PCI 8086:15B7)
 * Dell Precision 7530 on-board LAN
 *
 * Register-compatible with e1000e family but this driver targets only
 * the I219-LM specifically. No shared-driver umbrella.
 *
 * fry721: Initial driver
 */

#include <stdint.h>
#include "../../drivers/pci/pci.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/heap.h"
#include "netcore.h"

/* ---- External kernel API ---- */
void kprint(const char *fmt, ...);

/* ---- PCI IDs ---- */
#define I219_VENDOR_INTEL    0x8086

/* I219 device IDs — e1000e family, multiple silicon revisions */
static const uint16_t i219_device_ids[] = {
    0x15B7,  /* I219-LM 2  (SPT-H) */
    0x15B8,  /* I219-V  2  (SPT-H) */
    0x15B9,  /* I219-LM 3  (SPT-H) */
    0x15BB,  /* I219-LM 6  (CNP-H) — Dell 7530 most likely */
    0x15BC,  /* I219-V  6  (CNP-H) */
    0x15BD,  /* I219-LM 7  (CNP-H) */
    0x15BE,  /* I219-V  7  (CNP-H) */
    0x15D6,  /* I219-V  5  (CNP-LP) */
    0x15D7,  /* I219-LM 4  (CNP-H) */
    0x15D8,  /* I219-V  4  (CNP-H) */
    0x15E3,  /* I219-LM 8  (CNP-H) */
    0x0D4E,  /* I219-LM 11 (ADP) */
    0x0D4F,  /* I219-V  11 (ADP) */
    0x0D53,  /* I219-LM 12 (ADP) */
    0x0D55,  /* I219-V  12 (ADP) */
    0x1A1E,  /* I219-LM 15 (RPL) */
    0x1A1F,  /* I219-V  15 (RPL) */
    0x550A,  /* I219-LM 16 (MTL) */
    0x550B,  /* I219-V  16 (MTL) */
};
#define I219_NUM_DEVICE_IDS (sizeof(i219_device_ids)/sizeof(i219_device_ids[0]))

/* ---- CSR Register Offsets ---- */
#define I219_CTRL      0x00000   /* Device Control */
#define I219_STATUS    0x00008   /* Device Status (RO) */
#define I219_EECD      0x00010   /* EEPROM/Flash Control */
#define I219_EERD      0x00014   /* EEPROM Read */
#define I219_CTRL_EXT  0x00018   /* Extended Device Control */
#define I219_MDIC      0x00020   /* MDI Control (PHY access) */
#define I219_FEXTNVM   0x00028   /* Future Extended NVM */
#define I219_FEXTNVM3  0x0003C   /* Future Extended NVM 3 */
#define I219_FEXTNVM4  0x00024   /* Future Extended NVM 4 */
#define I219_FEXTNVM6  0x00010   /* Future Extended NVM 6 */
#define I219_FEXTNVM11 0x05ABC   /* Future Extended NVM 11 (I219 reset fix) */
#define I219_ICR       0x000C0   /* Interrupt Cause Read (clears on read) */
#define I219_ICS       0x000C8   /* Interrupt Cause Set */
#define I219_IMS       0x000D0   /* Interrupt Mask Set */
#define I219_IMC       0x000D8   /* Interrupt Mask Clear */
#define I219_RCTL      0x00100   /* Receive Control */
#define I219_TCTL      0x00400   /* Transmit Control */
#define I219_TIPG      0x00410   /* TX Inter-Packet Gap */
#define I219_RDBAL     0x02800   /* RX Descriptor Base Low */
#define I219_RDBAH     0x02804   /* RX Descriptor Base High */
#define I219_RDLEN     0x02808   /* RX Descriptor Length (bytes) */
#define I219_RDH       0x02810   /* RX Descriptor Head */
#define I219_RDT       0x02818   /* RX Descriptor Tail */
#define I219_TDBAL     0x03800   /* TX Descriptor Base Low */
#define I219_TDBAH     0x03804   /* TX Descriptor Base High */
#define I219_TDLEN     0x03808   /* TX Descriptor Length (bytes) */
#define I219_TDH       0x03810   /* TX Descriptor Head */
#define I219_TDT       0x03818   /* TX Descriptor Tail */
#define I219_RAL0      0x05400   /* Receive Address Low (MAC addr low 32) */
#define I219_RAH0      0x05404   /* Receive Address High (MAC addr high 16) */
#define I219_MTA_BASE  0x05200   /* Multicast Table Array (128 dwords) */

/* ---- CTRL bits ---- */
#define I219_CTRL_FD       (1u << 0)    /* Full Duplex */
#define I219_CTRL_LRST     (1u << 3)    /* Link Reset */
#define I219_CTRL_ASDE     (1u << 5)    /* Auto-Speed Detection Enable */
#define I219_CTRL_SLU      (1u << 6)    /* Set Link Up */
#define I219_CTRL_RST      (1u << 26)   /* Device Reset (self-clearing) */
#define I219_CTRL_PHY_RST  (1u << 31)   /* PHY Reset */

/* ---- STATUS bits ---- */
#define I219_STATUS_FD     (1u << 0)    /* Full Duplex */
#define I219_STATUS_LU     (1u << 1)    /* Link Up */
#define I219_STATUS_SPEED_MASK  (3u << 6)
#define I219_STATUS_SPEED_10    (0u << 6)
#define I219_STATUS_SPEED_100   (1u << 6)
#define I219_STATUS_SPEED_1000  (2u << 6)

/* ---- ICR / IMS / IMC bits ---- */
#define I219_INT_TXDW      (1u << 0)    /* TX Descriptor Written Back */
#define I219_INT_TXQE      (1u << 2)    /* TX Queue Empty */
#define I219_INT_LSC       (1u << 4)    /* Link Status Change */
#define I219_INT_RXSEQ     (1u << 6)    /* RX Sequence Error */
#define I219_INT_RXT0      (1u << 7)    /* RX Timer (packet received) */
#define I219_INT_RXDMT0    (1u << 8)    /* RX Descriptor Min Threshold */

/* ---- RCTL bits ---- */
#define I219_RCTL_EN       (1u << 1)    /* Receiver Enable */
#define I219_RCTL_SBP      (1u << 2)    /* Store Bad Packets */
#define I219_RCTL_UPE      (1u << 3)    /* Unicast Promiscuous */
#define I219_RCTL_MPE      (1u << 4)    /* Multicast Promiscuous */
#define I219_RCTL_BAM      (1u << 15)   /* Broadcast Accept Mode */
#define I219_RCTL_BSIZE_2K (0u << 16)   /* Buffer Size 2048 */
#define I219_RCTL_BSIZE_1K (1u << 16)   /* Buffer Size 1024 */
#define I219_RCTL_SECRC    (1u << 26)   /* Strip Ethernet CRC */

/* ---- TCTL bits ---- */
#define I219_TCTL_EN       (1u << 1)    /* Transmitter Enable */
#define I219_TCTL_PSP      (1u << 3)    /* Pad Short Packets */
#define I219_TCTL_CT_SHIFT 4            /* Collision Threshold */
#define I219_TCTL_COLD_SHIFT 12         /* Collision Distance */

/* ---- TIPG defaults ---- */
#define I219_TIPG_DEFAULT  0x00602006u  /* IPGT=6, IPGR1=8, IPGR2=6 (copper) */

/* ---- RAH bits ---- */
#define I219_RAH_AV        (1u << 31)   /* Address Valid */

/* ---- FEXTNVM11 bits (I219 reset workaround) ---- */
#define I219_FEXTNVM11_DISABLE_MULR_FIX (1u << 13)

/* ---- Descriptor Ring Configuration ---- */
#define I219_NUM_RX_DESC   256
#define I219_NUM_TX_DESC   256
#define I219_RX_BUF_SIZE   2048

/* ---- Legacy RX Descriptor ---- */
struct i219_rx_desc {
    uint64_t addr;       /* Physical address of receive buffer */
    uint16_t length;     /* Length of received data */
    uint16_t checksum;   /* Packet checksum */
    uint8_t  status;     /* Descriptor status */
    uint8_t  errors;     /* Descriptor errors */
    uint16_t special;    /* VLAN tag */
} __attribute__((packed));

#define I219_RXD_STAT_DD   (1u << 0)   /* Descriptor Done */
#define I219_RXD_STAT_EOP  (1u << 1)   /* End of Packet */

/* ---- Legacy TX Descriptor ---- */
struct i219_tx_desc {
    uint64_t addr;       /* Physical address of transmit buffer */
    uint16_t length;     /* Data length */
    uint8_t  cso;        /* Checksum Offset */
    uint8_t  cmd;        /* Command */
    uint8_t  status;     /* Status (DD set by HW) */
    uint8_t  css;        /* Checksum Start */
    uint16_t special;    /* Special / VLAN */
} __attribute__((packed));

#define I219_TXD_CMD_EOP   (1u << 0)   /* End of Packet */
#define I219_TXD_CMD_IFCS  (1u << 1)   /* Insert FCS/CRC */
#define I219_TXD_CMD_RS    (1u << 3)   /* Report Status */
#define I219_TXD_STAT_DD   (1u << 0)   /* Descriptor Done */

/* ---- Driver State ---- */
static volatile uint8_t *g_i219_mmio;  /* MMIO base virtual address */
static uint64_t g_i219_mmio_phys;      /* MMIO base physical address */

static struct i219_rx_desc *g_rx_ring;  /* RX descriptor ring (virtual) */
static uint64_t g_rx_ring_phys;         /* RX descriptor ring (physical) */
static uint8_t *g_rx_bufs[I219_NUM_RX_DESC];     /* RX buffer virtual addrs */
static uint64_t g_rx_bufs_phys[I219_NUM_RX_DESC]; /* RX buffer physical addrs */
static uint16_t g_rx_tail;              /* Next RX descriptor to check */

static struct i219_tx_desc *g_tx_ring;  /* TX descriptor ring (virtual) */
static uint64_t g_tx_ring_phys;         /* TX descriptor ring (physical) */
static uint8_t *g_tx_bufs[I219_NUM_TX_DESC];     /* TX buffer virtual addrs */
static uint64_t g_tx_bufs_phys[I219_NUM_TX_DESC]; /* TX buffer physical addrs */
static uint16_t g_tx_tail;              /* Next TX descriptor to use */

static uint8_t g_i219_mac[6];          /* MAC address */
static uint8_t g_i219_ready;           /* 1 if driver initialized OK */
static uint8_t g_i219_link_up;         /* 1 if link detected */

/* PCI location (saved for config space access) */
static uint8_t g_i219_bus, g_i219_slot, g_i219_func;

/* ---- Helpers ---- */

static void i219_zero(void *p, uint64_t len) {
    uint8_t *d = (uint8_t *)p;
    for (uint64_t i = 0; i < len; i++) d[i] = 0;
}

static void i219_copy(void *dst, const void *src, uint64_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
}

static void i219_udelay(uint32_t us) {
    for (uint32_t i = 0; i < us; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
}

/* ---- MMIO Register Access ---- */

static uint32_t i219_read(uint32_t reg) {
    return *(volatile uint32_t *)(g_i219_mmio + reg);
}

static void i219_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(g_i219_mmio + reg) = val;
}

/* ---- Cache flush for DMA coherency ---- */
static void i219_cache_flush(const void *addr, uint64_t size) {
    uint64_t a = (uint64_t)addr & ~63ULL;
    uint64_t end = ((uint64_t)addr + size + 63ULL) & ~63ULL;
    while (a < end) {
        __asm__ volatile("clflush (%0)" : : "r"(a) : "memory");
        a += 64;
    }
    __asm__ volatile("mfence" ::: "memory");
}

/* ---- PCI BAR0 Decode (64-bit capable) ---- */

static uint64_t i219_bar0_phys(const struct pci_device_info *dev) {
    if (!dev) return 0;
    uint32_t bar0 = dev->bar0;
    if (bar0 == 0 || (bar0 & 0x1u)) return 0;  /* I/O BAR, not MMIO */

    uint64_t phys = (uint64_t)(bar0 & ~0xFULL);
    uint32_t type = (bar0 >> 1) & 0x3u;
    if (type == 0x2u) {
        /* 64-bit BAR: high 32 bits in BAR1 */
        phys |= ((uint64_t)dev->bar1 << 32);
    }
    return phys;
}

/* ---- PCI Config Space Access ---- */

static void i219_pci_enable_bus_master(void) {
    uint32_t cmd = pci_ecam_read32(0, g_i219_bus, g_i219_slot, g_i219_func, 0x04);
    uint16_t command = (uint16_t)(cmd & 0xFFFF);
    /* Set bits: I/O Space (0), Memory Space (1), Bus Master (2) */
    command |= (1u << 1) | (1u << 2);
    /* Disable INTx (bit 10) since we'll poll */
    command |= (1u << 10);
    pci_ecam_write32(0, g_i219_bus, g_i219_slot, g_i219_func, 0x04,
                     (cmd & 0xFFFF0000u) | command);
    kprint("I219: PCI command = 0x%04x (bus master + memory enabled)\n", command);
}

/* ---- I219 Reset Workaround ---- */

static void i219_reset_workaround(void) {
    /* I219 has a broken reset mechanism.
     * If FEXTNVM11.DISABLE_MULR_FIX is set and TDLEN > 0,
     * we must clear the bit before reset to avoid hanging. */
    uint32_t fextnvm11 = i219_read(I219_FEXTNVM11);
    uint32_t tdlen = i219_read(I219_TDLEN);

    if ((fextnvm11 & I219_FEXTNVM11_DISABLE_MULR_FIX) && tdlen > 0) {
        kprint("I219: applying FEXTNVM11 reset workaround\n");
        i219_write(I219_FEXTNVM11, fextnvm11 & ~I219_FEXTNVM11_DISABLE_MULR_FIX);

        /* Flush TX ring by writing TCTL.EN = 0, then clearing descriptors */
        i219_write(I219_TCTL, i219_read(I219_TCTL) & ~I219_TCTL_EN);
        i219_udelay(10000); /* 10ms */
        i219_write(I219_TDLEN, 0);
    }
}

/* ---- Device Reset ---- */

static int i219_reset(void) {
    /* Apply I219-specific workaround before reset */
    i219_reset_workaround();

    /* Set RST bit — self-clearing */
    uint32_t ctrl = i219_read(I219_CTRL);
    i219_write(I219_CTRL, ctrl | I219_CTRL_RST);

    /* Wait for RST bit to self-clear */
    for (int i = 0; i < 1000; i++) {
        i219_udelay(1000); /* 1ms */
        if (!(i219_read(I219_CTRL) & I219_CTRL_RST)) {
            kprint("I219: reset complete (%d ms)\n", i + 1);
            i219_udelay(10000); /* 10ms post-reset settle */
            return 0;
        }
    }

    kprint("I219: reset TIMEOUT (CTRL still has RST after 1s)\n");
    return -1;
}

/* ---- Disable All Interrupts ---- */

static void i219_disable_interrupts(void) {
    i219_write(I219_IMC, 0xFFFFFFFFu);
    (void)i219_read(I219_ICR); /* clear any pending */
}

/* ---- Read MAC Address ---- */

static void i219_read_mac(void) {
    /* UEFI/BIOS pre-loads MAC into RAL0/RAH0 */
    uint32_t ral = i219_read(I219_RAL0);
    uint32_t rah = i219_read(I219_RAH0);

    g_i219_mac[0] = (uint8_t)(ral);
    g_i219_mac[1] = (uint8_t)(ral >> 8);
    g_i219_mac[2] = (uint8_t)(ral >> 16);
    g_i219_mac[3] = (uint8_t)(ral >> 24);
    g_i219_mac[4] = (uint8_t)(rah);
    g_i219_mac[5] = (uint8_t)(rah >> 8);

    /* Validate — MAC should not be all zeros or all FF */
    int all_zero = 1, all_ff = 1;
    for (int i = 0; i < 6; i++) {
        if (g_i219_mac[i] != 0x00) all_zero = 0;
        if (g_i219_mac[i] != 0xFF) all_ff = 0;
    }

    if (all_zero || all_ff) {
        kprint("I219: WARNING — MAC from RAL/RAH invalid (%02x:%02x:%02x:%02x:%02x:%02x)\n",
               g_i219_mac[0], g_i219_mac[1], g_i219_mac[2],
               g_i219_mac[3], g_i219_mac[4], g_i219_mac[5]);
    } else {
        kprint("I219: MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
               g_i219_mac[0], g_i219_mac[1], g_i219_mac[2],
               g_i219_mac[3], g_i219_mac[4], g_i219_mac[5]);
    }

    /* Ensure AV (Address Valid) bit is set in RAH */
    if (!(rah & I219_RAH_AV)) {
        i219_write(I219_RAH0, rah | I219_RAH_AV);
    }
}

/* ---- Clear Multicast Table ---- */

static void i219_clear_multicast(void) {
    for (int i = 0; i < 128; i++) {
        i219_write(I219_MTA_BASE + (uint32_t)(i * 4), 0);
    }
}

/* ---- Allocate DMA Ring + Buffers ---- */

static int i219_alloc_rx_ring(void) {
    /* RX descriptor ring: 256 * 16 = 4096 bytes = 1 page */
    g_rx_ring_phys = pmm_alloc_page_below(0x100000000ULL); /* below 4GB for DMA */
    if (g_rx_ring_phys == 0) {
        kprint("I219: failed to alloc RX ring page\n");
        return -1;
    }
    g_rx_ring = (struct i219_rx_desc *)vmm_phys_to_virt(g_rx_ring_phys);
    i219_zero(g_rx_ring, 4096);

    /* Allocate RX buffers — each 2KB, packed 2 per page */
    for (int i = 0; i < I219_NUM_RX_DESC; i++) {
        uint64_t buf_phys;
        if ((i & 1) == 0) {
            /* Allocate a new page for every pair of buffers */
            buf_phys = pmm_alloc_page_below(0x100000000ULL);
            if (buf_phys == 0) {
                kprint("I219: failed to alloc RX buffer %d\n", i);
                return -2;
            }
            g_rx_bufs_phys[i] = buf_phys;
            g_rx_bufs[i] = (uint8_t *)vmm_phys_to_virt(buf_phys);
        } else {
            /* Second buffer in same page */
            g_rx_bufs_phys[i] = g_rx_bufs_phys[i - 1] + I219_RX_BUF_SIZE;
            g_rx_bufs[i] = g_rx_bufs[i - 1] + I219_RX_BUF_SIZE;
        }

        /* Point descriptor at its buffer */
        g_rx_ring[i].addr = g_rx_bufs_phys[i];
        g_rx_ring[i].status = 0;
    }

    /* Flush descriptor ring to memory for DMA */
    i219_cache_flush(g_rx_ring, (uint64_t)(I219_NUM_RX_DESC * sizeof(struct i219_rx_desc)));
    return 0;
}

static int i219_alloc_tx_ring(void) {
    /* TX descriptor ring: 256 * 16 = 4096 bytes = 1 page */
    g_tx_ring_phys = pmm_alloc_page_below(0x100000000ULL);
    if (g_tx_ring_phys == 0) {
        kprint("I219: failed to alloc TX ring page\n");
        return -1;
    }
    g_tx_ring = (struct i219_tx_desc *)vmm_phys_to_virt(g_tx_ring_phys);
    i219_zero(g_tx_ring, 4096);

    /* Allocate TX buffers — same 2-per-page scheme */
    for (int i = 0; i < I219_NUM_TX_DESC; i++) {
        uint64_t buf_phys;
        if ((i & 1) == 0) {
            buf_phys = pmm_alloc_page_below(0x100000000ULL);
            if (buf_phys == 0) {
                kprint("I219: failed to alloc TX buffer %d\n", i);
                return -2;
            }
            g_tx_bufs_phys[i] = buf_phys;
            g_tx_bufs[i] = (uint8_t *)vmm_phys_to_virt(buf_phys);
        } else {
            g_tx_bufs_phys[i] = g_tx_bufs_phys[i - 1] + I219_RX_BUF_SIZE;
            g_tx_bufs[i] = g_tx_bufs[i - 1] + I219_RX_BUF_SIZE;
        }

        /* TX descriptors start empty */
        g_tx_ring[i].addr = g_tx_bufs_phys[i];
        g_tx_ring[i].cmd = 0;
        g_tx_ring[i].status = I219_TXD_STAT_DD; /* Mark as "done" (available) */
    }

    i219_cache_flush(g_tx_ring, (uint64_t)(I219_NUM_TX_DESC * sizeof(struct i219_tx_desc)));
    return 0;
}

/* ---- Configure RX ---- */

static void i219_setup_rx(void) {
    /* Program descriptor ring address */
    i219_write(I219_RDBAL, (uint32_t)(g_rx_ring_phys & 0xFFFFFFFFu));
    i219_write(I219_RDBAH, (uint32_t)(g_rx_ring_phys >> 32));
    i219_write(I219_RDLEN, (uint32_t)(I219_NUM_RX_DESC * sizeof(struct i219_rx_desc)));

    /* Head = 0, Tail = N-1 (all descriptors available to HW) */
    i219_write(I219_RDH, 0);
    i219_write(I219_RDT, I219_NUM_RX_DESC - 1);
    g_rx_tail = 0;

    /* Enable receiver: accept broadcast, 2KB buffers, strip CRC */
    i219_write(I219_RCTL,
               I219_RCTL_EN |
               I219_RCTL_BAM |
               I219_RCTL_BSIZE_2K |
               I219_RCTL_SECRC);

    kprint("I219: RX ring at phys 0x%08x%08x, %u descriptors\n",
           (uint32_t)(g_rx_ring_phys >> 32), (uint32_t)g_rx_ring_phys,
           I219_NUM_RX_DESC);
}

/* ---- Configure TX ---- */

static void i219_setup_tx(void) {
    /* Program descriptor ring address */
    i219_write(I219_TDBAL, (uint32_t)(g_tx_ring_phys & 0xFFFFFFFFu));
    i219_write(I219_TDBAH, (uint32_t)(g_tx_ring_phys >> 32));
    i219_write(I219_TDLEN, (uint32_t)(I219_NUM_TX_DESC * sizeof(struct i219_tx_desc)));

    /* Head = 0, Tail = 0 (ring empty) */
    i219_write(I219_TDH, 0);
    i219_write(I219_TDT, 0);
    g_tx_tail = 0;

    /* Set TX inter-packet gap */
    i219_write(I219_TIPG, I219_TIPG_DEFAULT);

    /* Enable transmitter: pad short packets, collision thresholds */
    i219_write(I219_TCTL,
               I219_TCTL_EN |
               I219_TCTL_PSP |
               (0x0Fu << I219_TCTL_CT_SHIFT) |    /* Collision Threshold = 15 */
               (0x40u << I219_TCTL_COLD_SHIFT));   /* Collision Distance = 64 */

    kprint("I219: TX ring at phys 0x%08x%08x, %u descriptors\n",
           (uint32_t)(g_tx_ring_phys >> 32), (uint32_t)g_tx_ring_phys,
           I219_NUM_TX_DESC);
}

/* ---- Link Status ---- */

static void i219_check_link(void) {
    uint32_t status = i219_read(I219_STATUS);
    g_i219_link_up = (status & I219_STATUS_LU) ? 1 : 0;

    if (g_i219_link_up) {
        const char *speed_str = "???";
        uint32_t speed = status & I219_STATUS_SPEED_MASK;
        if (speed == I219_STATUS_SPEED_10)   speed_str = "10";
        if (speed == I219_STATUS_SPEED_100)  speed_str = "100";
        if (speed == I219_STATUS_SPEED_1000) speed_str = "1000";

        kprint("I219: link UP — %s Mbps %s duplex\n",
               speed_str,
               (status & I219_STATUS_FD) ? "full" : "half");
    } else {
        kprint("I219: link DOWN\n");
    }
}

/* ---- TX: Send an Ethernet Frame ---- */

int i219_tx_packet(const uint8_t *frame, uint16_t len) {
    if (!g_i219_ready || !frame || len == 0 || len > 1518) return -1;

    /* Check if next descriptor is available */
    i219_cache_flush(&g_tx_ring[g_tx_tail], sizeof(struct i219_tx_desc));
    if (!(g_tx_ring[g_tx_tail].status & I219_TXD_STAT_DD)) {
        /* Ring full — descriptor not yet reclaimed */
        return -2;
    }

    /* Copy frame into TX buffer */
    i219_copy(g_tx_bufs[g_tx_tail], frame, len);
    i219_cache_flush(g_tx_bufs[g_tx_tail], len);

    /* Fill descriptor */
    g_tx_ring[g_tx_tail].addr = g_tx_bufs_phys[g_tx_tail];
    g_tx_ring[g_tx_tail].length = len;
    g_tx_ring[g_tx_tail].cmd = I219_TXD_CMD_EOP | I219_TXD_CMD_IFCS | I219_TXD_CMD_RS;
    g_tx_ring[g_tx_tail].status = 0; /* Clear DD — HW will set when done */
    g_tx_ring[g_tx_tail].cso = 0;
    g_tx_ring[g_tx_tail].css = 0;
    g_tx_ring[g_tx_tail].special = 0;

    i219_cache_flush(&g_tx_ring[g_tx_tail], sizeof(struct i219_tx_desc));

    /* Advance tail — tells NIC there's a new frame to send */
    g_tx_tail = (g_tx_tail + 1) % I219_NUM_TX_DESC;
    i219_write(I219_TDT, g_tx_tail);

    return 0;
}

/* ---- RX: Poll for Received Frames ---- */

/* Callback from netcore registration */
static void (*g_i219_rx_callback)(const uint8_t *data, uint16_t len);

void i219_set_rx_callback(void (*cb)(const uint8_t *data, uint16_t len)) {
    g_i219_rx_callback = cb;
}

int i219_rx_poll(uint32_t timeout_ms) {
    if (!g_i219_ready) return -1;

    int received = 0;
    uint32_t elapsed = 0;

    do {
        i219_cache_flush(&g_rx_ring[g_rx_tail], sizeof(struct i219_rx_desc));

        while (g_rx_ring[g_rx_tail].status & I219_RXD_STAT_DD) {
            uint16_t pkt_len = g_rx_ring[g_rx_tail].length;
            uint8_t pkt_status = g_rx_ring[g_rx_tail].status;
            uint8_t pkt_errors = g_rx_ring[g_rx_tail].errors;

            if ((pkt_status & I219_RXD_STAT_EOP) && pkt_errors == 0 && pkt_len > 0) {
                /* Valid complete frame — deliver to callback */
                i219_cache_flush(g_rx_bufs[g_rx_tail], pkt_len);
                if (g_i219_rx_callback) {
                    g_i219_rx_callback(g_rx_bufs[g_rx_tail], pkt_len);
                }
                received++;
            }

            /* Return descriptor to HW */
            g_rx_ring[g_rx_tail].status = 0;
            g_rx_ring[g_rx_tail].errors = 0;
            g_rx_ring[g_rx_tail].length = 0;
            i219_cache_flush(&g_rx_ring[g_rx_tail], sizeof(struct i219_rx_desc));

            /* Advance RDT so HW can reuse this descriptor */
            uint16_t old_tail = g_rx_tail;
            g_rx_tail = (g_rx_tail + 1) % I219_NUM_RX_DESC;
            i219_write(I219_RDT, old_tail);

            /* Check next descriptor */
            i219_cache_flush(&g_rx_ring[g_rx_tail], sizeof(struct i219_rx_desc));
        }

        if (received > 0 || timeout_ms == 0) break;

        i219_udelay(1000); /* 1ms */
        elapsed++;
    } while (elapsed < timeout_ms);

    return received;
}

/* ---- Public API: Check if NIC is ready ---- */

int i219_is_ready(void) {
    return g_i219_ready ? 1 : 0;
}

int i219_is_link_up(void) {
    if (!g_i219_ready) return 0;
    uint32_t status = i219_read(I219_STATUS);
    return (status & I219_STATUS_LU) ? 1 : 0;
}

const uint8_t *i219_get_mac(void) {
    return g_i219_mac;
}

/* ---- Diagnostic Dump (shell command "eth") ---- */

/* Simple string builder for diag output */
static int d_pos;
static char *d_buf;
static int d_max;

static void D_STR(const char *s) {
    while (*s && d_pos < d_max - 1) d_buf[d_pos++] = *s++;
}
static void D_NL(void) { if (d_pos < d_max - 1) d_buf[d_pos++] = '\n'; }
static void D_HEX(uint32_t v) {
    char tmp[9];
    for (int i = 7; i >= 0; i--) {
        int nib = v & 0xF;
        tmp[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        v >>= 4;
    }
    tmp[8] = 0;
    D_STR(tmp);
}
static void D_DEC(uint32_t v) {
    char tmp[12];
    int p = 0;
    if (v == 0) { tmp[p++] = '0'; }
    else { while (v) { tmp[p++] = '0' + (char)(v % 10); v /= 10; } }
    for (int i = p - 1; i >= 0; i--) { if (d_pos < d_max - 1) d_buf[d_pos++] = tmp[i]; }
}
static void D_MAC(const uint8_t *m) {
    for (int i = 0; i < 6; i++) {
        int hi = (m[i] >> 4) & 0xF, lo = m[i] & 0xF;
        if (d_pos < d_max - 2) {
            d_buf[d_pos++] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
            d_buf[d_pos++] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
        }
        if (i < 5 && d_pos < d_max - 1) d_buf[d_pos++] = ':';
    }
}

int i219_get_diag(char *buf, uint32_t bufsz) {
    d_buf = buf; d_max = (int)bufsz; d_pos = 0;

    D_STR("--- I219 Ethernet Diag ---"); D_NL();

    if (!g_i219_mmio) {
        D_STR("NOT INITIALIZED (MMIO not mapped)"); D_NL();
        d_buf[d_pos] = 0;
        return d_pos;
    }

    /* Basic state */
    D_STR("ready="); D_DEC(g_i219_ready);
    D_STR(" link="); D_DEC(g_i219_link_up); D_NL();

    D_STR("pci="); D_DEC(g_i219_bus); D_STR(":"); D_DEC(g_i219_slot);
    D_STR("."); D_DEC(g_i219_func); D_NL();

    D_STR("mmio=0x"); D_HEX((uint32_t)(g_i219_mmio_phys >> 32));
    D_HEX((uint32_t)g_i219_mmio_phys); D_NL();

    D_STR("mac="); D_MAC(g_i219_mac); D_NL();

    /* Key registers */
    uint32_t ctrl = i219_read(I219_CTRL);
    uint32_t status = i219_read(I219_STATUS);
    uint32_t rctl = i219_read(I219_RCTL);
    uint32_t tctl = i219_read(I219_TCTL);
    uint32_t icr = i219_read(I219_ICR);
    uint32_t ims = i219_read(I219_IMS);

    D_STR("--- REGISTERS ---"); D_NL();
    D_STR("CTRL=0x"); D_HEX(ctrl); D_NL();
    D_STR("STATUS=0x"); D_HEX(status);

    /* Decode STATUS */
    D_STR(" (");
    if (status & I219_STATUS_LU) D_STR("LINK_UP "); else D_STR("LINK_DOWN ");
    if (status & I219_STATUS_FD) D_STR("FD "); else D_STR("HD ");
    uint32_t speed = status & I219_STATUS_SPEED_MASK;
    if (speed == I219_STATUS_SPEED_10) D_STR("10M");
    else if (speed == I219_STATUS_SPEED_100) D_STR("100M");
    else if (speed == I219_STATUS_SPEED_1000) D_STR("1000M");
    else D_STR("???M");
    D_STR(")"); D_NL();

    D_STR("RCTL=0x"); D_HEX(rctl);
    if (rctl & I219_RCTL_EN) D_STR(" EN"); else D_STR(" DIS");
    D_NL();

    D_STR("TCTL=0x"); D_HEX(tctl);
    if (tctl & I219_TCTL_EN) D_STR(" EN"); else D_STR(" DIS");
    D_NL();

    D_STR("ICR=0x"); D_HEX(icr);
    D_STR(" IMS=0x"); D_HEX(ims); D_NL();

    /* RAL/RAH */
    uint32_t ral = i219_read(I219_RAL0);
    uint32_t rah = i219_read(I219_RAH0);
    D_STR("RAL=0x"); D_HEX(ral);
    D_STR(" RAH=0x"); D_HEX(rah);
    if (rah & I219_RAH_AV) D_STR(" AV"); else D_STR(" !AV");
    D_NL();

    /* RX ring state */
    D_STR("--- RX RING ---"); D_NL();
    uint32_t rdh = i219_read(I219_RDH);
    uint32_t rdt = i219_read(I219_RDT);
    uint32_t rdlen = i219_read(I219_RDLEN);
    D_STR("RDH="); D_DEC(rdh);
    D_STR(" RDT="); D_DEC(rdt);
    D_STR(" RDLEN="); D_DEC(rdlen);
    D_STR(" sw_tail="); D_DEC(g_rx_tail); D_NL();

    D_STR("RDBAL=0x"); D_HEX(i219_read(I219_RDBAL));
    D_STR(" RDBAH=0x"); D_HEX(i219_read(I219_RDBAH)); D_NL();

    /* Show first few RX descriptors status */
    if (g_rx_ring) {
        D_STR("RX desc[0..3] status: ");
        for (int i = 0; i < 4 && i < I219_NUM_RX_DESC; i++) {
            i219_cache_flush(&g_rx_ring[i], sizeof(struct i219_rx_desc));
            D_STR("0x"); D_HEX((uint32_t)g_rx_ring[i].status);
            D_STR(" ");
        }
        D_NL();
    }

    /* TX ring state */
    D_STR("--- TX RING ---"); D_NL();
    uint32_t tdh = i219_read(I219_TDH);
    uint32_t tdt = i219_read(I219_TDT);
    uint32_t tdlen = i219_read(I219_TDLEN);
    D_STR("TDH="); D_DEC(tdh);
    D_STR(" TDT="); D_DEC(tdt);
    D_STR(" TDLEN="); D_DEC(tdlen);
    D_STR(" sw_tail="); D_DEC(g_tx_tail); D_NL();

    D_STR("TDBAL=0x"); D_HEX(i219_read(I219_TDBAL));
    D_STR(" TDBAH=0x"); D_HEX(i219_read(I219_TDBAH)); D_NL();

    /* Extra registers for debug */
    D_STR("--- EXTRA ---"); D_NL();
    D_STR("EECD=0x"); D_HEX(i219_read(I219_EECD)); D_NL();
    D_STR("CTRL_EXT=0x"); D_HEX(i219_read(I219_CTRL_EXT)); D_NL();
    D_STR("TIPG=0x"); D_HEX(i219_read(I219_TIPG)); D_NL();
    D_STR("FEXTNVM11=0x"); D_HEX(i219_read(I219_FEXTNVM11)); D_NL();

    /* PCI command register */
    uint32_t pci_cmd = pci_ecam_read32(0, g_i219_bus, g_i219_slot, g_i219_func, 0x04);
    D_STR("PCI_CMD=0x"); D_HEX(pci_cmd & 0xFFFF);
    D_STR(" PCI_STATUS=0x"); D_HEX((pci_cmd >> 16) & 0xFFFF); D_NL();

    d_buf[d_pos] = 0;
    return d_pos;
}

/* ---- Initialization ---- */

void i219_init(void) {
    kprint("I219: scanning PCI for Intel I219 family...\n");

    /* Find the I219 on the PCI bus */
    uint32_t dev_count = 0;
    const struct pci_device_info *devs = pci_get_devices(&dev_count);
    const struct pci_device_info *found = 0;

    for (uint32_t i = 0; i < dev_count; i++) {
        if (devs[i].vendor_id != I219_VENDOR_INTEL) continue;
        for (uint32_t j = 0; j < I219_NUM_DEVICE_IDS; j++) {
            if (devs[i].device_id == i219_device_ids[j]) {
                found = &devs[i];
                break;
            }
        }
        if (found) break;
    }

    /* Fallback: match any Intel Ethernet controller (class 02:00) */
    if (!found) {
        for (uint32_t i = 0; i < dev_count; i++) {
            if (devs[i].vendor_id == I219_VENDOR_INTEL &&
                devs[i].class_code == 0x02 && devs[i].subclass == 0x00) {
                found = &devs[i];
                kprint("I219: matched by class 02:00 (device_id=0x%04x)\n",
                       found->device_id);
                break;
            }
        }
    }

    if (!found) {
        kprint("I219: not found on PCI bus\n");
        return;
    }

    kprint("I219: found at PCI %02x:%02x.%x (device_id=0x%04x class=%02x:%02x)\n",
           found->bus, found->slot, found->func,
           found->device_id, found->class_code, found->subclass);

    g_i219_bus = found->bus;
    g_i219_slot = found->slot;
    g_i219_func = found->func;

    /* Decode BAR0 */
    g_i219_mmio_phys = i219_bar0_phys(found);
    if (g_i219_mmio_phys == 0) {
        kprint("I219: BAR0 invalid\n");
        return;
    }
    /* Map the MMIO region into the physmap with uncacheable flags.
     * vmm_phys_to_virt just does arithmetic — without this call the
     * page tables won't cover the BAR region and we'd page-fault.
     * I219 BAR0 is ~128KB; ensure both 2MB pages are mapped in case
     * the BAR straddles a 2MB boundary. */
    vmm_ensure_physmap_uc(g_i219_mmio_phys + 1);
    vmm_ensure_physmap_uc(g_i219_mmio_phys + 0x20000ULL);
    g_i219_mmio = (volatile uint8_t *)vmm_phys_to_virt(g_i219_mmio_phys);
    kprint("I219: MMIO at phys 0x%08x%08x -> virt 0x%016llx\n",
           (uint32_t)(g_i219_mmio_phys >> 32), (uint32_t)g_i219_mmio_phys,
           (unsigned long long)(uint64_t)g_i219_mmio);

    /* Enable PCI bus mastering + memory space */
    i219_pci_enable_bus_master();

    /* Disable interrupts before reset */
    i219_disable_interrupts();

    /* Reset device */
    if (i219_reset() != 0) {
        kprint("I219: reset failed, aborting\n");
        return;
    }

    /* Disable interrupts again after reset (reset clears IMC) */
    i219_disable_interrupts();

    /* Read MAC address (BIOS pre-loads it into RAL/RAH) */
    i219_read_mac();

    /* Clear multicast table */
    i219_clear_multicast();

    /* Allocate DMA rings + buffers */
    if (i219_alloc_rx_ring() != 0) {
        kprint("I219: RX ring allocation failed\n");
        return;
    }
    if (i219_alloc_tx_ring() != 0) {
        kprint("I219: TX ring allocation failed\n");
        return;
    }

    /* Configure RX and TX */
    i219_setup_rx();
    i219_setup_tx();

    /* Set link up (auto-speed detect + set link up) */
    uint32_t ctrl = i219_read(I219_CTRL);
    ctrl |= I219_CTRL_SLU | I219_CTRL_ASDE;
    ctrl &= ~(I219_CTRL_LRST | I219_CTRL_PHY_RST);
    i219_write(I219_CTRL, ctrl);

    /* Wait for link (up to 5 seconds) */
    kprint("I219: waiting for link...\n");
    for (int i = 0; i < 50; i++) {
        i219_udelay(100000); /* 100ms */
        uint32_t status = i219_read(I219_STATUS);
        if (status & I219_STATUS_LU) {
            break;
        }
    }
    i219_check_link();

    /* Enable key interrupts: RXT0 (rx), TXDW (tx done), LSC (link change) */
    i219_write(I219_IMS, I219_INT_RXT0 | I219_INT_TXDW | I219_INT_LSC);

    g_i219_ready = 1;

    /* Register with netcore */
    net_set_mac(g_i219_mac);

    /* Attach RX callback so netcore receives frames from us */
    void netcore_attach_i219(void);
    netcore_attach_i219();

    kprint("I219: driver initialized successfully\n");
}
