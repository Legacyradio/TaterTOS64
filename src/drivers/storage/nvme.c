// NVMe driver (ported from TatertOS64-new, adapted for v3 physmap)

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/irq/manage.h"
#include "../../kernel/proc/process.h"
#include "../../kernel/proc/sched.h"
#include "../../drivers/irqchip/lapic.h"
#include "../../drivers/pci/pci.h"
#include "../../drivers/pci/msi.h"
#include "../../drivers/timer/hpet.h"
#include "../../kernel/acpi/extended.h"
#include "../../kernel/fs/part.h"
#include "vmd.h"

void kprint(const char *fmt, ...);

#define NVME_REG_CAP   0x00
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30

#define AHCI_VSCAP          0xA4
#define AHCI_REMAP_CAP      0x800
#define AHCI_REMAP_N_DCC    0x880
#define AHCI_REMAP_N_OFFSET 0x4000
#define AHCI_REMAP_N_SIZE   0x4000
#define AHCI_MAX_REMAP      3
#define PCI_CLASS_STORAGE_EXPRESS 0x010802

#define NVME_ADMIN_Q_DEPTH 16
#define NVME_IO_Q_DEPTH    64
#define NVME_PRP_LIST_MAX  256
#define NVME_CMD_TIMEOUT_MS       3000u
#define NVME_READY_TIMEOUT_MS     5000u
#define NVME_CMD_SPIN_FALLBACK    20000000u
#define NVME_READY_SPIN_FALLBACK  2000000u
#define NVME_YIELD_INTERVAL       4096u
#define NVME_WAIT_SLEEP_MS        1u
#define NVME_MSI_CAP_ID           0x05u

#define PHYSMAP_BASE VMM_PHYSMAP_BASE

struct nvme_cmd {
    uint8_t opc;
    uint8_t fuse;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct nvme_cpl {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

struct nvme_queue {
    struct nvme_cmd *sq;
    struct nvme_cpl *cq;
    uint16_t qid;
    uint16_t size;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t phase;
};

typedef uint32_t (*cfg_read32_fn)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
typedef void (*cfg_write32_fn)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

static volatile uint8_t *g_mmio = 0;
static uint32_t g_db_stride = 4;
static uint32_t g_sector_size = 512;
static uint64_t g_total_sectors = 0;
static uint32_t g_nsid = 1;
static uint8_t g_mdts = 0;
static int g_ready = 0;
static int g_msi_vector = -1;
static uint8_t g_msi_cap = 0;
static uint8_t g_msi_bus = 0;
static uint8_t g_msi_slot = 0;
static uint8_t g_msi_func = 0;
static uint8_t g_irq_completion = 0;
static cfg_read32_fn g_msi_cfg_read = 0;
static cfg_write32_fn g_msi_cfg_write = 0;
static volatile uint32_t g_irq_epoch = 0;
static volatile uint32_t g_wait_pid = 0;
static uint32_t g_nvme_trace_id = 0;
static uint32_t g_nvme_trace_budget = 0;

void nvme_trace_set(uint32_t trace_id, uint32_t budget) {
    g_nvme_trace_id = trace_id;
    g_nvme_trace_budget = budget;
    if (budget != 0) {
        kprint("NVME_TRACE[%u] armed budget=%u\n", trace_id, budget);
    }
}

static void nvme_trace_emit(const char *tag, uint64_t a, uint64_t b, uint64_t c) {
    if (g_nvme_trace_budget == 0) return;
    g_nvme_trace_budget--;
    kprint("NVME_TRACE[%u] %s a=%llu b=%llu c=%llu\n",
           g_nvme_trace_id, tag,
           (unsigned long long)a,
           (unsigned long long)b,
           (unsigned long long)c);
    if (g_nvme_trace_budget == 0) {
        kprint("NVME_TRACE[%u] budget exhausted\n", g_nvme_trace_id);
    }
}

static struct nvme_queue admin_q;
static struct nvme_queue io_q;
static uint16_t g_cid = 1;

static uint64_t admin_sq_phys = 0;
static uint64_t admin_cq_phys = 0;
static uint64_t io_sq_phys = 0;
static uint64_t io_cq_phys = 0;
static uint64_t identify_phys = 0;
static uint64_t ns_list_phys = 0;
static uint64_t prp_list_phys = 0;

static struct nvme_cmd *admin_sq = 0;
static struct nvme_cpl *admin_cq = 0;
static struct nvme_cmd *io_sq = 0;
static struct nvme_cpl *io_cq = 0;
static uint8_t *identify_buf = 0;
static uint8_t *ns_list_buf = 0;
static uint64_t *prp_list = 0;

static struct block_device nvme_bd;

static inline uint64_t phys_to_virt(uint64_t p) {
    return p + PHYSMAP_BASE;
}

static inline uint64_t virt_to_phys(uint64_t addr) {
    /* Physmap spans [VMM_PHYSMAP_BASE, KERNEL_VMA_BASE).
     * Kernel .text/.data/.stack live at KERNEL_VMA_BASE+, so they must go
     * through vmm_virt_to_phys — the simple subtraction gives a ~128 TB
     * garbage physical address for those addresses. */
    if (addr >= VMM_PHYSMAP_BASE && addr < KERNEL_VMA_BASE)
        return addr - VMM_PHYSMAP_BASE;
    return vmm_virt_to_phys(addr);
}

static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t*)(g_mmio + off);
}

