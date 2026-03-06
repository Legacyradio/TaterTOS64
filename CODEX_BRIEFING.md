# TaterTOS64v3 — Codex Handoff Briefing
**Date:** 2026-02-23  
**Written by:** Claude (the AI that architected and built this OS from scratch)  
**For:** Codex — continuing the build

---

## Current Resume Point (2026-02-26)

- Last completed implementation log: `logs/fry347.txt`.
- Root issue being debugged on bare metal:
  - GUI/SHELL launch failed with open error `-101` while VFS mounted the wrong FAT32 volume.
- Fix applied:
  - `src/kernel/fs/vfs.c` now scores FAT32 candidates across root, `/fry`, `/FRY`, `/EFI/FRY`, and `/EFI/BOOT/FRY` path patterns for GUI/SHELL payloads.
  - `src/boot/efi_loader.c` now scans additional EFI-subdir path variants (`\EFI\FRY\...`, `\EFI\BOOT\FRY\...`) when building the boot ramdisk.
  - `src/kernel/main.c` now tries expanded launch fallbacks (`/FRY`, `/EFI/FRY`, `/EFI/BOOT/FRY`) and emits richer DBG_VFS stat probes.
  - Added boot debug marker:
    - `ERROR: DBG_VFS picked_lba=<lba> userspace_score=<score>`
- Required rebuild/test flow (planv3 rule 11) was run and passed:
  - `make clean`
  - `PATH="/opt/cross/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH" /bin/bash -x build_iso.sh`
- After reboot, verify these lines in latest boot log:
  - `ERROR: DBG_VFS picked_lba=... userspace_score=...`
  - `VFS: FAT32 mounted at LBA ...`
  - `init: user pid=...` (or launch failure marker with code/path)
- User interaction preference for this project:
  - User prefers blunt, sassy communication.
  - Swearing/cursing is acceptable and encouraged by user.

---

## What This Project Is

TaterTOS64v3 is a ground-up 64-bit operating system for the **Dell Precision 7530**
(Intel i7-8750H Coffee Lake, UEFI). It is **not** a Linux fork or modification —
it is entirely original code with:

- Full ACPI subsystem including AML bytecode interpreter
- Linux-style bus/device/driver model
- Custom `.fry` user-space binary format (fry_header + ELF64)
- Round-robin scheduler with full user/kernel separation
- FAT32/GPT on NVMe (via VMD unwrapping)
- Graphical GUI with mouse cursor and app launcher
- PS/2 keyboard + mouse drivers via IOAPIC routing
- Interactive shell with ls, cat, echo, cd, pwd, spawn

---

## The Rules (planv3.txt — you MUST follow these)

1. Follow the plan step by step. No shortcuts or cut corners.
2. No "minimal" implementations; do thorough, functional work.
3. **No Linux or Windows code inside the OS.**
4. Use Edit/Write tools for code edits.
5. **Log every completed step in its own `logs/fryN.txt` with timestamps.**
6. If you change code, include a brief "why" in the log.
7. Don't pause after saying you'll continue; just continue.
8. Branding is **TaterTOS64v3** (never FryOS, never TatertOS).
9. UEFI-only boot path.
10. When testing, always include the necessary commands to run the test.

**Next fry log: fry348.txt**

---

## Build Commands

```bash
# Full build + ISO
PATH="/opt/cross/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH" /bin/bash build_iso.sh

# Kernel only
PATH="/opt/cross/bin:$PATH" make kernel

# Verify NVMe image contents
mdir -i out/tater_nvme.img@@1048576 ::

# Copy a .fry to NVMe
mcopy -i out/tater_nvme.img@@1048576 -o shell.fry ::/SHELL.FRY
```

Build produces:
- `out/tatertos64v3.iso` — bootable ISO
- `out/tater_nvme.img` — NVMe disk image with FAT32 partition

ISO is automatically exported to `/mnt/c/Users/jjsako/Documents/tateriso/tatertos64v3.iso`

---

## Key Source Files

