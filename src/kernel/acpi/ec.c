#include <stdint.h>
#include "ec.h"
#include "fadt.h"
#include "namespace.h"
#include "aml_exec.h"

void kprint(const char *fmt, ...);

/* Forward declarations for process/sched used by query worker */
#include "../proc/process.h"
#include "../proc/sched.h"

/*
 * ACPI Embedded Controller (EC) interface
 *
 * Ports come from PNP0C09 _CRS (preferred) or defaults 0x62/0x66.
 * Fallback: 0x68/0x6C.
 */

#define EC_OBF    0x01
#define EC_IBF    0x02
#define EC_SCI_EVT 0x20
#define EC_CMD_READ  0x80
#define EC_CMD_WRITE 0x81
#define EC_CMD_BURST_ENABLE  0x82
#define EC_CMD_BURST_DISABLE 0x83
#define EC_CMD_SCI_QUERY     0x84
#define EC_BURST_ACK         0x90

/* Policy struct — all tunable knobs with defaults matching previous hardcoded values */
static ec_policy_t g_ec_policy = {
    .ibf_obf_timeout    = 10000,      /* was 100000 — 10ms is plenty */
    .probe_retries      = 2,          /* was 5 — bail fast on dead bus */
    .retry_delay        = 200000,     /* was 2000000 */
    .max_consec_fail    = 4,
    .acpi_enable_wait   = 500000,     /* was 5000000 */
    .force_enable_wait  = 5000000,    /* was 50000000 */
    .burst_disable_wait = 100000,     /* was 1000000 */
    .obf_drain_max      = 64,         /* was 256 */
    .sci_query_drain_max = 4,         /* was 16 */
    .try_alternate_ports = 1,
    .try_swapped_ports   = 1,
    .suppress_reg_io     = 1,
};

static uint16_t g_data_port = 0x62;
static uint16_t g_cmd_port  = 0x66;
static int g_ec_ok = 0;
static int g_ec_timeout_reported = 0;
static uint32_t g_ec_consec_fail = 0;

/* _REG suppression: while set, ec_read/ec_write return dummy data without
 * touching EC ports.  Prevents AML _REG method from sending EC commands
 * before the handshake completes (kills Dell Precision 7530 EC). */
static int g_ec_reg_in_progress = 0;
static uint32_t g_ec_reg_suppressed = 0;

/* EC query queue (G451) */
#define EC_QUERY_QUEUE_SIZE 32
static uint8_t ec_query_queue[EC_QUERY_QUEUE_SIZE];
static volatile uint32_t ec_query_head = 0;
static volatile uint32_t ec_query_tail = 0;
static volatile uint32_t ec_queries_dispatched = 0;
static volatile uint32_t ec_queries_dropped = 0;
static int ec_query_worker_running = 0;

/* EC namespace node for _Qxx dispatch */
static struct acpi_node *g_ec_ns_node = 0;

/* Storm guard + freeze (G452) */
static volatile int g_ec_events_frozen = 0;
static volatile uint32_t g_ec_storm_count = 0;
#define EC_STORM_THRESHOLD 8
static volatile uint32_t g_ec_queries_this_tick = 0;

/* Probe diagnostics — surfaced on-screen via sysinfo since Dell has no serial */
#define EC_STEP_NOT_PROBED  0
#define EC_STEP_STS_FF      1  /* status register reads 0xFF (no device) */
#define EC_STEP_IBF_PRE     2  /* IBF stuck before sending command */
#define EC_STEP_IBF_POST    3  /* IBF stuck after sending command */
#define EC_STEP_OBF_TIMEOUT 4  /* OBF never set — EC didn't produce data */
#define EC_STEP_OK          5  /* probe succeeded */
static uint8_t g_ec_probe_step = EC_STEP_NOT_PROBED;
static uint8_t g_ec_probe_status = 0;
static uint8_t g_ec_probe_attempts = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

static void ec_busy_wait(uint32_t iters) {
    for (volatile uint32_t i = 0; i < iters; i++) {}
}

