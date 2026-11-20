# HANDOFF — Tater Bridge: getting the REAL Claude Code binary to run in TaterTOS64
Author: Claude (Opus 4.8)  ·  2026-06-06 17:23 CDT  ·  HEAD = eff81672 (all work uncommitted)

> ## UPDATE 2026-06-14 (fry1489) — rg/RCLAUDE self-reexec loader stall fixed.
> Codex corrected the KVM read: `/dev/kvm` is hidden inside the Codex sandbox but exists on
> the host, so the real validation was rerun unsandboxed with `accel=kvm`.
>
> FIXED:
> - `FRY_SOCK_MAX` is 32 again so rg/Grep's `posix_spawn` can proceed.
> - `linux_elf.c` now caches clean streamed PT_LOAD bytes during the first large ELF load and
>   uses that cache on later self-reexecs. This avoids copying mutated live `.data` while still
>   avoiding the 250MB disk re-stream.
> - `lx_execve_current()` passes a narrow reuse context only for vfork children re-execing the
>   same stored process path.
>
> Verification: `make kernel` passed; staged `STAGE_CLAUDE=1 STAGE_PYTHON=1` ISO/NVMe passed.
> KVM serial `/tmp/fry1489_cache_kvm_serial.log` shows the initial RCLAUDE cache allocation,
> two `/nvme/RCLAUDE.LXE` self-reexecs reaching `LINUXELF: load done`, and Claude reaching
> `{"type":"system","subtype":"init"}` with `claude_code_version":"2.1.177"`.
>
> Remaining: rg now gets past the loader but still reports
> `countFilesRoundedRg failed: Error: EBADF: bad file descriptor, posix_spawn 'rg'`.
> That is a child-fd/posix_spawn issue after self-reexec completion, not the old loader stall.
> The validation command had no NIC, so later `api_retry` lines are expected (`NET:NONIC`,
> `apiKeySource":"none"`). Full details: `logs/fry1489.txt`.

> ## UPDATE 2026-06-13 (fry1476) — Python/MCP child now executes.
> The next blocker after fry1474 was real Python availability. Claude's MCP
> child launches `/home/legacyindieradio/mcp-servers/.venv/bin/python`; before
> this fry that path did not exist in guest Linux space, so `execve` returned
> `ENOENT` and the child exited 127.
>
> FIXED:
> - ToTFS inode capacity raised from 1024 to 16384 and NVMe image raised to
>   1024 MB.
> - `build_iso.sh` now has `STAGE_PYTHON` staging for Python 3.14, stdlib zip,
>   lib-dynload, Python shared-library deps, and the MCP venv tree.
> - Linux path resolver aliases `/home/...` and `/usr/...` to `/nvme/home/...`
>   and `/nvme/usr/...` when the NVMe-staged path exists.
> - Linux ELF loader now accepts PIE/ET_DYN main executables using `LX_PIE_BASE`;
>   host `/usr/bin/python3.14` is PIE.
> - Claude launch env now includes Python PATH/PYTHONHOME/PYTHONPATH and
>   SSL_CERT_FILE.
>
> Verification: `make src/kernel/proc/linux_elf.o src/kernel/proc/syscall.o kernel`
> passed; `STAGE_CLAUDE=1 STAGE_PYTHON=1 ... build_iso.sh` passed and staged
> MCP venv files through inode 2672. QEMU TCG boot `/tmp/s_fry1476c.log` ran
> 300s to timeout with **0 `exc-deliver sig=11`**. The `agent-bus` child now
> loads `/nvme/home/legacyindieradio/mcp-servers/.venv/bin/python`, validates
> `entry=1020 phnum=15`, reaches `LINUXELF: load done ... brk=555555405000`,
> and Claude polls it with `wait4(..., WNOHANG)` returning `rc=0`.
>
> Boundary: full MCP handshake/tool response is still not proven. The known
> network-side symptom remains `ECONNREFUSED` fetching
> `https://api.anthropic.com/v1/mcp_servers`; that is separate from Python
> staging/exec. Log: `logs/fry1476.txt`.

