# TaterTOS64v3 Handoff — fry416 (2026-02-28)

## Hardware Lock (Non-Negotiable)
- Platform: Dell Precision 7530
- CPU: Intel Core i5-8300H (Coffee Lake-H)
- Firmware: latest available BIOS for this exact model
- Rule: all EC/eSPI/P2SB/ACPI validation is anchored to this hardware first; record BIOS revision/date in logs when available.
- Rule: OS design/behavior stays hardware-agnostic; any hardware-specific handling must be runtime-detected and isolated.

## What Was Done (fry416)

Fixed EC battery failure caused by wrong PCR sideband PID on Dell Precision 7530.

### Root Cause
fry407 re-introduced `PID_DMI_CANNONLAKE` (0x88) as the priority candidate for
Dell A30E (LPC device 0xA30E), undoing fry399's fix. The correct PID is 0xEF
(`PID_DMI_DEFAULT`), confirmed by coreboot `cannonlake/pcr_ids.h`.

**Failure chain**: Wrong PID → PCR mirror write to wrong sideband address →
EC decode bits never enabled → EC returns 0xFF → battery `_STA=0x00000000` → all zeros.

### Changes Made

**File: `src/kernel/acpi/extended.c`**

1. **`pcr_get_dmi_pid_candidates()` (line 663)** — Swapped candidate order for
   Dell A30E branch: `PID_DMI_DEFAULT` (0xEF) is now `out[0]` (first/priority),
   `PID_DMI_CANNONLAKE` (0x88) demoted to `out[1]` (fallback). Comment updated
   to correctly say "Coffee Lake i5-8300H, CM246 PCH" (not "Cannon Lake").

2. **`ec_mirror_pcr_ioe()` probe loop (line 933)** — Added tie-breaking logic:
   when two PIDs produce equal scores, prefer `PID_DMI_DEFAULT` (0xEF). Prevents
   0x88 from winning by position when both PIDs happen to score the same.

### Build Status
- ISO built successfully via `make clean` + `build_iso.sh`
- Exported to: `C:\Users\jjsako\Documents\tateriso\tatertos64v3.iso`

## What Needs Testing

Flash ISO to USB, boot on Dell Precision 7530. Verify in serial log / sysinfo:

- [ ] `PCR PID=0xef` (not 0x88)
- [ ] `IOE: 0x0000XXXX -> 0x00003f0f` (EC decode bits set after mirror write)
- [ ] `EC sts after PCR mirror: 0xXX` where XX != 0xFF
- [ ] Battery fields non-zero (mV, rate, remain)
- [ ] `Batt: STA != 0x00000000`

## Current State

- Next fry log: **fry417.txt**
- All other subsystems unchanged from prior state (see MEMORY.md for full list)
- Boot is stable; GUI, NVMe, keyboard, mouse, TaterWin SHM all working

## Key Files for Context
- `src/kernel/acpi/extended.c` — PCR sideband / EC decode logic
- `src/user/apps/sysinfo.c` — TaterWin app that displays battery/EC info
- `logs/fry416.txt` — detailed change log for this session