| File | Purpose |
|------|---------|
| `src/kernel/irq/irqdesc.c` | IRQ descriptor table + `common_isr` asm stub |
| `src/kernel/proc/process.c` | process_launch, process_create_user, setup_initial_stack |
| `src/kernel/proc/sched.c` | round-robin scheduler, context_switch |
| `src/kernel/proc/syscall.c` | syscall_entry asm + syscall_dispatch |
| `src/kernel/proc/elf.c` | .fry loader (strips fry_header, loads ELF64) |
| `src/kernel/fs/vfs.c` | VFS mount/open/read/readdir |
| `src/kernel/fs/fat32.c` | FAT32 driver |
| `src/drivers/input/ps2_kbd.c` | PS/2 keyboard, Set-1 scancodes |
| `src/drivers/input/ps2_mouse.c` | PS/2 mouse, 3-byte packets, x/y/btns |
| `src/drivers/irqchip/lapic.c` | LAPIC driver |
| `src/drivers/irqchip/ioapic.c` | IOAPIC routing |
| `src/drivers/timer/lapic_timer.c` | LAPIC timer, sched_tick |
| `src/drivers/storage/nvme.c` | NVMe driver (VMD-aware) |
| `src/user/shell/shell.c` | Interactive shell |
| `src/user/gui/gui.c` | Graphical GUI launcher + app output windows |
| `src/user/libc/libc.c` | Userspace libc + syscall wrappers |
| `src/user/libc/libc.h` | Libc header |
| `src/user/apps/sysinfo.c` | SYSINFO app |
| `src/user/apps/uptime.c` | UPTIME app |
| `src/user/apps/ps.c` | PS app |
| `tools/frypack.py` | Wraps ELF64 in fry_header (magic FRY0 + CRC32) |
| `planv3.txt` | The master plan — READ THIS for full detail |

---

## Architecture Notes

### Memory
- `VMM_PHYSMAP_BASE = 0xFFFF800000000000` — kernel physical map base
- User processes mapped at low virtual addresses
- Each process has its own page table (CR3)

### Interrupts
- `common_isr` in `irqdesc.c` saves all GP regs, saves user CR3 in **%r15**
  (callee-saved — safe across context switches), switches to kernel page table,
  calls `irq_dispatch`, then restores CR3 from %r15 and iretq
- LAPIC timer vector = 0x40, fires every ~10ms, calls `sched_tick` on BSP

### Syscalls
- `syscall` instruction, handled by `syscall_entry` asm in `syscall.c`
- Switches to per-process kernel stack (`g_syscall_kstack_top`) immediately
- Numbers: SYS_WRITE=0, SYS_READ=1, SYS_EXIT=2, SYS_SPAWN=3, SYS_SLEEP=4,
  SYS_OPEN=5, SYS_CLOSE=6, SYS_GETPID=7, SYS_STAT=8, SYS_READDIR=9,
  SYS_GETTIME=10, SYS_REBOOT=11, SYS_SHUTDOWN=12, SYS_WAIT=13,
  SYS_PROCCOUNT=14, SYS_SETBRIGHT=15, SYS_GETBRIGHT=16, SYS_GETBATTERY=17,
  SYS_FB_INFO=18, SYS_FB_MAP=19, SYS_MOUSE_GET=20, SYS_PROC_OUTPUT=21

### Per-Process stdout Ring Buffer
- `struct fry_process` has `outbuf[512]`, `outbuf_head`, `outbuf_tail`
- SYS_WRITE(fd=1) appends to both serial and ring buffer
- SYS_PROC_OUTPUT(21) drains ring buffer; returns -2 when process dead+empty
- GUI polls this each frame to render app output in the right-side content window

### .fry Binary Format
- 16-byte header: magic `FRY0` + CRC32 of ELF + size fields
- Followed by standard ELF64 binary
- Built by `tools/frypack.py`

---

## Milestone Status

### DONE (confirmed working)
- M01–M17: All foundational infrastructure (boot, PMM, ACPI, IRQ, APIC, HPET,
  LAPIC timer, DSDT/AML, PCI, SMP, NVMe/VMD, FAT32/GPT, PS/2 kbd+mouse)
- M20: shell.fry loads from disk, shell prompt appears
- M21: ls, cat, echo, cd, pwd all work in shell
- M22: spawn second .fry, both run concurrently (GUI + shell + apps)
- M26: Battery — fry_getbattery syscall implemented
- M27: Backlight — fry_setbrightness/fry_getbrightness syscalls implemented
- M28: ISO builds with single command
- M29: Triple fault fix (CR3 in %r15) + app output windows (fry322)

### NEEDS VERIFICATION (code exists, not yet tested)
- M18: XHCI USB enumeration + USB HID — `src/drivers/usb/` — test on real HW
- M19: USB hub downstream detection — test on real HW
- M23: e1000 ICMP ping in QEMU — add `-netdev user,id=n0 -device e1000,netdev=n0`
- M24: WiFi 9260 firmware ALIVE state — real hardware only
- M25: Thermal `_TMP` AML eval — call from a test .fry app

