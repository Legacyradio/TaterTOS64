/*
 * e1000.c — Intel 82540EM Gigabit Ethernet driver for TaterTOS64v3
 *
 * Targets the QEMU virtual e1000 NIC (PCI 8086:100E).
 * Polled mode (no IRQ), legacy TX/RX descriptors, 8 KB buffers.
 */

#include <stdint.h>
#include <stddef.h>

/* Kernel APIs */
void kprint(const char *fmt, ...);
uint64_t vmm_phys_to_virt(uint64_t phys);
void vmm_ensure_physmap_uc(uint64_t phys_end);
uint64_t pmm_alloc_page_below(uint64_t limit);

/* PCI — use the canonical header */
#include "../../drivers/pci/pci.h"
static volatile uint32_t e1000_rx_busy;

/* Netcore callback type */
typedef void (*rx_callback_t)(const uint8_t *data, uint16_t len);

/* ================================================================== */
/* Register offsets                                                     */
/* ================================================================== */

#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EERD      0x0014
#define E1000_ICR       0x00C0
#define E1000_IMS       0x00D0
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_TIPG      0x0410
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_MTA       0x5200
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404

/* CTRL bits */
#define CTRL_FD         (1u << 0)
#define CTRL_ASDE       (1u << 5)
#define CTRL_SLU        (1u << 6)
#define CTRL_RST        (1u << 26)

/* RCTL bits */
#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)
#define RCTL_BSIZE_8K   (2u << 16)
#define RCTL_BSEX       (1u << 25)
#define RCTL_SECRC      (1u << 26)

/* TCTL bits */
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)
#define TCTL_CT_SHIFT   4
#define TCTL_COLD_SHIFT 12

/* TX descriptor CMD bits */
#define TXD_CMD_EOP     (1u << 0)
#define TXD_CMD_IFCS    (1u << 1)
#define TXD_CMD_RS      (1u << 3)

/* Descriptor status bits */
#define DESC_DD         (1u << 0)
#define DESC_EOP        (1u << 1)

/* ================================================================== */
/* Descriptors                                                         */
/* ================================================================== */

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* ================================================================== */
/* Driver state                                                        */
/* ================================================================== */

#define NUM_RX_DESC  32
#define NUM_TX_DESC  32
#define BUF_SIZE     8192

static volatile uint8_t *g_mmio = NULL;
static uint8_t g_mac[6];
static int g_ready = 0;

/* RX ring */
static struct e1000_rx_desc *g_rx_ring;
static uint64_t g_rx_ring_phys;
static uint8_t *g_rx_bufs[NUM_RX_DESC];
static uint64_t g_rx_bufs_phys[NUM_RX_DESC];
static uint32_t g_rx_tail;

/* TX ring */
static struct e1000_tx_desc *g_tx_ring;
static uint64_t g_tx_ring_phys;
static uint8_t *g_tx_bufs[NUM_TX_DESC];
static uint64_t g_tx_bufs_phys[NUM_TX_DESC];
static uint32_t g_tx_tail;

/* RX callback */
static rx_callback_t g_rx_cb = NULL;

/* ================================================================== */
/* MMIO helpers                                                        */
/* ================================================================== */

static inline void e_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(g_mmio + reg) = val;
}

static inline uint32_t e_read(uint32_t reg) {
    return *(volatile uint32_t *)(g_mmio + reg);
}

/* ================================================================== */
/* EEPROM read — 82540EM style                                         */
/* ================================================================== */

static uint16_t eeprom_read(uint8_t addr) {
    uint32_t val = ((uint32_t)addr << 8) | 0x01u; /* addr | START */
    e_write(E1000_EERD, val);

    /* Poll DONE (bit 4) */
    int timeout = 100000;
    while (!(e_read(E1000_EERD) & (1u << 4))) {
        if (--timeout <= 0) return 0;
    }
    return (uint16_t)(e_read(E1000_EERD) >> 16);
}

/* ================================================================== */
/* DMA buffer allocation                                               */
/* ================================================================== */

static void *alloc_dma(uint64_t *phys_out, uint32_t size) {
    /* Allocate pages below 4 GB for DMA */
    uint32_t pages = (size + 4095) / 4096;
    uint64_t phys = 0;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t p = pmm_alloc_page_below(0x100000000ULL);
        if (!p) return NULL;
        if (i == 0) phys = p;
    }
    *phys_out = phys;
    return (void *)vmm_phys_to_virt(phys);
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

int e1000_is_ready(void) {
    return g_ready;
}

void e1000_set_rx_callback(void (*cb)(const uint8_t *data, uint16_t len)) {
    g_rx_cb = cb;
}

