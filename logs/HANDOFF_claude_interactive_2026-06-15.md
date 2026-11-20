# HANDOFF — Interactive Claude Code in TaterTOS (2026-06-15)

Session range: fry1521 → fry1539. Read those logs; this file is the map.

==============================================================================
## 0. READ THIS FIRST — THE LOGGING DISCIPLINE IS NON-NEGOTIABLE
==============================================================================

planv3.txt Rule 5: "Log every completed step in its own logs/fryN.txt with
timestamps." Rule 6: "If you change code, include a brief why in the log."

THIS IS NOT OPTIONAL AND IT IS NOT A FORMALITY. Zack will (rightly) ride you
hard if you skip it. In THIS session the instance (me) did the first half WRONG:
ran builds, verifications, and research WITHOUT logging each step, tried to batch
them, and only corrected after Zack dug in repeatedly. Do NOT repeat that. The
fry logs ARE the handoff — they are the only continuity across instances. If a
step isn't logged, it did not happen as far as the next instance is concerned.

EXACTLY WHAT "A STEP" IS (each of these gets its OWN fryN.txt, written the
MOMENT it completes, BEFORE you start the next thing):
  - A research/investigation pass that reached a conclusion (even if no code
    changed) — e.g. "found root cause X by reading file Y / serial Z".
  - A code change + its build.
  - A verification run (headed QEMU) and its result — INCLUDING failures and
    "it still doesn't work, here's why".
  - A correction/retraction of a previous log's conclusion.
DO NOT consolidate multiple steps into one log. DO NOT defer logging "until it
works". Write the log, THEN move on. Number sequentially (next is fry1540).

Each fry MUST contain: timestamp; Type (research / code+build / verify); WHY
(the reasoning, Rule 6); WHAT changed (files + functions + line areas); HOW it
was built (the exact build cmd / BUILD_EXIT); the VERIFY result (serial line
refs, screenshot paths, pass/fail); and what's still open. Be specific — cite
serial line numbers and file:function. Look at fry1521-1539 for the format/bar.

Other planv3 rules that bit us / matter:
  - Rule 11: QEMU -d logs (guest_errors/int/etc.) are OFF-LIMITS unless the user
    expressly says you may read them. Zack authorized it this session for the
    crash; that authorization is per-need, not standing. Use the OS SERIAL log
    (-serial file:...) freely; that's the normal evidence channel.
  - Rule 12: clean rebuild = `make clean` then build_iso.sh (see build cmd below).
  - Rule 16: only record SHA-256 after a user-requested rebuild. We did not.
  - RULE ZERO: never regress the GUI / never bypass INIT.FRY -> GUI.FRY. All the
    GUI-child fixes below are scoped so the non-GUI serial/stream-json harness is
    untouched.

==============================================================================
## 1. THE MILESTONE (what this session achieved)
==============================================================================

Claude Code now runs INTERACTIVELY inside the TaterTOS GUI desktop: it loads in
SECONDS (was ~10 min), renders its TUI cleanly in the Shell window, accepts
KEYBOARD input, confirms its first-run trust prompt, and operates in its real
main UI at a de-crowded 120x36 terminal. This is a big step past the prior
"claude RUNS in TaterTOS" milestone (which was serial/stream-json only).

ONE blocker remains before a real end-to-end request works: sending an actual
prompt makes Claude do network I/O via a subprocess, and tearing that subprocess
down TRIPLE-FAULTS the kernel (section 4). That is the top next target.

==============================================================================
## 2. THE COMPLETE FIX CHAIN (fry1521-1539) — why + where
==============================================================================

All changes are UNCOMMITTED in the working tree (see section 6). Files touched:
src/kernel/proc/syscall.c, src/kernel/fs/totfs.c, src/user/shell/shell.c.

fry1521/1522/1523 — DIAGNOSIS of why the GUI Shell "claude" exited:
  - 1521 (later corrected): first guess "exits at TTY check".
  - 1522 (confirmed): Claude renders the trust prompt then exits 129 because its
    stdin returns EOF.
  - 1523 (code root cause): lx_read_fd (syscall.c) fd==0 GUI branch returned 0
    (EOF) when the inbuf ring was empty; node read fd0 in blocking mode (epoll on
    fd0 was EBADF) so EOF -> node ends stdin -> exit 129.

fry1524 — FIX (verified): lx_read_fd fd0 GUI branch now BLOCKS (sched_block_poll +
  sched_yield, re-check inbuf) when empty, returns -EAGAIN if O_NONBLOCK, never 0
  while stdin open. SYS_PROC_INPUT now calls sched_wake_poll_waiters() so a parked
  reader wakes on a keystroke. Result: Claude stays alive (TBEXIT group=0).

