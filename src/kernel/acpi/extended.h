#ifndef TATER_ACPI_EXTENDED_H
#define TATER_ACPI_EXTENDED_H

#include <stdint.h>

struct fry_battery_status {
    uint32_t state;
    uint32_t present_rate;
    uint32_t remaining_capacity;
    uint32_t present_voltage;
};

void acpi_extended_init(void);
int acpi_backlight_set(uint32_t percent);
uint32_t acpi_backlight_get(void);
int acpi_battery_get(struct fry_battery_status *out);
void acpi_nvme_dsm_for_pci(uint8_t bus, uint8_t slot, uint8_t func);
void acpi_battery_refresh(void);
void acpi_thermal_poll_once(void);
void acpi_backlight_refresh(void);

struct fry_acpi_diag {
    /* Namespace */
    uint32_t ns_nodes;
    /* EC */
    uint8_t  ec_ok;
    uint8_t  _pad0[1];
    uint16_t ec_data_port;
    uint16_t ec_cmd_port;
    uint16_t _pad1;
    /* Battery */
    uint8_t  batt_count;
    uint8_t  _pad2[3];
    uint32_t batt_sta;
    char     batt_path[64];
    /* Backlight */
    uint8_t  bl_found;
    uint8_t  _pad3[3];
    uint32_t bl_sta;
    uint32_t bl_bcl_count;
    uint32_t bl_supported_count;
    uint32_t bl_raw;
    uint32_t bl_percent;
    char     bl_path[64];
    /* EC Diagnostics */
    uint8_t  ec_probe_step;      /* 0=not_probed 1=sts_ff 2=ibf_pre 3=ibf_post 4=obf 5=ok */
    uint8_t  ec_probe_status;    /* raw status byte at probe time */
    uint8_t  ec_probe_attempts;  /* number of attempts made */
    uint8_t  ec_node_found;      /* PNP0C09 found */
    uint8_t  ec_reg_called;      /* _REG(3,1) invoked */
    uint8_t  ec_ini_found;       /* _INI found and called */
    uint8_t  ec_gpe_found;       /* _GPE found */
    uint8_t  ec_gpe_num;         /* GPE number (0xFF = none) */
    uint16_t lpc_ioe_before;     /* 00:1f.0 IOE register (offset 0x82) before EC decode enable */
    uint16_t lpc_ioe_after;      /* 00:1f.0 IOE register (offset 0x82) after EC decode enable */
    uint8_t  ec_ports_source;    /* 0=default 1=ECDT 2=_CRS */
    uint8_t  lpc_bus;
    uint8_t  lpc_slot;
    uint8_t  lpc_func;
    uint16_t lpc_vendor;
    uint16_t lpc_device;
    uint8_t  lpc_class_code;
    uint8_t  lpc_subclass;
    uint8_t  lpc_prog_if;
    uint8_t  lpc_write_attempted; /* safety gates passed for IOE programming */
    uint8_t  lpc_write_performed; /* a write to config 0x80 was issued */
    uint8_t  _pad4[3];
    uint16_t lpc_cmd;
    uint16_t _pad5;
    uint32_t lpc_reg80_before;
    uint32_t lpc_reg80_after;
    uint32_t lpc_reg84;
    uint32_t lpc_reg88;
    /* PCR[DMI] sideband mirror diagnostics (fry398, fixed fry399) */
    uint32_t pcr_ioe_before;    /* PCR[DMI]+0x2774 before mirror write */
    uint32_t pcr_ioe_after;     /* PCR[DMI]+0x2774 after mirror write */
    uint8_t  pcr_mirror_done;   /* 1 if PCR mirror write was performed */
    uint8_t  p2sb_hidden;       /* P2SB was hidden, had to unhide */
    uint8_t  ec_early_sts;      /* port 0x66 status BEFORE any init */
    uint8_t  ec_post_lpc_sts;   /* port 0x66 status AFTER LPC decode config */
    uint8_t  pcr_pid;           /* sideband PID used (0xEF=DMI correct) */
    uint8_t  ec_pre_reg_sts;    /* port 0x66 status right before _REG */
    uint8_t  ec_pre_reg_probe_ok;   /* 1 if pre-_REG probe succeeded */
    uint8_t  ec_recovery_method;    /* 0=none 1=normal 2=state_reset 3=burst
                                       4=sci_drain 5=force_enable 6=burst+force */
    uint8_t  _pad6[1];
    uint16_t ec_reg_suppressed;     /* EC I/O calls suppressed during _REG */
    /* fry421: immediate raw probe diagnostics */
    uint8_t  ec_imm_step;      /* 0=not_tried 1=STS_FF 2=IBF_PRE 3=IBF_POST 4=OBF_TMO 5=OK */
    uint8_t  ec_imm_val;       /* reg[0] value if imm_step==5 */
    uint8_t  ec_imm_post_sts;  /* port 0x66 after immediate probe attempt */
    uint8_t  ec_post_setup_sts; /* port 0x66 after ec_setup_ports */
    /* fry422: floating bus detection + force SMI */
    uint8_t  ec_ibf_seen;      /* 1 if IBF was observed after cmd write (real EC, not floating) */
    uint8_t  ec_pre_data;      /* inb(0x62) before any command (pending data) */
    uint8_t  ec_smi_sent;      /* 1 if force ACPI_ENABLE was sent before re-probe */
    uint8_t  ec_imm2_step;     /* result of Phase 1 re-probe (after SMI) same encoding as imm_step */
    /* fry423: Gen3/Gen4 decode + eSPI diagnostics */
    uint32_t lpc_reg8c;        /* Generic I/O Decode Range 3 (offset 0x8C) */
    uint32_t lpc_reg90;        /* Generic I/O Decode Range 4 (offset 0x90) */
    uint32_t espi_raw[8];      /* [0]=CFG_VAL [1]=PCBC [2]=PCERR [3]=VWERR
                                   [4]=FCERR [5]=LNKERR [6]=SLV_CTL [7]=GenCfg */
    uint8_t  espi_probed;      /* 1 if eSPI PCR was probed */
    uint8_t  espi_pid;         /* PID that worked for eSPI PCR access */
    uint8_t  espi_en;          /* 1 if ESPI_EN strap set (D31:F0+0xDC bit 2) */
    uint8_t  _pad7[1];
    /* fry435: eSPI W1C error clear diagnostics */
    uint32_t espi_pre_clear[4];   /* PCERR/VWERR/FCERR/LNKERR before first clear */
    uint32_t espi_post_clear[4];  /* after most recent clear */
    uint8_t  espi_clear_run;      /* bit 0=proactive, bit 1=recovery */
    uint8_t  espi_clear_found;    /* 1 if errors were non-zero */
    uint8_t  espi_clear_ok;       /* 1 if all regs zero after clear */
    uint8_t  _pad8[1];
    /* G445: EC policy exposure */
    uint32_t ec_policy_timeout;     /* ibf_obf_timeout from policy */
    uint8_t  ec_policy_retries;     /* probe_retries from policy */
    uint8_t  ec_policy_max_fail;    /* max_consec_fail from policy */
    uint8_t  ec_policy_flags;       /* bit0=alternate bit1=swapped bit2=suppress_reg */
    uint8_t  _pad9;
    /* G451/G452: EC query/event diagnostics */
    uint32_t ec_queries_dispatched;
    uint32_t ec_queries_dropped;
    uint32_t ec_storm_count;
    /* G446: candidate source info */
    uint8_t  ec_cand_count;         /* number of EC candidates found */
    uint8_t  ec_best_cand_source;   /* 0=fallback 1=ECDT 2=CRS */
    uint8_t  ec_events_frozen;      /* 1 if events frozen */
    uint8_t  _pad10;
    /* fry444: eSPI slave channel diagnostics */
    uint32_t espi_slave_pc_cap;     /* Peripheral Channel Cap (slave reg 0x0010) */
    uint32_t espi_slave_vw_cap;     /* Virtual Wire Channel Cap (slave reg 0x0020) */
    uint8_t  espi_slave_pc_en;      /* PC Ready bit from slave */
    uint8_t  espi_slave_vw_en;      /* VW Ready bit from slave */
    uint8_t  espi_gen_chan_sup;      /* GenCfg[15:12] channel support: b0=PC b1=VW b2=OOB b3=FC */
    uint8_t  espi_slave_read_ok;    /* bitmask: b0=genCfg b1=pcCap b2=vwCap read succeeded */
    /* fry446: eSPI channel initialization diagnostics */
    uint8_t  espi_chinit_result;    /* 0=not_run 1=already_ready 2=toggled_ok 3=partial 4=failed */
    uint8_t  espi_pltrst_state;     /* 0xFF=not_found; else: b0=data b1=valid (for PLT_RST# wire) */
    uint8_t  espi_pltrst_sent;      /* 1 if PLT_RST de-assertion was attempted */
    uint8_t  espi_chinit_retries;   /* number of toggle retries actually attempted */
    uint32_t espi_chinit_pc_cap;    /* PC_CAP register value after channel init */
    uint32_t espi_chinit_vw_cap;    /* VW_CAP register value after channel init */
};

int acpi_get_diag(struct fry_acpi_diag *out);

#endif