static void ec_drain_obf(void) {
    uint32_t max = g_ec_policy.obf_drain_max;
    for (uint32_t i = 0; i < max; i++) {
        if (!(inb(g_cmd_port) & EC_OBF)) break;
        (void)inb(g_data_port);
        io_wait();
    }
}

static int ec_wait_ibf_clear(void) {
    uint32_t timeout = g_ec_policy.ibf_obf_timeout;
    for (uint32_t i = 0; i < timeout; i++) {
        if (!(inb(g_cmd_port) & EC_IBF)) return 1;
        io_wait();
    }
    return 0;
}

static int ec_wait_obf_set(void) {
    uint32_t timeout = g_ec_policy.ibf_obf_timeout;
    for (uint32_t i = 0; i < timeout; i++) {
        if (inb(g_cmd_port) & EC_OBF) return 1;
        io_wait();
    }
    return 0;
}

static const char *ec_probe_step_name(uint8_t step) {
    switch (step) {
    case EC_STEP_STS_FF:      return "sts_ff";
    case EC_STEP_IBF_PRE:     return "ibf_pre";
    case EC_STEP_IBF_POST:    return "ibf_post";
    case EC_STEP_OBF_TIMEOUT: return "obf_timeout";
    case EC_STEP_OK:          return "ok";
    default:                  return "unknown";
    }
}

static void ec_note_io_fail(void) {
    if (g_ec_consec_fail < 0xFFFFFFFFu) {
        g_ec_consec_fail++;
    }
    if (g_ec_consec_fail >= g_ec_policy.max_consec_fail && g_ec_ok) {
        if (!g_ec_timeout_reported) {
            kprint("ACPI: EC timeout, disabling EC after %u consecutive failures\n",
                   (unsigned)g_ec_consec_fail);
            g_ec_timeout_reported = 1;
        }
        g_ec_ok = 0;
    }
}

static void ec_note_io_ok(void) {
    g_ec_consec_fail = 0;
}

static void ec_try_acpi_enable(void) {
    const struct fadt_info *f = fadt_get_info();
    if (!f || !f->smi_cmd || !f->acpi_enable) return;
    if (f->smi_cmd > 0xFFFFu) return;
    if (f->pm1a_cnt_blk) {
        uint16_t pm1 = inw((uint16_t)f->pm1a_cnt_blk);
        if (pm1 & 1u) {
            kprint("ACPI: SCI_EN already set (PM1a=0x%04x)\n", pm1);
            return;
        }
    }
    kprint("ACPI: sending ACPI_ENABLE to SMI_CMD 0x%x\n", f->smi_cmd);
    outb((uint16_t)f->smi_cmd, f->acpi_enable);
    /* Wait for transition */
    ec_busy_wait(g_ec_policy.acpi_enable_wait);
    if (f->pm1a_cnt_blk) {
        uint16_t pm1 = inw((uint16_t)f->pm1a_cnt_blk);
        kprint("ACPI: after enable PM1a=0x%04x SCI_EN=%u\n", pm1, pm1 & 1u);
    }
}

static int ec_read_raw(uint8_t addr, uint8_t *out) {
    if (!out) return 0;
    if (!ec_wait_ibf_clear()) {
        return 0;
    }
    outb(g_cmd_port, EC_CMD_READ);
    if (!ec_wait_ibf_clear()) {
        return 0;
    }
    outb(g_data_port, addr);
    if (!ec_wait_obf_set()) {
        return 0;
    }
    *out = inb(g_data_port);
    return 1;
}

int ec_read(uint8_t addr, uint8_t *out) {
    if (!g_ec_ok || !out) return 0;
    if (g_ec_policy.suppress_reg_io && g_ec_reg_in_progress) {
        /* _REG is evaluating — do NOT touch EC ports.  Return dummy 0. */
        g_ec_reg_suppressed++;
        *out = 0;
        return 1;
    }
    if (!ec_read_raw(addr, out)) {
        ec_note_io_fail();
        return 0;
    }
    ec_note_io_ok();
    return 1;
}