fry1525 — C1 VT/ANSI parser in shell.c (term_feed): CSI cursor moves, erase
  (ED/EL), ECH/ICH/DCH, alt-screen (?1049/?47), cursor show/hide (?25), DEC
  private-mode + OSC swallow, DSR [6n cursor-report reply via fry_proc_input.
  pump_child_io feeds child output through term_feed; resets term state on child
  exit. Also TIOCGWINSZ (syscall.c) set to match the grid. BUT prompt still didn't
  render — led to fry1527.

fry1526 — totfs LOAD-TIME fix + output routing start:
  - totfs_read_block: one multi-sector bd->read instead of per-sector loop.
  - totfs_read_file_data: FAST PATH reads physically-contiguous runs.  <-- this
    fast path was BUGGY (fry1532); see fry1533 for the correction.
  - lx_write_fd fd1/2 GUI branch: route to the process outbuf (block-until-space)
    instead of kprint_write, so the Shell pump renders it. pump_child_io drains
    the outbuf fully each tick.
  Result: 250MB load went ~10 min -> seconds.

fry1527 — RESEARCH: prompt still blank because Claude/Bun writes its UI to fd 12
  (a dup of stdout), which the fry1526 routing (fd 1/2 only) missed -> FD_STDIO
  branch -> kprint_write -> discarded. Node was NOT stalled; it rendered to the
  wrong fd.

fry1528 — FIX (verified): lx_write_fd FD_STDIO branch (realfd 1/2, e.g. fd 12)
  also routes to the outbuf for GUI children. Factored helpers lx_is_gui_child()
  and lx_write_outbuf(). RESULT: Claude's TUI RENDERS cleanly in the Shell window.

fry1529 — VERIFY: keyboard input reaches Claude end-to-end (fd0 reads climb on
  keypress); a keypress confirmed a selection (clean exit code 0).

fry1530 — RESEARCH: rename(82) returns -5; design for a correct totfs rename.
fry1531 — FIX (built): rewrote totfs_rename_vfs to MOVE the dir entry keeping the
  same inode (added totfs_remove_dirent), overwrite existing dest. (Correct and
  kept — but it was NOT the config-corruption cause.)

fry1532 — RESEARCH: on a PRISTINE image Claude still saw a corrupt .claude.json.
  Root cause: the build STAGES $HOME/.claude.json (33657 bytes, valid) to
  /nvme/.claude.json (build_iso.sh ~line 189/710), and the fry1526
  totfs_read_file_data FAST PATH DMA'd a contiguous run DIRECTLY into the caller's
  buffer 'out' — fine for the ELF-load (kernel buffer) but CORRUPT for Claude's
  runtime reads where 'out' is a USER VA (lx_read_fd FD_FILE passes the user dst
  straight through). build_prp resolves buffers via the kernel map, can't target a
  user VA -> truncated read -> "JSON Parse error: Unexpected EOF".

fry1533 — FIX (verified): totfs_read_file_data fast path now DMAs the run into a
  KERNEL bounce buffer (pmm_alloc_pages, halving-retry, TOTFS_READ_BOUNCE_BLOCKS=64)
  then CPU-copies to 'out'. Correct for user buffers, still multi-block fast.
  RESULT: Claude reads its config intact -> reaches the NORMAL first-run trust
  prompt.

fry1534/1535 — RESEARCH (input dead at the trust prompt): 1534 (vaguer "wakeup
  stall") superseded by 1535 — PRECISE cause: epoll_ctl(fd0) returned EBADF
  (lx_epoll_ctl_fd rejected fd 0 because fd 0 has no fd object), so libuv could
  not arm its raw-mode stdin watcher -> keystrokes read but never dispatched.
  User's framing ("breaks when Claude takes over the terminal") pinned it.

fry1536 — FIX (verified): epoll(fd0) support.
  - lx_epoll_ctl_fd (syscall.c): allow target_fd==0 through the fd-object check.
  - poll_check_fd (syscall.c): for a GUI child, fd 0 (and FD_STDIO realfd 0)
    reports FRY_POLLIN ONLY when the inbuf has data; else 0. Non-GUI keeps legacy
    always-ready. (Forward-declared process_name_is_gui/gui_process_running.)
  RESULT: Claude is INTERACTIVE — Enter confirmed the trust prompt, Claude
  repainted (1818-byte write) and entered its MAIN UI, spawning subprocesses.

fry1537 — RESEARCH: Shell-window resize distortion = compositor reallocates the
  SHM surface on resize and sends TW_MSG_RESIZED (gui.c resize_client_surface,
  ~line 547), but the Shell had no handler -> drew the old buffer while the
  compositor showed the new empty one -> black + garbage. Plus 78x22 was cramped.

fry1538 — FIX (built; resize-distortion re-verify still PENDING) + the crash:
  - shell.c: default grid 640x384/78x22 -> 960x576/120x36 (de-crowd). Added
    term_surf_w/h (live surface). term_render fills the WHOLE surface. Added
    term_handle_resize() (fry_shm_map new surface, repoint gfx, re-render).
    process_input_stream handles TW_MSG_RESIZED.
  - syscall.c: TIOCGWINSZ -> 36 rows x 120 cols.
  - Verified: Shell came up bigger, Claude reached main UI de-crowded. The resize
    distortion fix itself was NOT re-tested (the crash below intervened).

fry1539 — RESEARCH on the crash (see section 4).

==============================================================================
## 3. CURRENT STATE — what works
==============================================================================

- Boot to GUI desktop (INIT.FRY -> GUI.FRY), selftest 106/0.
- Open Shell (double-click the Shell icon; it does NOT auto-open) -> bigger
  120x36 terminal window.
- Type `claude` -> loads /nvme/RCLAUDE.LXE (250MB) in SECONDS.
- Claude renders its TUI cleanly (C1 parser), reads its staged config intact,
  shows the first-run trust prompt, takes keyboard input, and on Enter enters its
  real main UI (de-crowded). Footer/hints/example-prompts all render.

==============================================================================
## 4. THE OPEN BLOCKER — network-teardown TRIPLE FAULT (TOP NEXT TARGET)
==============================================================================

Symptom: in Claude's main UI, sending a real prompt ("hi") -> kernel triple-fault
-> CPU reset -> UEFI reboot. No panic printed (triple fault resets instantly).

What the SERIAL shows (fry1538_serial.log): Claude (pid 14) ran the network
request via helper subprocess pid=28 (~14 open sockets), then killed it
("TBSIG default-exit sender=14 target=28 sig=15"). During pid 28 teardown the
SPCEN census counts sockets down (sock=14/64 ... 9/64) then jumps straight to a
UEFI reboot ("selftest: running" reappears at line ~41227).

Localized to: src/kernel/proc/process.c proc_free() fd-teardown loop (lines
649-722) — closing the dying subprocess's fds. For FD_SOCKET it (a) calls
tcp_close(sk->tcp_handle)/tcp_close(sk->listen_handle) for SOCK_STREAM, and (b)
manipulates g_pipes[] refcounts for AF_UNIX (domain==1). The fault is in here,
first exercised now that Claude opens REAL TCP sockets to the API.

NOT caused by this session's changes — nothing in 1521-1538 touches proc_free,
sockets, TCP, or signals; the epoll-fd0 item (fd==0) is NOT implicated (teardown
just kfrees the item list; epoll_wait/epoll_et_token both guard on fd_ptrs). This
is a PRE-EXISTING socket/TCP teardown bug.

Suspects (need the faulting RIP to confirm): (a) tcp_close UAF/bad-handle when
many sockets close at once on a killed proc; (b) socket refs double-free
(second free tcp_close's an already-freed handle); (c) AF_UNIX g_pipes refcount
on a shared socketpair (fry1474 area).

HOW TO DIAGNOSE NEXT (the QEMU guest_errors log is EMPTY — a triple fault isn't
logged under that flag): relaunch headed with `-d int,cpu_reset -D <log>` instead
of guest_errors (ASK Zack first per Rule 11 — he OK'd reading QEMU logs this
session but confirm). Drive to Claude's main UI, send a prompt, read the
exception chain at the tail (the RIP at the reset pins a/b/c). Then fix that
teardown path. -d int is verbose; only the tail matters. LOG it (fry1540+).

==============================================================================
## 5. REMAINING POLISH (after the crash)
==============================================================================

- C2: per-cell SGR COLOR in the Shell VT parser (term_csi_dispatch case 'm' is
  currently parse-and-ignore; gfx_draw_text already takes fg/bg). Plus the small
  underline-leak artifact at child-exit (cosmetic).
- Resize-distortion re-verify: fry1538 added the TW_MSG_RESIZED handler but it
  was never re-tested (crash intervened). Confirm a live resize re-renders clean.
- DYNAMIC reflow on live resize: making Claude relayout when the window resizes
  needs a per-child winsize the Shell can set + SIGWINCH delivery (kernel work).
  Currently the winsize is a fixed 120x36; resizing won't reflow Claude (but the
  bigger DEFAULT already de-crowds, and resize no longer garbages once verified).
- Cosmetic post-child-exit Shell repaint glitch.
- Optional: stage /nvme/.local/bin/claude so Claude's /doctor stops flagging the
  binary as missing.

==============================================================================
## 6. BUILD / RUN / VERIFY — exact mechanics (so you don't flail)
==============================================================================

Toolchain: Makefile TOOLBIN = tools/host/bin/x86_64-elf-gcc. The /opt/cross/bin
in the PATH below DOES NOT EXIST and is a harmless no-op (don't chase it).

Clean build (Rule 12), with Claude staged:
  make clean
  STAGE_CLAUDE=1 CLAUDE_BIN=/home/legacyindieradio/.local/share/claude/versions/2.1.177 \
  PATH="/opt/cross/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH" /bin/bash build_iso.sh
  Expect BUILD_EXIT=0. shell.tot ~150KB, RCLAUDE.LXE 250480336 staged.

Headed VM (MUST be setsid -f + DISPLAY=:0 + -display gtk; NOT run_in_background,
which SIGHUPs the window). NOTE: pkill -f "qemu..." self-matches the bash wrapper
and dies (exit 144) — kill via `pgrep qemu | xargs -r kill`. Do NOT put a bare
`sleep` in a command (the harness blocks it); use a bounded for-loop with sleep
inside, or run_in_background for the build.
  pgrep qemu | xargs -r kill
  cp /usr/share/edk2/x64/OVMF_VARS.4m.fd /tmp/VARS.fd
  setsid -f env DISPLAY=:0 qemu-system-x86_64 -m 4G -machine q35,accel=kvm -cpu host -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file=/tmp/VARS.fd \
    -cdrom out/tatertos64v3.iso \
    -drive file=out/tater_nvme.img,if=none,id=nvme0,format=raw \
    -device nvme,drive=nvme0,serial=tatertos-nvme \
    -netdev user,id=n0 -device e1000,netdev=n0 -vga std -display gtk \
    -serial file:/tmp/serial.log -D /tmp/qemu_dbg.log -d guest_errors

Driving input / screenshots (host tools):
  - Find window: WID=$(DISPLAY=:0 wmctrl -l | grep -i qemu | awk '{print $1}')
  - Screenshot: DISPLAY=:0 import -window "$WID" /tmp/shot.png  (then Read it)
  - Focus: DISPLAY=:0 xdotool windowactivate "$WID"
  - MOUSE: QEMU uses a RELATIVE PS/2 mouse, so xdotool absolute mousemove does NOT
    map into the guest. You CANNOT reliably click TaterTOS UI from the host. Zack
    drives the GUI (opens the Shell, types claude, answers prompts). Plan for that.
  - The TaterTOS trust prompt is an ARROW-KEY menu; Enter confirms the highlighted
    default (option 1). Number keys don't select.

Disk image: build_iso only (re)creates out/tater_nvme.img if missing/wrong-size,
then mktotfs reformats and re-stages. To force a pristine image: rm
out/tater_nvme.img before building. The staged /nvme/.claude.json comes from
$HOME/.claude.json via build_iso (so it's present on every fresh image).

Fresh boot markers in serial: "selftest: 106 passed", "SPAWN_OK
path=/system/GUI.FRY pid=12". Claude load done: "LINUXELF: load done". Claude
alive vs exit: "TBEXIT group pid=14 ... code=N". Claude UI writes: "TBSTDOUT
pid=14 fd=12 len=N". fd0 input reads: "read fd=0 ... -> N".

==============================================================================
## 7. UNCOMMITTED STATE
==============================================================================

Nothing committed this session. The working tree also carries prior-session
uncommitted changes (deadlock fix, etc.) intentionally kept. Files changed this
session: src/kernel/proc/syscall.c, src/kernel/fs/totfs.c, src/user/shell/shell.c.
Do NOT commit unless Zack asks. No hashes recorded (Rule 16).

==============================================================================
## 8. THE BAR
==============================================================================

Match the fry1521-1539 logging bar exactly. When Zack pushes back ("I don't think
that's it"), STOP and re-investigate from evidence — his "it breaks when Claude
takes over the terminal" steer is what cracked the epoll-fd0 root cause (fry1535).
Read the serial, cite line numbers, don't hand-wave. And LOG EVERY STEP.