---

## What Remains (in priority order)

### 1. Verify remaining milestones (M23, M25 first — QEMU-testable)
- M23: Boot QEMU with e1000, check serial for MAC/link, write ping test .fry
- M25: Write a small .fry that calls fry_getbattery/thermal AML, verify result
- M18/M19/M24: Real hardware — test after QEMU passes

### 2. GUI Polish
- **Font rendering**: Current text is pixel-by-pixel; implement an 8x16 bitmap font
  for crisp readable text in the app output window and GUI labels
- **Window chrome**: Add proper borders/shadows to content window
- **App icons**: Replace text buttons with small icon graphics for SHELL/SYSINFO/etc
- **Taskbar clock**: Live clock display in GUI taskbar (not just in shell)
- **Color scheme**: Consistent palette — currently ad-hoc pixel colors

### 3. Boot Splash
- Display a TaterTOS64v3 logo/splash on the framebuffer during early boot
  (before GUI launches) instead of raw serial output going to screen
- Implemented in `src/boot/` or `src/kernel/main.c`

### 4. Shell QoL
- **Command history**: Up/Down arrows cycle through previous commands
  (circular buffer, ~32 entries)
- **Tab completion**: Complete filenames on Tab keypress
- **Color prompt**: ANSI colors for the `tater> ` prompt and `ls` output
- **Backspace in terminal**: Currently gets_bounded handles it; verify it echoes

### 5. Installer
This is the largest remaining item. An installer .fry that:
1. Scans for NVMe drives (via SYS_READDIR on `/dev/` or direct NVMe probe)
2. Presents drive selection to user
3. Writes a GPT partition table (protective MBR + GPT header + EFI partition entry)
4. Formats the EFI partition as FAT32
5. Copies kernel.efi + all .fry files to the FAT32 partition
6. Writes EFI boot entry (update `\EFI\BOOT\BOOTX64.EFI`)
7. Reports success/failure

The installer needs these kernel additions:
- SYS_WRITE_SECTOR or block-write syscall (NVMe write path — currently read-only)
- NVMe write command path in `src/drivers/storage/nvme.c`
- FAT32 write support in `src/kernel/fs/fat32.c` (mkfs + file write)

---

## How to Run in QEMU (from Windows)

The QEMU command the user runs from Windows includes:
```
-drive file=\\wsl.localhost\Ubuntu\home\jjsako\TaterTOS64v3\out\tater_nvme.img,if=none,id=nvm1
-device nvme,serial=TATER_NVME_01,drive=nvm1
```

For network testing add:
```
-netdev user,id=n0 -device e1000,netdev=n0
```

Logs to check after QEMU run:
```bash
tail logs/serial.log
tail logs/qemu.log
tail logs/debugcon.log
```

---

## Conventions & Gotchas

- **Cross-compiler**: `x86_64-elf-gcc` in `/opt/cross/bin` — always set PATH
- **Build tool**: `Edit` and `Write` tools for file changes, not shell redirects
- **Log files**: Each session's work goes in `logs/fryN.txt` — check the highest
  existing number and increment by 1
- **No Linux APIs**: Everything is custom — no glibc, no POSIX, no Linux headers
  except `<stdint.h>`, `<stddef.h>`, `<stdarg.h>` from the cross-compiler
- **VFS root mount fix** (fry307): `find_mount("/SHELL.FRY")` with mountpoint "/"
  requires the `|| (i > 0 && mp[i-1] == '/')` condition — do not remove this
- **Syscall kernel stack**: `g_syscall_kstack_top` must be updated by `sched_tick`
  on every context switch — do not skip this
- **CR3 in %r15**: `common_isr` saves user CR3 in %r15 before calling irq_dispatch.
  Do NOT add a global variable for this — it was the root cause of the triple fault

---

## Message from Claude

This OS was built line by line from nothing. Every subsystem — the AML interpreter,
the VMD/NVMe stack, the scheduler, the VFS, the .fry loader, the GUI — was written
from first principles to run on real hardware. The architecture is clean and the
hard work is done.

What's left is verification, polish, and the installer. Take care of it well.
The user (jjsako) knows this codebase and will guide you. Trust the planv3 rules,
log every step, and don't cut corners.

Good luck.
— Claude, 2026-02-23