int ec_write(uint8_t addr, uint8_t val) {
    if (!g_ec_ok) return 0;
    if (g_ec_policy.suppress_reg_io && g_ec_reg_in_progress) {
        /* _REG is evaluating — do NOT touch EC ports.  Silently succeed. */
        g_ec_reg_suppressed++;
        return 1;
    }
    if (!ec_wait_ibf_clear()) {
        ec_note_io_fail();
        return 0;
    }
    outb(g_cmd_port, EC_CMD_WRITE);
    if (!ec_wait_ibf_clear()) {
        ec_note_io_fail();
        return 0;
    }
    outb(g_data_port, addr);
    if (!ec_wait_ibf_clear()) {
        ec_note_io_fail();
        return 0;
    }
    outb(g_data_port, val);
    ec_note_io_ok();
    return 1;
}

/*
 * ec_reset_state: Reset EC state machine after failed/garbled transactions.
 * Waits for IBF clear, drains OBF, delays ~5ms, drains again.
 */
void ec_reset_state(void) {
    /* Wait for IBF to clear — up to the normal timeout */
    ec_wait_ibf_clear();
    /* Drain any pending output bytes */
    ec_drain_obf();
    /* Delay ~5ms (busy-loop) */
    ec_busy_wait(5000000);
    /* Drain again in case EC produced something during the delay */
    ec_drain_obf();
}

/*
 * ec_burst_enable: Send BURST_ENABLE (0x82) to the EC command port.
 * Waits for OBF and checks for 0x90 ack byte.
 * Returns 1 on success (burst mode active), 0 on failure.
 */
int ec_burst_enable(void) {
    if (!ec_wait_ibf_clear()) return 0;
    outb(g_cmd_port, EC_CMD_BURST_ENABLE);
    if (!ec_wait_obf_set()) return 0;
    uint8_t ack = inb(g_data_port);
    return (ack == EC_BURST_ACK);
}

/*
 * ec_burst_disable: Send BURST_DISABLE (0x83) to exit burst mode.
 */
void ec_burst_disable(void) {
    if (!ec_wait_ibf_clear()) return;
    outb(g_cmd_port, EC_CMD_BURST_DISABLE);
    /* Give EC time to process */
    ec_busy_wait(g_ec_policy.burst_disable_wait);
}

/*
 * ec_sci_query_drain: Repeatedly send SCI_QUERY (0x84) and read the event
 * byte until event==0 or 16 iterations. Clears pending SCI events that
 * may be blocking the EC from accepting new commands.
 */
void ec_sci_query_drain(void) {
    uint32_t max = g_ec_policy.sci_query_drain_max;
    for (uint32_t i = 0; i < max; i++) {
        if (!ec_wait_ibf_clear()) break;
        outb(g_cmd_port, EC_CMD_SCI_QUERY);
        if (!ec_wait_obf_set()) break;
        uint8_t evt = inb(g_data_port);
        if (evt == 0) break;
    }
}

/*
 * ec_force_acpi_enable: Send ACPI_ENABLE to SMI_CMD unconditionally,
 * bypassing the SCI_EN check. On Dell Precision 7530, the BIOS sets
 * SCI_EN in PM1a_CNT but still needs the ACPI_ENABLE SMI to fully
 * hand off EC control to the OS.
 * Returns 1 if the SMI was sent, 0 if FADT info unavailable.
 */
int ec_force_acpi_enable(void) {
    const struct fadt_info *f = fadt_get_info();
    if (!f || !f->smi_cmd || !f->acpi_enable) {
        kprint("ACPI: EC force_acpi_enable: no SMI_CMD in FADT\n");
        return 0;
    }
    if (f->smi_cmd > 0xFFFFu) {
        kprint("ACPI: EC force_acpi_enable: SMI_CMD out of range\n");
        return 0;
    }

    if (f->pm1a_cnt_blk) {
        uint16_t pm1 = inb((uint16_t)f->pm1a_cnt_blk) |
                        ((uint16_t)inb((uint16_t)(f->pm1a_cnt_blk + 1)) << 8);
        kprint("ACPI: EC force_acpi_enable: PM1a=0x%04x SCI_EN=%u (forcing anyway)\n",
               (uint32_t)pm1, (uint32_t)(pm1 & 1u));
    }

    kprint("ACPI: EC force_acpi_enable: sending ACPI_ENABLE to SMI_CMD 0x%x\n",
           f->smi_cmd);
    outb((uint16_t)f->smi_cmd, f->acpi_enable);

    /* Wait for the SMI to process */
    ec_busy_wait(g_ec_policy.force_enable_wait);

    if (f->pm1a_cnt_blk) {
        uint16_t pm1 = inb((uint16_t)f->pm1a_cnt_blk) |
                        ((uint16_t)inb((uint16_t)(f->pm1a_cnt_blk + 1)) << 8);
        kprint("ACPI: EC force_acpi_enable: after PM1a=0x%04x SCI_EN=%u\n",
               (uint32_t)pm1, (uint32_t)(pm1 & 1u));
    }
    return 1;
}

