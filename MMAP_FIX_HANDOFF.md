# MMAP FIX HANDOFF - TaterTOS64

## Status
- **Current State:** Kernel code for `mmap` aliasing fix is implemented in `src/kernel/proc/syscall.c`.
- **Build Status:** Build process (`build_iso.sh`) is currently running in the background. It successfully compiled the kernel and user apps but appears to be stuck or extremely slow in the image packaging phase.
- **Verification:** The fix needs to be validated by running the headless QEMU test and inspecting the serial logs for mmap overlaps.

## Technical Summary
- **The Problem:** The `mmap` allocator was causing memory region collisions between eager (non-sparse) mappings and hint-based mappings, leading to type confusion crashes in JSC (confirmed via TBTRACE logs).
- **The Solution:** 
  1.  Modified `LNX_mmap` to force all mappings into the `PROC_VMREGS` tracking table.
  2.  Refactored the top-down allocation cursor to use `is_range_free_for_mmap_hint()` to skip occupied regions.
- **Changes:**
  - `src/kernel/proc/syscall.c`: Refactored `LNX_mmap`.

## Pending Steps
1.  **Wait/Finish Build:** Ensure `build_iso.sh` completes the ISO and NVMe image creation.
2.  **Run QEMU:** Execute the headless boot command and redirect output to `/tmp/tb_verify_1383.log`.
3.  **Analyze Logs:**
    ```bash
    grep -a "TBTRACE mmap detail" /tmp/tb_verify_1383.log
    ```
    Verify no overlapping regions exist for the same PID.
4.  **Confirm #GP Fix:** Check if the `#GP` at 0x4C17293 is resolved in the serial logs.

## RESULT — executed 2026-06-06 16:44 CDT (Claude, build 2.1.162, KVM)  ❌ FIX INEFFECTIVE
- Built clean (rc=0), booted (KVM, /tmp/tb_mmap_s.log, 1313 lines).
- **#GP NOT resolved:** identical crash, vec=0x0D at RIP=0x4C17293,
  rdi=0x820000000000 (non-canonical), entry r12=0x6FBBFF3C2C6C,
  table base r13=0x6FBBFF3A8020, index r14=0x4762, mask r15=0x3FFFF
  (256K-entry, 6-byte-stride table; r13 + 0x4762*6 == entry, consistent).
- **Overlap analysis (step 3):** 55 mmap regions, 9 overlaps — but ALL are
  normal dynamic-loader sub-mappings (file mapped, then PT_LOAD segments mapped
  over parts), e.g. [0x6fbfec000000,0x6fbff0002000) ⊃ [0x6fbff0001000,…).
  NONE overlaps the table region 0x6FBBFF3x. So the mmap aliasing the fix
  targets DOES NOT EXIST at the crash site.
- **Conclusion:** the #GP is NOT mmap-level aliasing (hypothesis a). It is
  INTRA-HEAP type confusion (hypothesis b): the writer at user rip 0x5835D16
  stores a valid 8-byte pointer (0x6FBFF40EC200) with a 32-byte stride into the
  same bytes the reader reads as a 6-byte entry; the pointer's low 2 bytes
  (0xC200/0x8220) become the reader's high-16 → non-canonical → #GP. Two JSC
  objects share one heap address (use-after-free / double-alloc in JSC's
  userspace allocator), invisible to mmap-overlap checks.
- **Recommendation:** the LNX_mmap refactor is aimed at the wrong layer. Decide
  whether to KEEP it (it may still be a latent correctness improvement) or
  REVERT to reduce risk/diff — but it is not this bug. Real next step:
  hypothesis (b) — trace the reader's table base r13=0x6FBBFF3A8020 origin
  (disasm up the 0x4C0D… chain) to find the stale/aliased pointer; or hunt the
  JSC allocator handing the same address to two objects.
- Full detail: logs/fry1382 (diagnosis) + fry1383 (this verification).
- NOTE: this build's syscall.c contains BOTH the Gemini mmap refactor AND
  Claude's signal-FPU + diagnostic changes (uncommitted). The CONFIRMED win this
  session is Makefile -mgeneral-regs-only (fry1379): it fixed the original hang
  and Bun/JSC now boots + runs Claude Code; this #GP is the next, separate bug.
