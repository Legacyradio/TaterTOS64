# HANDOFF — Native Hermes / CPython ELF-TLS fix (2026-06-21)

**Read this first, then the fry logs fry1701→fry1705 in order.**

## Goal
Get native `hermes` (CPython 3.14.5 as `python3.fry`) to run its self-test in
TaterTOS. Step 1 (os.environ, fry1697) is DONE+VERIFIED. The self-test was then
blocked by an allocator crash — that's what this handoff covers.

## Root cause (CONFIRMED, fry1701, read-only via readelf/objdump — no guess)
`_PyObject_Malloc` reads the current PyThreadState from `__thread _Py_tss_tstate`
at `%fs:-8` → NULL → deref +0x10 → fault cr2=0x10. The `.fry` loader never laid
out the ELF **PT_TLS** image, so CPython's `__thread` vars (and mimalloc's
`__thread _mi_heap_default`) had no backing below `%fs`. Same no-crt0 family as
the argv/environ/exit bugs. This required implementing the x86-64 **variant-II
ELF TLS ABI** for native `.fry` binaries.

## What was implemented (kernel-side, this session)
Lay out the PT_TLS image at load time and point `%fs` at a TCB just above it.

- **src/kernel/proc/elf.h** — `elf_load_fry()` gains 5th out-param
  `uint64_t *tls_base_out`; new error `ELF_LOAD_ERR_TLS_ALLOC = -114`.
- **src/kernel/proc/elf.c** — `#define PT_TLS 7`;
  `FRY_TLS_REGION_BASE 0x00007FFF80000000` (in the unused gap between mmap-arena
  top `0x7FFF00000000` and the user stack base `0x7FFFFF000000`),
  `FRY_TLS_TCB_RESERVE 0x1000`, `FRY_TLS_MEMSZ_MAX 0x400000`.
  After PT_LOAD segs + stack are mapped (and while the file payload is still
  live): scan phdrs for PT_TLS; if `memsz>0`, validate bounds/align, compute
  `tls_alloc = align_up(memsz, align)`, `tp = REGION_BASE + tls_alloc`, map
  `[REGION_BASE, tp+TCB_RESERVE)` WRITE|USER|NX zero-filled, copy `.tdata`
  (filesz) from the payload into `[REGION_BASE, REGION_BASE+filesz)`, leave
  `.tbss`+TCB zero, set `*tls_base_out = tp`.
  Variant-II: `__thread` var at module offset x → `tp - tls_alloc + x =
  REGION_BASE + x`; the linker already emitted `fs:-8` for `_Py_tss_tstate`.
  No self-ptr at `fs:0` (main exe = local-exec model; `fs:0` overlaps libc
  `fry_tls_block.slots[0]`; libc `fry_tls_block_ensure()` adopts the non-NULL
  `%fs` via its early `if (block) return block;` — no libc change needed).
- **src/kernel/proc/process.c** — `process_launch` + `process_launch_args`:
  declare `tls_base=0`, pass `&tls_base`, set `p->user_fs_base = tls_base` after
  `process_create_user`. `process_start()` already programs MSR_FS_BASE from
  `user_fs_base` at process.c:162 before entry. Backward-compat: no PT_TLS →
  tls_base=0 → libc lazy-allocates as before.

## Build status — DONE + STAGED, **NOT VERIFIED**
- `make kernel` EXIT=0; `kernel.elf` relinked 08:18 (+4144B); TLS refs present in
  `elf.o` (objdump). `build_iso.sh` EXIT=0; ISO restaged `out/tatertos64v3.iso`
  08:19:36 (after the kernel). **python3.fry NOT relinked — kernel-only change**;
  the staged python3.fry already has the fry1697 os.environ fix.
- **Nobody has booted it. Do NOT claim the fix works until proven on serial.**

## NEXT ACTION — verify (the only step left)
1. Boot: `./launch_debug.sh` (q35/OVMF/nvme/cdrom; serial stdio, GTK display).
   **Zack types `hermes` at the shell** (shell.c:845 `cmd_hermes`). This is a
   normal keypress — there is NO "driving the desktop". Do not invent input
   machinery.
2. Read serial to the END. EXPECTED: no `_PyObject_Malloc` crash at `fs:-8`
   (TLS now backed); self-test proceeds past the allocator. If it still faults,
   capture `/tmp/fry1706_qemu_serial.log` and disasm the new fault RIP.
3. Log the ACTUAL end state as **fry1706** (next number).

## Still owed (separate, do not conflate with the above)
- **Thread TLS** (follow-up): threads from `fry_thread_create`→clone get a bare
  libc `fry_tls_block` with no ELF image below it. Main-thread TLS (this fix) is
  enough for the single-threaded self-test; CPython worker threads need the same
  layout in the clone path later.
- Cleanup owed from earlier steps: fry1688 marker (python.c), fry1697 diag
  (bootstrap.py), fake `_ssl` registration (config.c). And the dormant
  fry1391–1393 TBWATCH/TBALIAS diff in irqdesc.c (NOT mine; old CodeBlock hunt;
  harmless/dormant for hermes).

## Gotchas
- The harness **evals via /bin/bash** even though the env header says
  `Shell: /usr/bin/fish`. Use bash syntax; trust behavior over the label.
- **Verify build before launch**: confirm EXIT=0 + fresh artifact mtime; never
  chain `make && qemu` (stale-image false-positive).
- **Log every step its own fryNNNN** the moment it completes (Rule 5/6).
- The fry1700 "page-validated FBT" that double-panicked is reverted; source is
  clean for the build (verified in fry1702).

## Pointers
fry1701 root cause · fry1702 design + pre-build verification · fry1703 impl ·
fry1704 build · fry1705 honest checkpoint · HERMES_NATIVE_PLAN.txt overall plan.