/* Probe EC at current ports with diagnostic state updates.
 * Returns: 1=found, 0=not found.
 * Updates g_ec_probe_step/status/attempts for on-screen diagnostics. */
static int ec_probe_verbose(int attempt) {
    uint8_t s = inb(g_cmd_port);
    g_ec_probe_status = s;
    g_ec_probe_attempts = (uint8_t)attempt;

    if (s == 0xFF) {
        g_ec_probe_step = EC_STEP_STS_FF;
        return 0;
    }

    ec_drain_obf();

    /* Step 1: wait for IBF clear before sending command */
    if (!ec_wait_ibf_clear()) {
        g_ec_probe_step = EC_STEP_IBF_PRE;
        return 0;
    }

    /* Step 2: send READ command */
    outb(g_cmd_port, EC_CMD_READ);

    /* Step 3: wait for IBF clear after command */
    if (!ec_wait_ibf_clear()) {
        g_ec_probe_step = EC_STEP_IBF_POST;
        return 0;
    }

    /* Step 4: send address byte */
    outb(g_data_port, 0x00);

    /* Step 5: wait for OBF (data ready) */
    if (!ec_wait_obf_set()) {
        g_ec_probe_step = EC_STEP_OBF_TIMEOUT;
        return 0;
    }

    (void)inb(g_data_port);
    g_ec_probe_step = EC_STEP_OK;
    return 1;
}

static int ec_probe_with_retries(int log_result) {
    uint32_t retries = g_ec_policy.probe_retries;
    for (uint32_t i = 0; i < retries; i++) {
        if (i > 0) {
            ec_busy_wait(g_ec_policy.retry_delay);
        }
        if (ec_probe_verbose((int)(i + 1))) {
            if (log_result) {
                kprint("ACPI: EC probe ok ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
                       g_data_port, g_cmd_port, (uint32_t)(i + 1), (uint32_t)g_ec_probe_status);
            }
            return 1;
        }
        /* Fast bail: 0xFF = floating bus, retrying won't help */
        if (g_ec_probe_step == EC_STEP_STS_FF) break;
    }
    if (log_result) {
        kprint("ACPI: EC probe fail ports=0x%02x/0x%02x tries=%u step=%s sts=0x%02x\n",
               g_data_port, g_cmd_port, retries,
               ec_probe_step_name(g_ec_probe_step), (uint32_t)g_ec_probe_status);
    }
    return 0;
}

/*
 * ec_setup_ports: set ports and enable ACPI mode WITHOUT probing.
 * Call this before _REG so that any EC I/O from AML uses correct ports.
 * Sets g_ec_ok=1 optimistically so ec_read/ec_write work during _REG.
 */
void ec_setup_ports(uint16_t data_port, uint16_t cmd_port) {
    g_data_port = data_port;
    g_cmd_port = cmd_port;
    g_ec_timeout_reported = 0;
    g_ec_consec_fail = 0;
    ec_try_acpi_enable();
    g_ec_ok = 1;  /* optimistic: allow ec_read/ec_write during _REG */
    kprint("ACPI: EC ports set to 0x%02x/0x%02x (pre-REG)\n",
           g_data_port, g_cmd_port);
}

/*
 * ec_probe: probe the EC at currently configured ports.
 * Returns 0 on success, -1 on failure.
 * Tries primary ports, then alternate Dell ports 0x68/0x6C.
 */