int e1000_tx_packet(const uint8_t *frame, uint16_t len) {
    if (!g_ready || !frame || len == 0 || len > BUF_SIZE) return -1;

    struct e1000_tx_desc *desc = &g_tx_ring[g_tx_tail];

    /* Wait for descriptor to become available */
    int timeout = 100000;
    while (!(desc->status & DESC_DD)) {
        if (--timeout <= 0) return -2; /* ring full */
    }

    /* Copy frame to DMA buffer */
    uint8_t *buf = g_tx_bufs[g_tx_tail];
    for (uint16_t i = 0; i < len; i++) buf[i] = frame[i];

    /* Fill descriptor */
    desc->addr = g_tx_bufs_phys[g_tx_tail];
    desc->length = len;
    desc->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    desc->status = 0;
    desc->cso = 0;
    desc->css = 0;
    desc->special = 0;

    /* Advance tail — kicks the NIC */
    g_tx_tail = (g_tx_tail + 1) % NUM_TX_DESC;
    e_write(E1000_TDT, g_tx_tail);

    {
        static int tx_trace_count;
        if (tx_trace_count < 20) {
            void kprint_serial_only(const char *fmt, ...);
            kprint_serial_only("E1000_TX: len=%u tail=%u\n", len, g_tx_tail);
            tx_trace_count++;
        }
    }

    return 0;
}

int e1000_rx_poll(uint32_t timeout_ms) {
    (void)timeout_ms;
    int count = 0;

    /* Atomic test-and-set to prevent SMP races on the RX ring */
    if (__sync_lock_test_and_set(&e1000_rx_busy, 1))
        return 0; /* Another CPU is already polling */

    while (g_rx_ring[g_rx_tail].status & DESC_DD) {
        uint16_t len = g_rx_ring[g_rx_tail].length;
        uint8_t  err = g_rx_ring[g_rx_tail].errors;
        uint8_t  eop = g_rx_ring[g_rx_tail].status & DESC_EOP;

        if (eop && !err && len > 0 && g_rx_cb) {
            g_rx_cb(g_rx_bufs[g_rx_tail], len);
            count++;
        }

        /* Return descriptor to hardware */
        g_rx_ring[g_rx_tail].status = 0;
        g_rx_ring[g_rx_tail].errors = 0;
        g_rx_ring[g_rx_tail].length = 0;

        uint32_t old = g_rx_tail;
        g_rx_tail = (g_rx_tail + 1) % NUM_RX_DESC;
        e_write(E1000_RDT, old);
    }

    __sync_lock_release(&e1000_rx_busy);
    return count;
}

const uint8_t *e1000_get_mac(void) {
    return g_mac;
}

/* ================================================================== */
/* Initialization                                                      */
/* ================================================================== */

