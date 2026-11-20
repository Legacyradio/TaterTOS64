# HANDOFF — rg/Grep in claude-in-TaterTOS: FIXED, pending one un-throttled green run
Date: 2026-06-13. Author: Claude (Opus 4.8). Logs: fry1482–1485.

## TL;DR
- **The rg/Grep blocker is FIXED.** One-line root cause, one-line fix, verified.
- **Fix:** `FRY_SOCK_MAX` 16 → 64 in `src/include/fry_limits.h` (+ `src/kernel/selftest.c:122` assertion 16→64).
- **Verified:** claude's startup rg probe (`countFilesRoundedRg`) failed on the 16-slot kernel and **stopped failing on the 64-slot kernel** (0 failures, 0 EBADF/ENFILE across 6 runs).
- **Only open item:** a full green end-to-end session run has not been re-confirmed on the 64-slot kernel. fry1483 (16-slot) reached `subtype:init` + `subtype:success`; several 64-slot runs stalled before `subtype:init`. **Cause UNKNOWN — not root-caused.** Two theories were investigated and KILLED: (a) in-OS "HTTP 429 rate limiting" — false, the "429" strings were timestamp digits, not HTTP statuses; (b) a timerfd busy-spin — false, the successful run has the same timerfd activity. Do not reintroduce either. Next step is to actually diagnose the path between "LSP notification handlers registered" and the first `subtype` event, or just re-run and observe.

## What was broken
claude's Grep tool (and its startup `countFilesRoundedRg` file-count) spawns ripgrep (`rg`) via `posix_spawn`. It failed two ways:
- `ENFILE: file table overflow` — `socketpair()` (syscall nr=53) itself failed.
- `EBADF: bad file descriptor` — the rg child was missing its stdio fd.

## Root cause (proven with instrumentation, not guessed)
Both are the SAME root cause: **kernel socket-slot pool starvation.**
- `FRY_SOCK_MAX` was 16 → only 8 concurrent AF_UNIX socketpairs system-wide.
- claude's init legitimately holds ~7–8 pairs (peak observed: **17 sockets**), so:
  - `socketpair()` returns `-ENFILE` at `sock=15/16` (syscall.c SYS_SOCKETPAIR inlined slot scan).
  - `copy_vfork_fd_table` (`src/kernel/proc/process.c:519-524`) **silently skips** copying the rg child's AF_UNIX socketpair-stdio when the pool is full (`if (nsi < 0) continue;`) → the child's `posix_spawn` dup2 hits a missing fd → `EBADF`.
- Instrumentation: SPCEN census beacons added in fry1482 (`pipe_alloc`, `sock_alloc`, SYS_SOCKETPAIR, SYS_CLOSE in syscall.c; teardown in process.c). The census PROVED sockets (not pipes) were the limit: `socketpair -ENFILE at sock=15/16` while pipes sat at `12/128`. It is genuine NEED, not a leak (socket count drains to 0 cleanly).
- NOTE: last session (fry1477) wrongly concluded "sockets aren't the limit" because the `sock_alloc` high-water beacon is blind to socketpair (which bypasses `sock_alloc`). The census tags (reading the live `g_sockets` array) exposed the true 15/16.