int ec_probe(void) {
    uint16_t orig_data = g_data_port;
    uint16_t orig_cmd = g_cmd_port;
    uint16_t fail_data = g_data_port;
    uint16_t fail_cmd = g_cmd_port;

    if (ec_probe_with_retries(0)) {
        g_ec_ok = 1;
        g_ec_consec_fail = 0;
        kprint("ACPI: EC probe success ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
               g_data_port, g_cmd_port,
               (uint32_t)g_ec_probe_attempts, (uint32_t)g_ec_probe_status);
        return 0;
    }
    fail_data = g_data_port;
    fail_cmd = g_cmd_port;

    /* Fast bail: 0xFF = no device on bus, skip alternate port combos */
    if (g_ec_probe_step == EC_STEP_STS_FF) {
        g_ec_ok = 0;
        kprint("ACPI: EC probe STS_FF on 0x%02x/0x%02x — skipping alternates\n",
               g_data_port, g_cmd_port);
        return -1;
    }

    /*
     * Some firmware/resource templates present command/data ordering
     * ambiguously. For legacy low I/O EC windows, try one swapped probe.
     */
    if (g_ec_policy.try_swapped_ports &&
        orig_data <= 0xFFu && orig_cmd <= 0xFFu &&
        (orig_data == (uint16_t)(orig_cmd + 4u) || orig_cmd == (uint16_t)(orig_data + 4u))) {
        g_data_port = orig_cmd;
        g_cmd_port = orig_data;
        if (ec_probe_with_retries(0)) {
            g_ec_ok = 1;
            g_ec_consec_fail = 0;
            kprint("ACPI: EC probe success ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
                   g_data_port, g_cmd_port,
                   (uint32_t)g_ec_probe_attempts, (uint32_t)g_ec_probe_status);
            return 0;
        }
        fail_data = g_data_port;
        fail_cmd = g_cmd_port;
        g_data_port = orig_data;
        g_cmd_port = orig_cmd;
    }

    /* Try alternate Dell ports if primary failed */
    if (g_ec_policy.try_alternate_ports &&
        (g_data_port != 0x68 || g_cmd_port != 0x6C)) {
        g_data_port = 0x68;
        g_cmd_port = 0x6C;
        if (ec_probe_with_retries(0)) {
            g_ec_ok = 1;
            g_ec_consec_fail = 0;
            kprint("ACPI: EC probe success ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
                   g_data_port, g_cmd_port,
                   (uint32_t)g_ec_probe_attempts, (uint32_t)g_ec_probe_status);
            return 0;
        }
        fail_data = g_data_port;
        fail_cmd = g_cmd_port;

        /*
         * Same ordering fallback for alternate legacy pair.
         */
        if (g_ec_policy.try_swapped_ports) {
            g_data_port = 0x6C;
            g_cmd_port = 0x68;
            if (ec_probe_with_retries(0)) {
                g_ec_ok = 1;
                g_ec_consec_fail = 0;
                kprint("ACPI: EC probe success ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
                       g_data_port, g_cmd_port,
                       (uint32_t)g_ec_probe_attempts, (uint32_t)g_ec_probe_status);
                return 0;
            }
            fail_data = g_data_port;
            fail_cmd = g_cmd_port;
        }
    }

    /* Restore original ports for diagnostics display */
    g_data_port = orig_data;
    g_cmd_port = orig_cmd;
    g_ec_ok = 0;
    kprint("ACPI: EC probe failed all candidates (last=0x%02x/0x%02x step=%s sts=0x%02x)\n",
           fail_data, fail_cmd,
           ec_probe_step_name(g_ec_probe_step), (uint32_t)g_ec_probe_status);
    return -1;
}