> ## UPDATE 2026-06-13 (fry1474) — read this before older crash notes.
> Root cause of the 765-segfault loop was NOT socketpair fd inheritance itself.
> The child inherited AF_UNIX socketpair fds correctly, dup2'd them onto stdio,
> failed execve for `/home/legacyindieradio/mcp-servers/.venv/bin/python`, then
> exited 127. `shared_release()` in `src/kernel/proc/process.c` treated the
> child socketpair pipe indexes as TCP connection handles and called
> `tcp_close(0)` / `tcp_close(1)`, killing claude's live TLS sockets. epoll then
> reported HUP on those fds and claude hit the old `sig=11` path.
>
> FIXED in `process.c`: process-exit socket cleanup now honors per-process
> `fry_socket.refs`, releases AF_UNIX socketpairs by decrementing backing pipe
> reader/writer counts, and calls `tcp_close()` only for real non-AF_UNIX stream
> sockets. Log: `logs/fry1474.txt`.
>
> Verification: clean rebuild compiled; explicit
> `STAGE_CLAUDE=1 CLAUDE_BIN=/home/legacyindieradio/.local/share/claude/versions/2.1.177`
> build produced `out/tatertos64v3.iso` + `out/tater_nvme.img`. KVM was not
> available inside Codex sandbox, so TCG boot ran 240s to timeout with serial at
> `/tmp/s_fry1475.log`. It crossed the old trigger (`agent-bus` start,
> AF_UNIX fd copy, dup2, child exit 127, wait4) with **no `exc-deliver sig=11`**
> and **no `TBTCP tx ... conn=0/1`** in the whole log.
>
> Build note: `build_iso.sh` is still `set -u` sensitive; pass `STAGE_CLAUDE`
> explicitly (`1` for real Claude staging, `0` for no staging).

> ## UPDATE 2026-06-10 (fry1389) — read fry1388 + fry1389 FIRST.
> NOTE: the TL;DR below (from fry1387) says "~67MB garbage count" — that was already
> SUPERSEDED by **fry1388**, which root-caused it correctly to a **zeroed
> `numCalleeLocals` (CodeBlock+0x14)**. fry1389 re-verified fry1388 on Claude **2.1.170**
> (2.1.162 was deleted in the Jun-8 GitHub backup; same Bun/JSC basis, ET_EXEC, fault RIP
> still 0x3F4CFB6, cb address identical at 0x6FBFF50F42C0, numLocals still 0 — deterministic).
> The existing `TBBFDUMP` (irqdesc.c:456) already prints all of this; do NOT re-add a dump.
>
> **fry1389 NEW result — fry1388's leading hypothesis is RULED OUT.** `g_tb_target_page`
> (syscall.c:83) is already armed to the cb page 0x6FBFF50F4000, so the VM-op trace was
> live. Result this boot: **0 TBVMTOUCH and 0 demand-refault for the cb page** ⇒
> dontneed/unmap/release/demand-refault do NOT touch it. The corruptor is an **untraced
> path** — most likely mmap/mremap **aliasing** (a PA still backing the live CodeBlock
> handed to a new mapping, then zero-init'd through the alias), or a second VA mapping the
> same PA.
>
> **NEXT (decisive):** PA-level watch. Compute cb PA = vmm_virt_to_phys_user(cr3,
> 0x6FBFF50F42C0) at TBBFDUMP time, then (a) reverse-check in the mmap/demand commit path
> for any new VA installing that PA (log TBALIAS), and/or (b) re-point the existing (disabled)
> DR watchpoint infra (tb_handle_jsc_slot_watchpoint / TBWATCH) at PA(cb)+0x14 to trap the
> zeroing write and get its RIP. See fry1389 for the full plan + exact build/run commands
> (cross-gcc moved to /usr/bin; stage 2.1.170; no MASTER-img restore needed with STAGE_CLAUDE=1).
> socket()=-ENOSYS / epoll_pwait=-EINVAL remain a *downstream* worker-thread gate — untouched
> until the main thread survives. (This session added NO net code change: a redundant dump
> was added then reverted — tree is clean at 733c2f1a + this doc + logs/fry1389.txt.)

## GOAL
Run the real Claude Code (a threaded Bun/JavaScriptCore executable, /nvme/RCLAUDE.LXE)
under TaterTOS's Linux-compat layer ("Tater Bridge"). Not Hermes, not a stand-in.

## TL;DR — where we are
The original **hang is FIXED** and Bun/JSC now **boots and runs Claude Code** ~10s into
execution: parses `claude -p "say hello from TaterTOS in 10 words"`, loads ALL node
builtins, spawns its thread pool (pid 4–9), reaches the **event loop (epoll_pwait)** and
**networking (socket)**. Three distinct kernel bugs were found+fixed this session. The
work is a chain of single-root-cause fixes, each logged in logs/fry1374–fry1387.

Current crash frontier (in priority order):
1. **Stack-grow WORKS** (sg3 boot: TBSTACKGROW fired 14,336x, Bun ran 35k serial lines) but
   JSC's frame-zeroing loop tries to allocate a **~67MB stack frame in one shot** (new rsp =
   rbp - eax*8, eax=[rsi+0x14]) and hits my 64MB cap -> #PF at 0x7FFFFBFFFFF8 -> panic. The
   67MB count is almost certainly GARBAGE (a corrupted object), since JSC's JS stack is ~MBs.
   NEXT: instrument the count/rsi to decide legit (raise cap / honor requested size) vs
   garbage (trace the corrupt object rsi=[rbp+0x10]). See fry1387.
