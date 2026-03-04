#ifndef TATER_ACPI_EC_H
#define TATER_ACPI_EC_H

#include <stdint.h>

/*
 * ACPI Embedded Controller (EC) interface
 */

/*
 * EC policy struct — all hardcoded values as tunable knobs.
 * Modeled from F96-CE kernel behavioral analysis.
 */
typedef struct ec_policy {
    uint32_t ibf_obf_timeout;       /* max iterations for IBF/OBF waits */
    uint32_t probe_retries;         /* number of probe retry attempts */
    uint32_t retry_delay;           /* busy-loop iterations between retries */
    uint32_t max_consec_fail;       /* consecutive failures before disabling EC */
    uint32_t acpi_enable_wait;      /* busy-loop iters in ec_try_acpi_enable */
    uint32_t force_enable_wait;     /* busy-loop iters in ec_force_acpi_enable */
    uint32_t burst_disable_wait;    /* busy-loop iters in ec_burst_disable */
    uint32_t obf_drain_max;         /* max bytes to drain from OBF */
    uint32_t sci_query_drain_max;   /* max SCI_QUERY iterations */
    uint8_t  try_alternate_ports;   /* 1 = try 0x68/0x6C fallback */
    uint8_t  try_swapped_ports;     /* 1 = try data/cmd swap probe */
    uint8_t  suppress_reg_io;       /* 1 = suppress EC I/O during _REG */
    uint8_t  _pad[1];
} ec_policy_t;

const ec_policy_t *ec_policy_get(void);
void ec_policy_set(const ec_policy_t *p);
void ec_policy_defaults(ec_policy_t *out);

int ec_init(void);
int ec_init_ports(uint16_t data_port, uint16_t cmd_port);
void ec_setup_ports(uint16_t data_port, uint16_t cmd_port);
int ec_probe(void);
int ec_read(uint8_t addr, uint8_t *out);
int ec_write(uint8_t addr, uint8_t val);
int ec_available(void);
void ec_get_ports(uint16_t *data_port, uint16_t *cmd_port);
void ec_get_probe_diag(uint8_t *step, uint8_t *status, uint8_t *attempts);
int  ec_last_probe_stsff(void);
void ec_reset_fail_count(void);

/* Recovery primitives for Dell EC handoff */
void ec_reset_state(void);
int  ec_burst_enable(void);
void ec_burst_disable(void);
void ec_sci_query_drain(void);
int  ec_force_acpi_enable(void);

/*
 * _REG suppression: while _REG(3,1) is evaluating, ec_read/ec_write must
 * NOT send real port I/O.  Dell Precision 7530 EC desyncs if it receives
 * READ/WRITE commands before the _REG handshake completes.  These let
 * extended.c bracket the _REG call so aml_ops.c region handler returns
 * dummy data instead of touching the hardware.
 */
void ec_set_reg_in_progress(int flag);
int  ec_is_reg_in_progress(void);
uint32_t ec_get_reg_suppressed_count(void);

/* Set EC namespace node for _Qxx dispatch */
struct acpi_node;
void ec_set_ns_node(struct acpi_node *node);

/* EC query/event queue (G451) */
void ec_enqueue_query(uint8_t val);
int  ec_dequeue_query(uint8_t *val);
void ec_gpe_handler(uint32_t gpe);
void ec_query_worker_start(void);
uint32_t ec_get_queries_dispatched(void);
uint32_t ec_get_queries_dropped(void);

/* Storm guard + freeze/resume (G452) */
void ec_freeze_events(void);
void ec_unfreeze_events(void);
int  ec_events_frozen(void);
uint32_t ec_get_storm_count(void);

#endif