int ec_init_ports(uint16_t data_port, uint16_t cmd_port) {
    g_ec_ok = 0;
    g_ec_timeout_reported = 0;
    g_ec_consec_fail = 0;
    g_data_port = data_port;
    g_cmd_port = cmd_port;
    ec_try_acpi_enable();

    kprint("ACPI: EC init using ports 0x%02x/0x%02x\n", g_data_port, g_cmd_port);
    if (ec_probe_with_retries(0)) {
        g_ec_ok = 1;
        kprint("ACPI: EC probe success ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
               g_data_port, g_cmd_port,
               (uint32_t)g_ec_probe_attempts, (uint32_t)g_ec_probe_status);
        return 0;
    }

    /* Try alternate Dell ports if primary failed */
    if (g_ec_policy.try_alternate_ports &&
        (g_data_port != 0x68 || g_cmd_port != 0x6C)) {
        g_data_port = 0x68;
        g_cmd_port = 0x6C;
        if (ec_probe_with_retries(0)) {
            g_ec_ok = 1;
            kprint("ACPI: EC probe success ports=0x%02x/0x%02x tries=%u sts=0x%02x\n",
                   g_data_port, g_cmd_port,
                   (uint32_t)g_ec_probe_attempts, (uint32_t)g_ec_probe_status);
            return 0;
        }
    }

    /* Restore original ports for diagnostics display */
    g_data_port = data_port;
    g_cmd_port = cmd_port;
    kprint("ACPI: EC probe failed all candidates (last=0x%02x/0x%02x step=%s sts=0x%02x)\n",
           g_data_port, g_cmd_port,
           ec_probe_step_name(g_ec_probe_step), (uint32_t)g_ec_probe_status);
    return -1;
}

int ec_init(void) {
    return ec_init_ports(0x62, 0x66);
}

int ec_available(void) {
    return g_ec_ok;
}

void ec_get_ports(uint16_t *data_port, uint16_t *cmd_port) {
    if (data_port) *data_port = g_data_port;
    if (cmd_port)  *cmd_port  = g_cmd_port;
}

void ec_get_probe_diag(uint8_t *step, uint8_t *status, uint8_t *attempts) {
    if (step)     *step     = g_ec_probe_step;
    if (status)   *status   = g_ec_probe_status;
    if (attempts) *attempts = g_ec_probe_attempts;
}

void ec_reset_fail_count(void) {
    g_ec_consec_fail = 0;
    g_ec_timeout_reported = 0;
}

int ec_last_probe_stsff(void) {
    return (g_ec_probe_step == EC_STEP_STS_FF);
}

void ec_set_reg_in_progress(int flag) {
    if (flag) {
        g_ec_reg_suppressed = 0;
    }
    g_ec_reg_in_progress = flag;
}

int ec_is_reg_in_progress(void) {
    return g_ec_reg_in_progress;
}

uint32_t ec_get_reg_suppressed_count(void) {
    return g_ec_reg_suppressed;
}

/* --- Policy accessors (G443) --- */

const ec_policy_t *ec_policy_get(void) {
    return &g_ec_policy;
}

void ec_policy_set(const ec_policy_t *p) {
    if (!p) return;
    g_ec_policy = *p;
}

void ec_policy_defaults(ec_policy_t *out) {
    if (!out) return;
    out->ibf_obf_timeout    = 10000;
    out->probe_retries      = 2;
    out->retry_delay        = 200000;
    out->max_consec_fail    = 4;
    out->acpi_enable_wait   = 500000;
    out->force_enable_wait  = 5000000;
    out->burst_disable_wait = 100000;
    out->obf_drain_max      = 64;
    out->sci_query_drain_max = 4;
    out->try_alternate_ports = 1;
    out->try_swapped_ports   = 1;
    out->suppress_reg_io     = 1;
    out->_pad[0]             = 0;
}

/* --- Set EC namespace node for _Qxx dispatch (G451) --- */

void ec_set_ns_node(struct acpi_node *node) {
    g_ec_ns_node = node;
}

/* --- EC Query/Event Queue (G451) --- */