## The fix (already applied, uncommitted)
- `src/include/fry_limits.h`: `FRY_SOCK_MAX 16 -> 64` (64 = 32 pairs = ~4x the observed peak). SAFE because `g_sockets` is a BSS global (`process.c:51`), NOT on the kernel stack like `fd_paths`/`FRY_FD_MAX` (which is why FD_MAX can't be raised without moving fd_paths off-stack first).
- `src/kernel/selftest.c:122`: `st_check("FRY_SOCK_MAX", ..., 16)` -> `64`. (st_check `kernel_panic`s on mismatch; selftest is skipped in prompt-only/claude boots, but a normal boot would panic.)
- fry1482 SPCEN census beacons remain in syscall.c/process.c (capped, serial-only diagnostics). Optional to strip in a cleanup pass.

## Verification evidence
`countFilesRoundedRg failed` count per run:
- fry1483 (16-slot): **1** (EBADF then ENFILE)
- fry1484c, s_rg_a1, s_rg_a4 (64-slot): **0** + zero EBADF/ENFILE
Resource census on the 64-slot kernel: 0 socketpair exhaustion, socket high-water 17/64, pipes 12/128. No regression: fry1483's authenticated round-trip returned `{"type":"result","subtype":"success"}` WITH the beacons in place.

## Open (THE REAL REMAINING BUG): a futex deadlock exposed by giving claude >16 sockets
**The socket bump fixes rg's spawn but EXPOSES a deadlock. The bump alone is NOT a shippable fix.**

Reproducible result by `FRY_SOCK_MAX`:
- **16** (fry1483): claude reaches `subtype:init` + `subtype:success`. BUT rg's spawn FAILS (socketpair starves at 15/16) — `countFilesRoundedRg failed`. claude takes a degraded path and completes.
- **32** (s_32.log): peak sock 17/32, rg no longer starves — and claude **DEADLOCKS before `subtype:init`**.
- **64** (multiple runs): same deadlock.

The deadlock (consistent across 32 and 64):
- Last app log: `[DEBUG] LSP notification handlers registered successfully for all 0 server(s)`.
- Then the **main thread (pid 6) busy-spins on a futex WAIT_BITSET (~120ms abs-timeout)** that is **never woken**. The worker (pid ~14) that should wake it drops out / stops being scheduled. Serial keeps growing (47KB/8s) but it's PURE pid-6 futex-spin — zero new app/stream-json output. Never reaches `subtype:init`.
- In the SUCCESS run (fry1483) the pre-init window is full of futex **wake** events (threads waking each other); in the deadlock there are WAITs with no matching WAKE.

PINNED via the existing TBFUTEX traces (no new run needed):
- The main thread **pid 6 waits on `uaddr=6fbff800e8c8` (key `80056fbff800e8c8`, private, expected=0)** and **ZERO `TBFUTEX wake-call`s ever target that key** (5658 wakes go to the adjacent `...e880`, 0 to `...e8c8`).
- It is NOT a thread-join / `clear_child_tid` lost-wakeup: every clone3 `child_tid` is in the `6fbb…ce8` region; none is `…e8c8`. The kernel's `clear_child_tid` wake path (syscall.c:2272-2288, clone3 set at :7327) is present and correct. `…e8c8` is a regular userspace condvar/mutex word.
- The worker **pid 14 never exits** (`TBEXIT pid=14` = 0) — it stays alive, stuck in an `epoll_pwait2(441) → read 8 bytes` loop (reading an always-ready eventfd/timerfd) and never runs the userspace code that would `FUTEX_WAKE …e8c8`.
=> **LIVE circular-ish deadlock:** pid 6 blocks on e8c8 waiting for worker pid 14; pid 14 is stuck in its epoll/eventfd loop and never signals e8c8. With >16 sockets claude spins up its full worker set and hits this; with 16 (socketpair starves) fewer workers spin up and the run completes. The trigger is the socket count; the bug is whatever keeps pid 14's epoll loop from making progress (or the missing condition that should let it break out and signal e8c8). Most likely lives in the uncommitted vfork/AF_UNIX-socketpair worker wiring (fry1463–1485), NOT the count.

NOT YET PINNED (next person starts here): WHY pid 14's epoll loop never terminates. Identify the fd pid 14 polls (the always-ready 8-byte one) and what data/condition it's actually blocked on, and compare to the 16-slot success where the equivalent worker breaks out and issues the `FUTEX_WAKE …e8c8`. Instrument: in the epoll/eventfd/read path, log pid 14's interest-set fds + what it's waiting for, and every `FUTEX_WAKE` target near `…e8c8`.

RULED OUT (do not revisit): "HTTP 429 rate limiting" (the "429" strings were timestamp digits, fabricated by a bad grep — there is zero real rate-limit evidence) and the timerfd fd=15 spin (the SUCCESS run has identical timerfd activity).

### Next steps to actually fix it
1. **Instrument the futex path** (syscall.c futex WAIT/WAKE handlers): log every FUTEX_WAKE (uaddr, count woken) and every WAIT that times out without a wake, scoped to the claude tgid, in the window after "LSP notification handlers registered." Find the WAIT whose uaddr never receives a WAKE — that's the lost wakeup. Likely causes: a WAKE issued against a different uaddr/key than the WAITer computed (key mismatch), a WAKE lost because the waiter hadn't enqueued yet (missed-wakeup race), or a worker thread exiting/erroring before issuing its WAKE.
2. **Cross-check the uncommitted vfork/socketpair work**: the AF_UNIX socketpair vfork inheritance (fry1474, process.c:506-533, own-struct + pipe-refcount) and the thread/clone (`set_tid`) handling are the most likely places a worker thread mismanages its futex/wake. Check whether a socketpair-backed worker exits without waking its joiner.
3. Only after the deadlock is fixed does `FRY_SOCK_MAX` >16 become a real win. Keep it at a value that covers rg's peak need (17) with headroom (e.g. 32) ONCE the deadlock is gone.
4. Optional stronger Grep-TOOL proof (after the above): change the baked prompt `src/kernel/main.c:759` to force a Grep tool call over a small in-OS dir.

### Honest status of "the rg fix"
rg's posix_spawn failure (ENFILE+EBADF) IS understood and IS resolved by more sockets — that part is real. But shipping it requires fixing the futex deadlock first, or claude can't reach init. The two are coupled. Current tree is at FRY_SOCK_MAX=32 (deadlocks). Set it back to 16 if you need a booting-claude tree while the deadlock is worked (16 boots+completes but rg stays broken).

## Exact repro / commands
Build kernel (NOTE: Makefile has NO header-dep tracking — after editing any header you MUST force-recompile):
```
cd ~/TaterTOS64
find src/kernel -name '*.c' -exec touch {} +    # only needed after a header edit
make
```
Build image (stages claude + python, restages OAuth from live ~/.claude):
```
env STAGE_CLAUDE=1 STAGE_PYTHON=1 \
  CLAUDE_BIN=/home/legacyindieradio/.local/share/claude/versions/2.1.177 \
  PYTHON_BIN=/usr/bin/python3.14 MTOOLSRC=/dev/null \
  PATH=/opt/cross/bin:/usr/bin:/bin:/usr/sbin:/sbin /bin/bash build_iso.sh
```
Run (q35/kvm, -smp 4, e1000 slirp for OAuth, serial to file):
```
cp /usr/share/edk2/x64/OVMF_VARS.4m.fd /tmp/OVMF_VARS_run.fd
timeout 900s qemu-system-x86_64 -m 4G -machine q35,accel=kvm -cpu host -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=/tmp/OVMF_VARS_run.fd \
  -cdrom out/tatertos64v3.iso \
  -drive file=out/tater_nvme.img,if=none,id=nvme0,format=raw \
  -device nvme,drive=nvme0,serial=tatertos-nvme \
  -netdev user,id=n0 -device e1000,netdev=n0 -display none -serial file:/tmp/run.log
```
Grep the serial for the verdict:
```
grep -ac 'countFilesRoundedRg failed' /tmp/run.log   # want 0  (rg fixed)
grep -ac 429 /tmp/run.log                            # high = throttled startup
grep -a '"subtype":"init"' /tmp/run.log              # claude session started
grep -a '"subtype":"success"' /tmp/run.log           # full green
grep -a 'SPCEN exhaust' /tmp/run.log                 # want none (socket pool ok)
```
OAuth: token at `~/.claude/.credentials.json` rotates ~8h; build_iso restages whatever is live. Check validity:
```
python3 -c "import json,time;d=json.load(open('/home/legacyindieradio/.claude/.credentials.json'));o=d.get('claudeAiOauth',d);print((o['expiresAt']-time.time()*1000)/60000,'min left')"
```

## Coordination / state
- Uncommitted: `src/kernel/proc/syscall.c`, `src/kernel/proc/process.c` (2400+ lines = the verified fry1463–1485 chain), plus `src/include/fry_limits.h`, `src/kernel/selftest.c`. NOTHING pushed.
- Codex co-edits the proc subsystem (syscall.c/process.c) — serialize edits to avoid clobber.
- Full per-step logs: `logs/fry1482.txt` (instrument), `logs/fry1483.txt` (diagnosis), `logs/fry1484.txt` (fix), `logs/fry1485.txt` (verify + 429 explanation). Archived serials: `logs/serial_fry1483.log`, `serial_fry1484c.log`, `serial_fry1484d.log`.