void e1000_init(void) {
    /* ---- PCI probe ---- */
    uint32_t dev_count = 0;
    const struct pci_device_info *devs = pci_get_devices(&dev_count);
    const struct pci_device_info *nic = NULL;

    for (uint32_t i = 0; i < dev_count; i++) {
        if (devs[i].vendor_id == 0x8086 &&
            (devs[i].device_id == 0x100E ||   /* 82540EM (QEMU default) */
             devs[i].device_id == 0x100C ||   /* 82544GC */
             devs[i].device_id == 0x100F)) {  /* 82545EM */
            nic = &devs[i];
            break;
        }
    }

    if (!nic) {
        kprint("E1000: not found (scanned %u PCI devs)\n", dev_count);
        return;
    }

    kprint("E1000: found PCI %02x:%02x.%x (device %04x) bar0=0x%08x bar1=0x%08x\n",
           nic->bus, nic->slot, nic->func, nic->device_id,
           nic->bar0, nic->bar1);

    /* ---- Map MMIO BAR0 ---- */
    uint64_t bar0_phys = (uint64_t)(nic->bar0 & ~0xFu);
    uint32_t bar_type = (nic->bar0 >> 1) & 0x3u;
    if (bar_type == 0x2u)
        bar0_phys |= ((uint64_t)nic->bar1 << 32);

    if (bar0_phys == 0) {
        kprint("E1000: BAR0 is zero, cannot init\n");
        return;
    }

    vmm_ensure_physmap_uc(bar0_phys + 0x20000);
    g_mmio = (volatile uint8_t *)vmm_phys_to_virt(bar0_phys);

    /* Enable PCI bus mastering + memory space */
    uint32_t pci_cmd = pci_ecam_read32(0, nic->bus, nic->slot, nic->func, 0x04);
    pci_cmd |= (1u << 1) | (1u << 2); /* Memory Space + Bus Master */
    pci_ecam_write32(0, nic->bus, nic->slot, nic->func, 0x04, pci_cmd);

    /* ---- Reset ---- */
    uint32_t ctrl = e_read(E1000_CTRL);
    ctrl |= CTRL_RST;
    e_write(E1000_CTRL, ctrl);

    /* Wait for reset to self-clear */
    int timeout = 100000;
    while ((e_read(E1000_CTRL) & CTRL_RST) && --timeout > 0)
        ;

    /* Small post-reset delay */
    for (volatile int i = 0; i < 100000; i++) ;

    /* ---- Disable interrupts ---- */
    e_write(E1000_IMC, 0xFFFFFFFFu);
    e_read(E1000_ICR); /* clear pending */

    /* ---- Set Link Up ---- */
    ctrl = e_read(E1000_CTRL);
    ctrl |= CTRL_ASDE | CTRL_SLU;
    ctrl &= ~CTRL_RST;
    e_write(E1000_CTRL, ctrl);

    /* ---- Read MAC from EEPROM ---- */
    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);

    g_mac[0] = (uint8_t)(w0 & 0xFF);
    g_mac[1] = (uint8_t)(w0 >> 8);
    g_mac[2] = (uint8_t)(w1 & 0xFF);
    g_mac[3] = (uint8_t)(w1 >> 8);
    g_mac[4] = (uint8_t)(w2 & 0xFF);
    g_mac[5] = (uint8_t)(w2 >> 8);

    kprint("E1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    /* Write MAC to RAL0/RAH0 */
    uint32_t ral = (uint32_t)w0 | ((uint32_t)w1 << 16);
    uint32_t rah = (uint32_t)w2 | (1u << 31); /* AV = address valid */
    e_write(E1000_RAL0, ral);
    e_write(E1000_RAH0, rah);

    /* ---- Clear Multicast Table ---- */
    for (int i = 0; i < 128; i++)
        e_write(E1000_MTA + (uint32_t)i * 4, 0);

    /* ---- Set up RX ring ---- */
    g_rx_ring = (struct e1000_rx_desc *)alloc_dma(&g_rx_ring_phys,
                                                    NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    if (!g_rx_ring) { kprint("E1000: RX ring alloc failed\n"); return; }

    for (int i = 0; i < NUM_RX_DESC; i++) {
        g_rx_bufs[i] = (uint8_t *)alloc_dma(&g_rx_bufs_phys[i], BUF_SIZE);
        if (!g_rx_bufs[i]) { kprint("E1000: RX buf alloc failed\n"); return; }
        g_rx_ring[i].addr = g_rx_bufs_phys[i];
        g_rx_ring[i].status = 0;
    }

    e_write(E1000_RDBAL, (uint32_t)(g_rx_ring_phys));
    e_write(E1000_RDBAH, (uint32_t)(g_rx_ring_phys >> 32));
    e_write(E1000_RDLEN, NUM_RX_DESC * (uint32_t)sizeof(struct e1000_rx_desc));
    e_write(E1000_RDH, 0);
    e_write(E1000_RDT, NUM_RX_DESC - 1);
    g_rx_tail = 0;

    /* Enable RX: broadcast accept, 8K buffers, strip CRC */
    e_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSEX | RCTL_BSIZE_8K | RCTL_SECRC);

    /* ---- Set up TX ring ---- */
    g_tx_ring = (struct e1000_tx_desc *)alloc_dma(&g_tx_ring_phys,
                                                    NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    if (!g_tx_ring) { kprint("E1000: TX ring alloc failed\n"); return; }

    for (int i = 0; i < NUM_TX_DESC; i++) {
        g_tx_bufs[i] = (uint8_t *)alloc_dma(&g_tx_bufs_phys[i], BUF_SIZE);
        if (!g_tx_bufs[i]) { kprint("E1000: TX buf alloc failed\n"); return; }
        g_tx_ring[i].addr = g_tx_bufs_phys[i];
        g_tx_ring[i].status = DESC_DD; /* mark available */
    }

    e_write(E1000_TDBAL, (uint32_t)(g_tx_ring_phys));
    e_write(E1000_TDBAH, (uint32_t)(g_tx_ring_phys >> 32));
    e_write(E1000_TDLEN, NUM_TX_DESC * (uint32_t)sizeof(struct e1000_tx_desc));
    e_write(E1000_TDH, 0);
    e_write(E1000_TDT, 0);
    g_tx_tail = 0;

    /* Enable TX: pad short packets, CT=16, COLD=64 */
    e_write(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x10u << TCTL_CT_SHIFT)
                         | (0x40u << TCTL_COLD_SHIFT));

    /* TIPG: IEEE 802.3 recommended */
    e_write(E1000_TIPG, 0x0060200A);

    /* ---- Link check ---- */
    uint32_t status = e_read(E1000_STATUS);
    if (status & 0x02u)
        kprint("E1000: link UP\n");
    else
        kprint("E1000: link down (will come up when traffic starts)\n");

    g_ready = 1;
    kprint("E1000: driver ready (RX=%d TX=%d buf=%d)\n",
           NUM_RX_DESC, NUM_TX_DESC, BUF_SIZE);
    {
        void kprint_serial_only(const char *fmt, ...);
        kprint_serial_only("E1000: mmio=0x%llx rx_ring_phys=0x%llx tx_ring_phys=0x%llx\n",
               (unsigned long long)(uint64_t)g_mmio,
               (unsigned long long)g_rx_ring_phys,
               (unsigned long long)g_tx_ring_phys);
    }

    /* Notify netcore */
    extern void netcore_attach_e1000(void);
    netcore_attach_e1000();
}