void ec_enqueue_query(uint8_t val) {
    if (g_ec_events_frozen) {
        ec_queries_dropped++;
        return;
    }
    /* Storm detection (G452) */
    g_ec_queries_this_tick++;
    if (g_ec_queries_this_tick > EC_STORM_THRESHOLD) {
        g_ec_storm_count++;
        ec_queries_dropped++;
        if (g_ec_storm_count == 1) {
            kprint("ACPI: EC: event storm detected (>%u queries/tick)\n",
                   (uint32_t)EC_STORM_THRESHOLD);
        }
        return;
    }

    uint32_t next = (ec_query_head + 1) % EC_QUERY_QUEUE_SIZE;
    if (next == ec_query_tail) {
        /* Queue full — drop */
        ec_queries_dropped++;
        return;
    }
    ec_query_queue[ec_query_head] = val;
    ec_query_head = next;
}

int ec_dequeue_query(uint8_t *val) {
    if (!val) return 0;
    if (ec_query_tail == ec_query_head) return 0;
    *val = ec_query_queue[ec_query_tail];
    ec_query_tail = (ec_query_tail + 1) % EC_QUERY_QUEUE_SIZE;
    return 1;
}

/*
 * EC GPE handler — called from SCI interrupt context.
 * Reads EC status, if SCI_EVT is set, sends QR_EC (0x84) to get query
 * value, and enqueues it for the worker thread.
 */
void ec_gpe_handler(uint32_t gpe) {
    (void)gpe;
    if (!g_ec_ok) return;

    uint8_t sts = inb(g_cmd_port);
    if (!(sts & EC_SCI_EVT)) return;

    /* Send QR_EC command to get query value */
    if (sts & EC_IBF) return; /* IBF busy, skip this event */
    outb(g_cmd_port, EC_CMD_SCI_QUERY);

    /* Wait for OBF — short spin, we're in interrupt context */
    for (int i = 0; i < 10000; i++) {
        if (inb(g_cmd_port) & EC_OBF) {
            uint8_t qval = inb(g_data_port);
            if (qval != 0) {
                ec_enqueue_query(qval);
            }
            return;
        }
        __asm__ volatile("pause");
    }
}

/*
 * EC query worker thread — dispatches _Qxx methods from the queue.
 * Modeled from F96 kernel's kec_query worker.
 */
static void ec_query_worker_entry(void *arg) {
    (void)arg;
    for (;;) {
        uint8_t val;
        int did_work = 0;

        /* Reset per-tick storm counter */
        g_ec_queries_this_tick = 0;

        while (ec_dequeue_query(&val)) {
            did_work = 1;
            ec_queries_dispatched++;

            /* Build _Qxx method name */
            if (g_ec_ns_node) {
                const char *hex = "0123456789ABCDEF";
                char qname[5];
                qname[0] = '_';
                qname[1] = 'Q';
                qname[2] = hex[(val >> 4) & 0xF];
                qname[3] = hex[val & 0xF];
                qname[4] = 0;

                struct acpi_node *qmethod = ns_find_child(g_ec_ns_node, qname);
                if (qmethod) {
                    char fullpath[256];
                    ns_build_path(qmethod, fullpath, sizeof(fullpath));
                    aml_eval(fullpath);
                }
            }
        }

        /* Get current process for sleep */
        struct fry_process *cur = proc_current();
        if (cur && !did_work) {
            sched_sleep(cur->pid, 50);
        }
        sched_yield();
    }
}

void ec_query_worker_start(void) {
    if (ec_query_worker_running) return;
    struct fry_process *p = process_create_kernel(ec_query_worker_entry, 0, "ec_query");
    if (p) {
        sched_add(p->pid);
        ec_query_worker_running = 1;
        kprint("ACPI: EC query worker started\n");
    }
}

uint32_t ec_get_queries_dispatched(void) {
    return ec_queries_dispatched;
}

uint32_t ec_get_queries_dropped(void) {
    return ec_queries_dropped;
}

/* --- Storm guard + freeze/resume (G452) --- */

void ec_freeze_events(void) {
    g_ec_events_frozen = 1;
    kprint("ACPI: EC: event blocked\n");
}

void ec_unfreeze_events(void) {
    g_ec_events_frozen = 0;
    kprint("ACPI: EC: event unblocked\n");
}

int ec_events_frozen(void) {
    return g_ec_events_frozen;
}

uint32_t ec_get_storm_count(void) {
    return g_ec_storm_count;
}