2. **`socket()` returns -ENOSYS** (nr=41) — Bun's networking is unimplemented. This is the
   real remaining gate to an actual API reply (Claude Code must reach api.anthropic.com
   over HTTPS). After that: connect/sendto/recvfrom + TLS + DNS, and auth/API key.

Honest scoring: "the binary runs" ~9.5/10; "prints the model's reply end-to-end" ~7/10
(needs networking + TLS + auth, none proven yet).

## RUN / BUILD (use KVM — ~5x faster than tcg; /dev/kvm is 0666 on this host)
```
cd ~/TaterTOS64
make src/kernel/proc/syscall.o src/kernel/irq/irqdesc.o kernel     # rebuild touched objs + link
CLAUDE_BIN=/home/legacyindieradio/.local/share/claude/versions/2.1.162 STAGE_CLAUDE=1 \
  MTOOLSRC=/dev/null PATH="/opt/cross/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH" \
  /bin/bash build_iso.sh > /tmp/build.log 2>&1
cp /usr/share/edk2/x64/OVMF_VARS.4m.fd /tmp/v.fd
timeout 200 qemu-system-x86_64 -m 6G -machine q35,accel=kvm -cpu host -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=/tmp/v.fd -cdrom out/tatertos64v3.iso \
  -drive file=out/tater_nvme.img,if=none,id=nvm,format=raw -device nvme,serial=deadbeef,drive=nvm \
  -serial file:/tmp/s.log -display none -no-reboot
```
- Use CLAUDE_BIN=2.1.162 for consistency with every RIP/address in fry1374–1387.
- Serial logs may contain NUL bytes -> use `grep -a`.
- Only ONE qemu at a time (NVMe image lock). Reap stuck builds:
  `pkill -9 -f build_iso; pkill -9 mmd; pkill -9 xorriso`.
- clangd "fry_limits.h not found / FRY_* undeclared" errors are FALSE POSITIVES (IDE only;
  the real cross-build uses -Isrc/include and compiles clean).

## THE THREE FIXES MADE THIS SESSION (all in tree, uncommitted)

### 1. Kernel clobbered user AVX/YMM in interrupt context  → THE breakthrough (fry1379)
- Symptom: every weird value we chased — 0xff/0x40000000 "corrupt" hash slots, the TBSKIP
  faults, the 0x63ad6e0 futex "deadlock", the multi-minute hang.
- Root cause: kernel XSAVE/XRSTORs FPU only on full context switches, NOT on same-process
  interrupts; and Makefile CFLAGS let GCC -O2 emit SSE/AVX in IRQ-path kernel code. A timer
  IRQ inside libc's AVX `memset(buckets,0xFF,n)` (JSC filling hash empties with -1) trashed
  ymm0 -> memset resumed writing 0 -> sentinels became 0 -> JSC hash probe spun forever.