static inline void mmio_write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_mmio + off) = v;
}

static inline uint64_t mmio_read64(uint32_t off) {
    uint32_t lo = mmio_read32(off);
    uint32_t hi = mmio_read32(off + 4);
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t mmio_read32_phys(uint64_t base, uint32_t off) {
    uint64_t addr = base + off;
    vmm_ensure_physmap_uc(addr + 1);
    return *(volatile uint32_t*)(uintptr_t)phys_to_virt(addr);
}

static inline uint64_t mmio_read64_phys(uint64_t base, uint32_t off) {
    uint32_t lo = mmio_read32_phys(base, off);
    uint32_t hi = mmio_read32_phys(base, off + 4);
    return ((uint64_t)hi << 32) | lo;
}

static uint8_t cfg_read8(cfg_read32_fn cfg_read, uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    if (!cfg_read) return 0xFFu;
    uint32_t v = cfg_read(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint32_t shift = (uint32_t)(off & 0x3u) * 8u;
    return (uint8_t)((v >> shift) & 0xFFu);
}

static uint16_t cfg_read16(cfg_read32_fn cfg_read, uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    if (!cfg_read) return 0xFFFFu;
    uint32_t v = cfg_read(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint32_t shift = (uint32_t)(off & 0x2u) * 8u;
    return (uint16_t)((v >> shift) & 0xFFFFu);
}

static void cfg_write16(cfg_read32_fn cfg_read, cfg_write32_fn cfg_write,
                        uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint16_t value) {
    if (!cfg_read || !cfg_write) return;
    uint8_t dword_off = (uint8_t)(off & 0xFCu);
    uint32_t shift = (uint32_t)(off & 0x2u) * 8u;
    uint32_t reg = cfg_read(bus, slot, func, dword_off);
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    cfg_write(bus, slot, func, dword_off, reg);
}

static int cpu_irqs_enabled(void) {
    uint64_t rflags = 0;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    return (rflags & (1ULL << 9)) != 0;
}

static uint8_t nvme_find_cap(cfg_read32_fn cfg_read, uint8_t bus, uint8_t slot,
                             uint8_t func, uint8_t cap_id) {
    uint16_t status = cfg_read16(cfg_read, bus, slot, func, 0x06);
    if ((status & (1u << 4)) == 0) return 0;

    uint8_t ptr = (uint8_t)(cfg_read8(cfg_read, bus, slot, func, 0x34) & 0xFCu);
    for (uint32_t hop = 0; hop < 48 && ptr >= 0x40u; hop++) {
        uint8_t id = cfg_read8(cfg_read, bus, slot, func, ptr);
        uint8_t next = (uint8_t)(cfg_read8(cfg_read, bus, slot, func, (uint8_t)(ptr + 1u)) & 0xFCu);
        if (id == cap_id) return ptr;
        if (next == 0 || next == ptr) break;
        ptr = next;
    }
    return 0;
}

static void nvme_irq_handler(uint32_t vector, void *ctx, void *dev_id, uint64_t error) {
    (void)vector;
    (void)ctx;
    (void)dev_id;
    (void)error;
    g_irq_epoch++;
    uint32_t pid = g_wait_pid;
    if (pid) {
        sched_wake(pid);
    }
}

static void nvme_irq_teardown(void) {
    if (g_msi_cap && g_msi_cfg_read && g_msi_cfg_write) {
        uint16_t ctrl = cfg_read16(g_msi_cfg_read, g_msi_bus, g_msi_slot, g_msi_func,
                                   (uint8_t)(g_msi_cap + 2u));
        ctrl &= (uint16_t)~1u;
        cfg_write16(g_msi_cfg_read, g_msi_cfg_write, g_msi_bus, g_msi_slot, g_msi_func,
                    (uint8_t)(g_msi_cap + 2u), ctrl);
    }
    if (g_msi_vector >= 0) {
        free_irq((uint32_t)g_msi_vector, 0);
        msi_free_vector(g_msi_vector);
    }
    g_msi_vector = -1;
    g_msi_cap = 0;
    g_msi_cfg_read = 0;
    g_msi_cfg_write = 0;
    g_irq_completion = 0;
    g_irq_epoch = 0;
    g_wait_pid = 0;
}

static int nvme_enable_msi(cfg_read32_fn cfg_read, cfg_write32_fn cfg_write,
                           uint8_t bus, uint8_t slot, uint8_t func) {
    if (!cfg_read || !cfg_write) return 0;

    uint8_t cap = nvme_find_cap(cfg_read, bus, slot, func, NVME_MSI_CAP_ID);
    if (!cap) return 0;

    int vec = msi_alloc_vector();
    if (vec < 0) return 0;
    if (request_irq((uint32_t)vec, nvme_irq_handler, 0, "nvme", 0) != 0) {
        msi_free_vector(vec);
        return 0;
    }

    uint16_t ctrl = cfg_read16(cfg_read, bus, slot, func, (uint8_t)(cap + 2u));
    uint8_t is_64 = (ctrl & (1u << 7)) ? 1u : 0u;

    ctrl &= (uint16_t)~1u;
    cfg_write16(cfg_read, cfg_write, bus, slot, func, (uint8_t)(cap + 2u), ctrl);

    uint32_t msg_addr = 0xFEE00000u | ((uint32_t)lapic_get_id() << 12);
    cfg_write(bus, slot, func, (uint8_t)(cap + 4u), msg_addr);

    uint8_t data_off = (uint8_t)(cap + 8u);
    if (is_64) {
        cfg_write(bus, slot, func, (uint8_t)(cap + 8u), 0);
        data_off = (uint8_t)(cap + 12u);
    }
    cfg_write16(cfg_read, cfg_write, bus, slot, func, data_off, (uint16_t)(vec & 0xFFu));

    ctrl = cfg_read16(cfg_read, bus, slot, func, (uint8_t)(cap + 2u));
    ctrl &= (uint16_t)~(0x7u << 4); /* force single-message */
    ctrl |= 1u;                     /* MSI enable */
    cfg_write16(cfg_read, cfg_write, bus, slot, func, (uint8_t)(cap + 2u), ctrl);

    g_msi_vector = vec;
    g_msi_cap = cap;
    g_msi_bus = bus;
    g_msi_slot = slot;
    g_msi_func = func;
    g_msi_cfg_read = cfg_read;
    g_msi_cfg_write = cfg_write;
    g_irq_completion = 1;
    g_irq_epoch = 0;
    g_wait_pid = 0;
    return 1;
}

static inline void doorbell_sq(struct nvme_queue *q) {
    uint32_t off = 0x1000 + (uint32_t)(2 * q->qid) * g_db_stride;
    mmio_write32(off, q->sq_tail);
}

static inline void doorbell_cq(struct nvme_queue *q) {
    uint32_t off = 0x1000 + (uint32_t)(2 * q->qid + 1) * g_db_stride;
    mmio_write32(off, q->cq_head);
}

static uint64_t nvme_now_ms(void) {
    uint64_t freq = hpet_get_freq_hz();
    if (freq == 0) return 0;
    uint64_t cnt = hpet_read_counter();
    return (cnt * 1000ULL) / freq;
}

static void cache_sync_range(void *buf, uint64_t len) {
    if (!buf || len == 0) return;
    uintptr_t start = (uintptr_t)buf;
    uintptr_t end = start + (uintptr_t)len;
    uintptr_t p = start & ~(uintptr_t)63u;
    while (p < end) {
        __asm__ volatile ("clflush (%0)" : : "r"((void*)p) : "memory");
        p += 64u;
    }
    __asm__ volatile ("mfence" : : : "memory");
}

static int build_prp(uint64_t buf, uint32_t len, uint64_t *prp1, uint64_t *prp2) {
    if (!prp1 || !prp2) return 0;
    if (len == 0) return 0;
    uint64_t buf_phys = virt_to_phys(buf);
    if (!buf_phys) return 0;
    *prp1 = buf_phys;
    *prp2 = 0;

    uint32_t off = (uint32_t)(buf_phys & 0xFFFu);
    uint32_t first_len = 4096u - off;
    if (len <= first_len) return 1;

    uint32_t remain = len - first_len;
    uint64_t second_virt = buf + first_len;
    uint64_t second_phys = virt_to_phys(second_virt);
    if (!second_phys) return 0;

    if (remain <= 4096u) {
        *prp2 = second_phys;
        return 1;
    }

    uint32_t pages = (remain + 4095u) / 4096u;
    if (pages > NVME_PRP_LIST_MAX) return 0;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t v = second_virt + (uint64_t)i * 4096u;
        uint64_t p = virt_to_phys(v);
        if (!p) return 0;
        prp_list[i] = p;
    }
    cache_sync_range(prp_list, (uint64_t)pages * sizeof(uint64_t));
    uint64_t prp_list_p = virt_to_phys((uint64_t)(uintptr_t)prp_list);
    if (!prp_list_p) return 0;
    *prp2 = prp_list_p;
    return 1;
}

static void queue_init(struct nvme_queue *q, struct nvme_cmd *sq, struct nvme_cpl *cq, uint16_t qid, uint16_t size) {
    q->sq = sq;
    q->cq = cq;
    q->qid = qid;
    q->size = size;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;
    for (uint16_t i = 0; i < size; i++) {
        q->cq[i].status = 0;
    }
}

static int nvme_try_cq(struct nvme_queue *q, struct nvme_cpl *out) {
    if (!q || !q->cq || q->size == 0) return 0;
    struct nvme_cpl *cpl = &q->cq[q->cq_head];
    __asm__ volatile ("clflush (%0)" : : "r"(cpl) : "memory");
    __asm__ volatile ("mfence" : : : "memory");
    uint16_t st = cpl->status;
    if ((st & 1u) != q->phase) return 0;

    if (out) *out = *cpl;
    q->cq_head++;
    if (q->cq_head >= q->size) {
        q->cq_head = 0;
        q->phase ^= 1u;
    }
    doorbell_cq(q);
    return 1;
}

static int poll_cq(struct nvme_queue *q, struct nvme_cpl *out) {
    uint64_t start_ms = nvme_now_ms();
    for (uint32_t spin = 0;; spin++) {
        if (nvme_try_cq(q, out)) return 1;

        if (start_ms != 0) {
            uint64_t now = nvme_now_ms();
            if (now >= start_ms && (now - start_ms) >= NVME_CMD_TIMEOUT_MS) break;
        } else if (spin >= NVME_CMD_SPIN_FALLBACK) {
            break;
        }

        __asm__ volatile ("pause" : : : "memory");
        if ((spin % NVME_YIELD_INTERVAL) == 0 && proc_current()) {
            sched_yield();
        }
    }
    return 0;
}

static int wait_cq_irq(struct nvme_queue *q, struct nvme_cpl *out) {
    uint64_t start_ms = nvme_now_ms();
    for (uint32_t spin = 0;; spin++) {
        if (nvme_try_cq(q, out)) return 1;

        if (start_ms != 0) {
            uint64_t now = nvme_now_ms();
            if (now >= start_ms && (now - start_ms) >= NVME_CMD_TIMEOUT_MS) break;
        } else if (spin >= NVME_CMD_SPIN_FALLBACK) {
            break;
        }

        struct fry_process *cur = proc_current();
        if (!cur || !cpu_irqs_enabled()) {
            __asm__ volatile ("pause" : : : "memory");
            continue;
        }

        uint32_t pid = cur->pid;
        uint32_t before_epoch = g_irq_epoch;
        g_wait_pid = pid;
        __asm__ volatile ("" : : : "memory");

        if (nvme_try_cq(q, out)) {
            g_wait_pid = 0;
            return 1;
        }
        if (g_irq_epoch != before_epoch) {
            g_wait_pid = 0;
            continue;
        }

        sched_sleep(pid, NVME_WAIT_SLEEP_MS);
        sched_yield();
        if (g_wait_pid == pid) {
            g_wait_pid = 0;
        }
    }
    g_wait_pid = 0;
    return 0;
}

static int wait_cq(struct nvme_queue *q, struct nvme_cpl *out) {
    if (q == &io_q && g_irq_completion) {
        return wait_cq_irq(q, out);
    }
    return poll_cq(q, out);
}

static int submit_cmd(struct nvme_queue *q, struct nvme_cmd *cmd, struct nvme_cpl *out) {
    if (!q || !cmd) return 0;
    uint16_t cid = g_cid++;
    if (g_cid == 0) g_cid = 1;

    cmd->cid = cid;
    q->sq[q->sq_tail] = *cmd;
    __asm__ volatile ("" : : : "memory");
    __asm__ volatile ("clflush (%0)" : : "r"(&q->sq[q->sq_tail]) : "memory");
    __asm__ volatile ("mfence" : : : "memory");

    q->sq_tail++;
    if (q->sq_tail >= q->size) q->sq_tail = 0;
    doorbell_sq(q);

    struct nvme_cpl cpl;
    if (!wait_cq(q, &cpl)) return 0;
    if (cpl.cid != cid) return 0;
    if (((cpl.status >> 1) & 0x7FFu) != 0) return 0;
    if (out) *out = cpl;
    return 1;
}

static int nvme_identify(uint32_t nsid, uint32_t cns, void *buf) {
    if (!buf) return 0;
    struct nvme_cmd cmd;
    for (size_t i = 0; i < sizeof(cmd); i++) ((uint8_t*)&cmd)[i] = 0;

    uint64_t prp1 = 0, prp2 = 0;
    if (!build_prp((uint64_t)(uintptr_t)buf, 4096, &prp1, &prp2)) return 0;

    cmd.opc = 0x06;
    cmd.nsid = nsid;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = cns;
    return submit_cmd(&admin_q, &cmd, 0);
}

static uint32_t nvme_pick_nsid(void) {
    if (!nvme_identify(0, 2, ns_list_buf)) return 1;
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t nsid = *(uint32_t*)(ns_list_buf + i * 4u);
        if (nsid != 0) return nsid;
    }
    return 1;
}

static int nvme_set_num_queues(uint16_t req_nsq, uint16_t req_ncq, uint16_t *out_nsq, uint16_t *out_ncq) {
    struct nvme_cmd cmd;
    for (size_t i = 0; i < sizeof(cmd); i++) ((uint8_t*)&cmd)[i] = 0;
    cmd.opc = 0x09; // Set Features
    cmd.cdw10 = 0x07; // Number of Queues
    cmd.cdw11 = ((uint32_t)req_nsq << 16) | (uint32_t)req_ncq;
    struct nvme_cpl cpl;
    if (!submit_cmd(&admin_q, &cmd, &cpl)) return 0;
    uint32_t dw0 = cpl.dw0;
    uint16_t ncq = (uint16_t)(dw0 & 0xFFFFu);
    uint16_t nsq = (uint16_t)((dw0 >> 16) & 0xFFFFu);
    if (out_nsq) *out_nsq = nsq;
    if (out_ncq) *out_ncq = ncq;
    return 1;
}

static int nvme_create_io_queues(uint16_t qdepth) {
    uint16_t nsq = 0, ncq = 0;
    int setq_ok = nvme_set_num_queues(0xFFFFu, 0xFFFFu, &nsq, &ncq);
    if (!setq_ok) {
        nsq = 0;
        ncq = 0;
    }
    uint16_t max_qpairs = (uint16_t)((nsq < ncq ? nsq : ncq) + 1u);
    if (max_qpairs < 1) max_qpairs = 1;
    static const uint16_t depth_try[] = {2, 4, 8, 16};
    struct nvme_cmd cmd;
    uint16_t qid = 1;
    if (max_qpairs < qid) return 0;
    for (size_t di = 0; di < sizeof(depth_try) / sizeof(depth_try[0]); di++) {
        uint16_t d = depth_try[di];
        if (d > qdepth) d = qdepth;
        if (d < 2) continue;
        queue_init(&io_q, io_sq, io_cq, qid, d);

        for (size_t i = 0; i < sizeof(cmd); i++) ((uint8_t*)&cmd)[i] = 0;
        cmd.opc = 0x05; // Create IO CQ
        cmd.prp1 = virt_to_phys((uint64_t)(uintptr_t)io_cq);
        cmd.cdw10 = (uint32_t)qid | ((uint32_t)(d - 1) << 16);
        cmd.cdw11 = g_irq_completion ? 3u : 1u; // PC=1, IEN={1|0}, IV=0
        if (!submit_cmd(&admin_q, &cmd, 0)) {
            continue;
        }

        for (size_t i = 0; i < sizeof(cmd); i++) ((uint8_t*)&cmd)[i] = 0;
        cmd.opc = 0x01; // Create IO SQ
        cmd.prp1 = virt_to_phys((uint64_t)(uintptr_t)io_sq);
        cmd.cdw10 = (uint32_t)qid | ((uint32_t)(d - 1) << 16);
        cmd.cdw11 = ((uint32_t)qid << 16) | 1u; // CQID + PC=1
        if (!submit_cmd(&admin_q, &cmd, 0)) {
            for (size_t i = 0; i < sizeof(cmd); i++) ((uint8_t*)&cmd)[i] = 0;
            cmd.opc = 0x04; // Delete IO CQ
            cmd.cdw10 = (uint32_t)qid;
            submit_cmd(&admin_q, &cmd, 0);
            continue;
        }
        return 1;
    }

    return 0;
}

static int nvme_wait_ready(int want_ready) {
    uint64_t start_ms = nvme_now_ms();
    for (uint32_t spin = 0;; spin++) {
        uint32_t csts = mmio_read32(NVME_REG_CSTS);
        if (((csts & 1u) != 0) == (want_ready != 0)) return 1;
        if (start_ms != 0) {
            uint64_t now = nvme_now_ms();
            if (now >= start_ms && (now - start_ms) >= NVME_READY_TIMEOUT_MS) break;
        } else if (spin >= NVME_READY_SPIN_FALLBACK) {
            break;
        }
        __asm__ volatile ("pause" : : : "memory");
    }
    return 0;
}

static int nvme_alloc_buffers(void) {
    admin_sq_phys = pmm_alloc_page_below(0x100000000ULL);
    admin_cq_phys = pmm_alloc_page_below(0x100000000ULL);
    io_sq_phys = pmm_alloc_page_below(0x100000000ULL);
    io_cq_phys = pmm_alloc_page_below(0x100000000ULL);
    identify_phys = pmm_alloc_page_below(0x100000000ULL);
    ns_list_phys = pmm_alloc_page_below(0x100000000ULL);
    prp_list_phys = pmm_alloc_page_below(0x100000000ULL);

    if (!admin_sq_phys || !admin_cq_phys || !io_sq_phys || !io_cq_phys ||
        !identify_phys || !ns_list_phys || !prp_list_phys) {
        return 0;
    }

    admin_sq = (struct nvme_cmd *)(uintptr_t)vmm_phys_to_virt(admin_sq_phys);
    admin_cq = (struct nvme_cpl *)(uintptr_t)vmm_phys_to_virt(admin_cq_phys);
    io_sq = (struct nvme_cmd *)(uintptr_t)vmm_phys_to_virt(io_sq_phys);
    io_cq = (struct nvme_cpl *)(uintptr_t)vmm_phys_to_virt(io_cq_phys);
    identify_buf = (uint8_t *)(uintptr_t)vmm_phys_to_virt(identify_phys);
    ns_list_buf = (uint8_t *)(uintptr_t)vmm_phys_to_virt(ns_list_phys);
    prp_list = (uint64_t *)(uintptr_t)vmm_phys_to_virt(prp_list_phys);

    for (uint32_t i = 0; i < 4096; i++) ((uint8_t*)admin_sq)[i] = 0;
    for (uint32_t i = 0; i < 4096; i++) ((uint8_t*)admin_cq)[i] = 0;
    for (uint32_t i = 0; i < 4096; i++) ((uint8_t*)io_sq)[i] = 0;
    for (uint32_t i = 0; i < 4096; i++) ((uint8_t*)io_cq)[i] = 0;
    for (uint32_t i = 0; i < 4096; i++) identify_buf[i] = 0;
    for (uint32_t i = 0; i < 4096; i++) ns_list_buf[i] = 0;
    for (uint32_t i = 0; i < 4096 / 8; i++) prp_list[i] = 0;

    return 1;
}

static int nvme_init_from_mmio(uint64_t mmio) {
    if (!mmio) return 0;
    vmm_ensure_physmap_uc(mmio + 0x2000);
    g_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(mmio);

    if (!nvme_alloc_buffers()) return 0;

    uint64_t cap = mmio_read64(NVME_REG_CAP);
    uint16_t mqes = (uint16_t)(cap & 0xFFFFu);
    uint16_t max_qe = (uint16_t)(mqes + 1u);
    uint16_t admin_q_depth = NVME_ADMIN_Q_DEPTH;
    uint16_t io_q_depth = NVME_IO_Q_DEPTH;
    if (admin_q_depth > max_qe) admin_q_depth = max_qe;
    if (io_q_depth > max_qe) io_q_depth = max_qe;
    if (admin_q_depth < 2 || io_q_depth < 2) return 0;

    uint8_t dstrd = (uint8_t)((cap >> 32) & 0x0Fu);
    g_db_stride = 4u << dstrd;
    uint8_t mpsmin = (uint8_t)((cap >> 48) & 0x0Fu);

    mmio_write32(NVME_REG_CC, 0);
    if (!nvme_wait_ready(0)) return 0;

    queue_init(&admin_q, admin_sq, admin_cq, 0, admin_q_depth);
    queue_init(&io_q, io_sq, io_cq, 1, io_q_depth);

    mmio_write32(NVME_REG_AQA, ((uint32_t)(admin_q_depth - 1) << 16) | (uint32_t)(admin_q_depth - 1));
    uint64_t asq_phys = admin_sq_phys;
    uint64_t acq_phys = admin_cq_phys;
    mmio_write32(NVME_REG_ASQ, (uint32_t)asq_phys);
    mmio_write32(NVME_REG_ASQ + 4, (uint32_t)(asq_phys >> 32));
    mmio_write32(NVME_REG_ACQ, (uint32_t)acq_phys);
    mmio_write32(NVME_REG_ACQ + 4, (uint32_t)(acq_phys >> 32));

    uint32_t cc = 0;
    cc |= (0u << 4);   // CSS
    cc |= ((uint32_t)mpsmin << 8);   // MPS
    cc |= (0u << 11);  // AMS
    cc |= (0u << 14);  // SHN
    cc |= (6u << 16);  // IOSQES (64 bytes)
    cc |= (4u << 20);  // IOCQES (16 bytes)
    cc |= 1u;          // EN
    mmio_write32(NVME_REG_CC, cc);
    if (!nvme_wait_ready(1)) return 0;

    if (!nvme_identify(0, 1, identify_buf)) return 0;
    g_mdts = identify_buf[77];

    if (!nvme_create_io_queues(io_q_depth)) return 0;

    g_nsid = nvme_pick_nsid();
    if (!nvme_identify(g_nsid, 0, identify_buf)) return 0;
    uint64_t nsze = *(uint64_t*)(identify_buf + 0x00);
    if (nsze == 0 && g_nsid != 1) {
        g_nsid = 1;
        if (!nvme_identify(g_nsid, 0, identify_buf)) return 0;
        nsze = *(uint64_t*)(identify_buf + 0x00);
    }
    if (nsze == 0) return 0;

    uint8_t flbas = identify_buf[0x1A];
    uint8_t fmt = (uint8_t)(flbas & 0x0Fu);
    uint8_t *lbaf = identify_buf + 0x80 + (uint32_t)fmt * 4u;
    uint8_t lbads = lbaf[2];
    g_sector_size = 1u << lbads;
    g_total_sectors = nsze;

    g_ready = 1;
    return 1;
}

static int nvme_find_with_cfg(uint8_t bus_start, uint8_t bus_end, cfg_read32_fn cfg_read,
                              uint8_t *out_b, uint8_t *out_s, uint8_t *out_f) {
    for (uint16_t bus = bus_start; bus <= bus_end; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = cfg_read((uint8_t)bus, slot, func, 0x00);
                if (id == 0xFFFFFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint32_t cc = cfg_read((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (uint8_t)(cc >> 24);
                uint8_t subclass = (uint8_t)(cc >> 16);
                uint8_t prog = (uint8_t)(cc >> 8);
                if (class_code == 0x01 && subclass == 0x08 &&
                    (prog == 0x02 || prog == 0x01 || prog == 0x00)) {
                    if (out_b) *out_b = (uint8_t)bus;
                    if (out_s) *out_s = slot;
                    if (out_f) *out_f = func;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return pci_ecam_read32(0, bus, slot, func, offset);
}

static void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    pci_ecam_write32(0, bus, slot, func, offset, value);
}

static int nvme_init_from_cfg(uint8_t bus_start, uint8_t bus_end, cfg_read32_fn cfg_read, cfg_write32_fn cfg_write) {
    uint8_t fb = 0, fs = 0, ff = 0;
    if (!nvme_find_with_cfg(bus_start, bus_end, cfg_read, &fb, &fs, &ff)) return 0;

    if (cfg_read == pci_cfg_read32) {
        acpi_nvme_dsm_for_pci(fb, fs, ff);
    }

    uint32_t bar0 = cfg_read(fb, fs, ff, 0x10);
    uint32_t bar1 = cfg_read(fb, fs, ff, 0x14);
    if ((bar0 & 0x1u) != 0) return 0; // IO BAR not supported
    uint64_t mmio = (uint64_t)(bar0 & ~0xFULL);
    if ((bar0 & 0x6u) == 0x4u) {
        mmio |= ((uint64_t)bar1 << 32);
    }
    if (!mmio) return 0;

    uint32_t cmd = cfg_read(fb, fs, ff, 0x04);
    cmd |= 0x0006u; // MEM + BUS MASTER
    cfg_write(fb, fs, ff, 0x04, cmd);

    if (!nvme_enable_msi(cfg_read, cfg_write, fb, fs, ff)) {
        g_irq_completion = 0;
        kprint("NVMe: MSI setup unavailable, using bounded polling\n");
    }

    if (!nvme_init_from_mmio(mmio)) {
        nvme_irq_teardown();
        return 0;
    }
    return 1;
}

static int nvme_init_from_rst_remap(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_cfg_read32((uint8_t)bus, slot, func, 0x00);
                if (id == 0xFFFFFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint16_t ven = (uint16_t)(id & 0xFFFF);
                if (ven != 0x8086) continue;
                uint32_t cc = pci_cfg_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (uint8_t)(cc >> 24);
                uint8_t subclass = (uint8_t)(cc >> 16);
                if (class_code != 0x01 || subclass != 0x04) continue;

                uint32_t bar5 = pci_cfg_read32((uint8_t)bus, slot, func, 0x24);
                if (bar5 & 0x1u) continue;
                uint64_t abar = (uint64_t)(bar5 & ~0xFULL);
                if (!abar) continue;

                uint32_t cmd = pci_cfg_read32((uint8_t)bus, slot, func, 0x04);
                cmd |= 0x0006u; // MEM + BUS MASTER
                pci_cfg_write32((uint8_t)bus, slot, func, 0x04, cmd);

                uint32_t vscap = mmio_read32_phys(abar, AHCI_VSCAP);
                if ((vscap & 0x1u) == 0) continue;

                uint64_t cap = mmio_read64_phys(abar, AHCI_REMAP_CAP);
                for (uint32_t i = 0; i < AHCI_MAX_REMAP; i++) {
                    if ((cap & (1ull << i)) == 0) continue;
                    uint32_t dcc = mmio_read32_phys(abar, AHCI_REMAP_N_DCC + i * 0x80u);
                    if (dcc != PCI_CLASS_STORAGE_EXPRESS) continue;
                    uint64_t mmio = abar + AHCI_REMAP_N_OFFSET + (uint64_t)i * AHCI_REMAP_N_SIZE;
                    if (nvme_init_from_mmio(mmio)) return 1;
                }
            }
        }
    }
    return 0;
}

static uint32_t nvme_max_blocks(void) {
    if (g_sector_size == 0) return 0;
    uint64_t max_bytes = 1024ull * 1024ull; // 1MB safety cap
    if (g_mdts != 0) {
        uint64_t mdts_bytes = 1ull << (g_mdts + 12u);
        if (mdts_bytes < max_bytes) max_bytes = mdts_bytes;
    }
    uint64_t blocks = max_bytes / g_sector_size;
    if (blocks == 0) blocks = 1;
    if (blocks > 65536ull) blocks = 65536ull;
    return (uint32_t)blocks;
}

static int nvme_io_rw(int is_write, uint64_t lba, uint32_t count, void *buf) {
    if (!g_ready || !buf || count == 0) return 0;
    if (g_sector_size == 0) return 0;

    uint32_t max_blocks = nvme_max_blocks();
    uint8_t *p = (uint8_t*)buf;

    while (count > 0) {
        uint32_t blocks = count;
        if (blocks > max_blocks) blocks = max_blocks;
        uint64_t bytes = (uint64_t)blocks * g_sector_size;

        nvme_trace_emit(is_write ? "io_write" : "io_read", lba, blocks, bytes);

        uint64_t prp1 = 0, prp2 = 0;
        if (!build_prp((uint64_t)(uintptr_t)p, (uint32_t)bytes, &prp1, &prp2)) return 0;

        struct nvme_cmd cmd;
        for (size_t i = 0; i < sizeof(cmd); i++) ((uint8_t*)&cmd)[i] = 0;
        cmd.opc = is_write ? 0x01 : 0x02;
        cmd.nsid = g_nsid;
        cmd.prp1 = prp1;
        cmd.prp2 = prp2;
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = (uint32_t)(blocks - 1);

        cache_sync_range(p, bytes);
        if (!submit_cmd(&io_q, &cmd, 0)) {
            nvme_trace_emit("submit_fail", lba, blocks, 0);
            return 0;
        }
        if (!is_write) cache_sync_range(p, bytes);

        lba += blocks;
        count -= blocks;
        p += bytes;
    }

    return 1;
}

static int nvme_read_blocks(void *ctx, uint64_t lba, void *buf, uint32_t count) {
    (void)ctx;
    return nvme_io_rw(0, lba, count, buf) ? 0 : -1;
}

static int nvme_write_blocks(void *ctx, uint64_t lba, const void *buf, uint32_t count) {
    (void)ctx;
    return nvme_io_rw(1, lba, count, (void*)buf) ? 0 : -1;
}

void nvme_init(void) {
    g_ready = 0;
    g_mmio = 0;
    nvme_irq_teardown();

    if (nvme_init_from_cfg(0, 255, pci_cfg_read32, pci_cfg_write32)) goto ok;
    if (nvme_init_from_rst_remap()) goto ok;

    if (vmd_ready()) {
        uint8_t start = vmd_bus_start();
        uint8_t count = vmd_bus_count();
        uint8_t end = (uint8_t)(start + (count ? (count - 1) : 0));
        if (end < start) end = 255;
        if (nvme_init_from_cfg(start, end, vmd_cfg_read32, vmd_cfg_write32)) goto ok;
    }

    nvme_irq_teardown();
    kprint("NVMe: controller not found\n");
    return;

ok:
    kprint("NVMe: ready nsid=%u lba_bytes=%u sectors=%llu\n",
           g_nsid, g_sector_size, (unsigned long long)g_total_sectors);
    if (g_irq_completion) {
        kprint("NVMe: completion mode=irq vector=0x%x\n", g_msi_vector);
    } else {
        kprint("NVMe: completion mode=bounded-poll fallback\n");
    }
    nvme_bd.sector_size = g_sector_size;
    nvme_bd.total_sectors = g_total_sectors;
    nvme_bd.read = nvme_read_blocks;
    nvme_bd.write = nvme_write_blocks;
    nvme_bd.ctx = 0;
}

struct block_device *nvme_get_block_device(void) {
    if (!g_ready) return 0;
    return &nvme_bd;
}