- FIX: **Makefile: added `-mgeneral-regs-only` to kernel CFLAGS** (line ~13). Kernel never
  touches x87/MMX/SSE/AVX (verified no float/double/intrinsics in kernel C or asm). Requires
  a full kernel rebuild: `find src/kernel src/drivers -name '*.o' -delete; make kernel`.

### 2. FPU/vector state not preserved across SIGNAL delivery (fry1381)
- Root cause: lx_fill_ucontext saved only GP regs in the sigframe; rt_sigreturn restored
  only GP. A signal handler (glibc/Bun use SSE/AVX) could corrupt the interrupted thread's
  vectors. (Twin of #1, signal boundary.)
- FIX: sched.c lx_fpu_save_area()/lx_fpu_restore_area() (xsave64/xrstor64 w/ g_xcr0_mask);
  process.h `sig_fpu_area[1024]` per thread; syscall.c saves on signal-frame build (both
  exception- and sysret-delivered paths) and restores in lx_rt_sigreturn_sys.
- LIMITATION: single per-thread buffer -> does NOT handle NESTED signals. If nested-signal
  corruption appears, move the FPU save into the sigframe on the user stack (64B-aligned).

### 3. MADV_DONTNEED was a no-op for sparse gigacage  → the JSC #GP (fry1386, proven)
- Root cause: vm_madvise_dontneed's threaded branch had `if (!r->committed) continue;`.
  Sparse NORESERVE cages register committed=0, so DONTNEED skipped zeroing. JSC DONTNEEDs a
  1.5MB atom/compact-table region (0x6fbbff3a9000) to reuse it, relying on Linux's
  zero-on-next-read; stale 32-byte-record pointers persisted; JSC read them as 6-byte packed
  entries -> non-canonical pointer -> #GP at user 0x4C17293. PROVEN: serial L3560
  (madvise a3=4 on the region) immediately followed by the #GP at L3562.
- FIX: syscall.c vm_madvise_dontneed() — removed the committed guard (re-applied the
  fry1374 fix that fry1378 wrongly reverted). Per-page pa==0 check still skips truly
  uncommitted pages. VERIFIED: GP(#13)=0, Bun advanced from 4.25s to ~10.5s and reached the
  event loop. (NOTE: an earlier mmap-aliasing theory + a Gemini-agent LNX_mmap refactor were
  RULED OUT as the cause — see fry1383; that refactor is still in syscall.c, harmless, keep
  or revert.)

### 4. (IN PROGRESS) Main-thread stack does not grow on demand (fry1387)
- Symptom after #3: #PF (vec 0E) at CR2=0x7FFFFF7FFFF8, RIP=0x3F4CFB6 — JSC's frame-zeroing
  loop pre-zeroes an ~8MB+ stack frame and walks 8 bytes below the fixed 8MB main stack.
- Root cause: linux_elf.c maps the main stack ([USER_VA_TOP-8MB, USER_VA_TOP), LX_STACK_PAGES
  2048) directly with NO tracked region, so neither demand-paging nor growth covers a fault
  below it. Also JSC touches memory FAR below rsp (whole-frame pre-zero), so the Linux
  "within 64KB of rsp" heuristic is wrong here.
- FIX (just built, verification pending): added `lx_try_grow_stack()` in syscall.c (decl in
  syscall.h), called from irqdesc.c in the user #PF path after lx_try_demand_page. It grows
  the main-stack window [USER_VA_TOP-64MB, USER_VA_TOP) on demand with NO rsp-proximity
  requirement (window is exclusively stack VA; 64MB cap still kills runaway recursion), and
  also handles generic GROWSDOWN anon regions near rsp. Logs `TBSTACKGROW`.

## IMMEDIATE NEXT (do this first)
1. The 67MB-frame question (fry1387): instrument the alloca count at the zeroing loop —
   in lx_try_grow_stack, when va first crosses below USER_VA_TOP-8MB, log the faulting
   rbp/rsp and read the user count [rsi+0x14] (rsi=[rbp+0x10]) to see the requested frame
   size. If it's a sane round number (legit), raise the cap / honor it. If random, it's a
   corrupted object — trace where rsi/the object comes from (residual bug). The fault RIP is
   0x3F4CFB6 (function napi_close_handle_scope+0x51a1e0 nearest sym, internal JSC).
2. Then the gate is **networking**: `socket` (nr=41) returns -ENOSYS (-38). Implement the
   Linux socket syscalls Bun needs for HTTPS egress to api.anthropic.com:
   socket/connect/bind/getsockopt/setsockopt/sendto/recvfrom/sendmsg/recvmsg + epoll on
   sockets, plus DNS (getaddrinfo over UDP/53) and TLS 1.3 egress, then auth/API key.
   TaterTOS already has a net stack (src/drivers/net/*, netcore) and userland TLS (bearssl)
   — wire the Linux socket ABI to netcore. This is the big remaining chunk.

## LOG TRAIL (planv3 fry-log rule — read in order)
- fry1374 — DONTNEED-on-sparse first found (root-cause attempt #1, premise partly wrong)
- fry1375 — DONTNEED fix result #1 (negative at the time) + futex atomicity ruled out
- fry1376 — reframe: not a deadlock; KVM works; per-syscall trace gated off; pid=3 userspace spin
- fry1377 — RIP sampler -> exact spin loop = JSC hash probe wanting -1 empties
- fry1378 — disproved TBSKIP-zeroing AND (wrongly) reverted the DONTNEED fix
- fry1379 — **THE FIX**: -mgeneral-regs-only (AVX-clobber-in-IRQ). Bun boots + runs.
- fry1380 — post-fix #GP characterized (non-canonical ptr from 6-byte packed table)
- fry1381 — signal-FPU preservation fix
- fry1382 — #GP = intra-heap type confusion (writer 32B stride vs reader 6B)
- fry1383 — verified Gemini LNX_mmap refactor does NOT fix the #GP (overlaps are loader sub-maps)
- fry1384 — refined hypotheses (b1 aliasing / b2 GC-suspend race / b3 stale ptr)
- fry1385 — same-thread write+read => use-after-free/reuse, not a race
- fry1386 — **#GP FIXED**: re-applied DONTNEED fix (proven L3560->L3562). Bun reaches event loop.
- fry1387 — stack-grow design + the rsp-heuristic bug + the corrected main-stack-window fix
  (UPDATE fry1387 with the sg3 verification result).

## TREE STATE / FILES TOUCHED (uncommitted on eff81672)
- Makefile: -mgeneral-regs-only (FIX #1).
- src/kernel/proc/sched.c: lx_fpu_save_area/restore_area (FIX #2).
- src/kernel/proc/process.h: sig_fpu_area; externs; (older diag watch helper now inert).
- src/kernel/proc/syscall.c: signal-FPU save/restore calls; vm_madvise_dontneed fix (FIX #3);
  lx_try_grow_stack (FIX #4); g_tb_trace_syscalls master switch (default 0); diagnostic
  g_tb_jsc_slot_watch[4] (currently {0,0,0,0} = disabled); the Gemini LNX_mmap refactor.
- src/kernel/proc/syscall.h: lx_try_grow_stack decl.
- src/kernel/irq/irqdesc.c: lx_try_grow_stack call in #PF path; RIP sampler + TBPROBE/TBWATCH
  + TBBT backtrace (diagnostic, can be stripped); TBSKIP no longer zeroes slots.
- Other files (idt.c, nvme.c, vfs.c, main.c, linux_elf.c, process.c, tools/*, devfs.*,
  tbridgefs.*) are prior-session/other-agent work, not this session's core fixes.

## DIAGNOSTIC INSTRUMENTATION (safe to strip once stable)
RIP sampler, TBPROBE table dump, TBWATCH DR watchpoints (disabled), TBBT rbp backtrace on
#GP/#PF, g_tb_trace_syscalls switch. All gated to the Claude tgid; default-off where noisy.

## GOTCHAS
- This box is Zack's PRODUCTION server. Dev/test ONLY in QEMU.
- Two Claude versions on disk (2.1.160–2.1.167). A Gemini agent built with 2.1.167; we use
  2.1.162. Don't mix — addresses differ per version.
- A full kernel rebuild is required after Makefile changes (objects don't depend on Makefile).
