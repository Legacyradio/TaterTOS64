# HANDOFF — fry1543: TCP conn ownership for safe teardown + input pass-through for child TUI (Claude main UI)
Date: 2026-06-15 ~23:55 CDT
Session: continuation of fry1541 (beacon run that hit the crash) + fry1542 (diagnosis) + this implementation/verify attempts.
Author: per planv3 + CLAUDE.md + prior handoff.

## TL;DR / MILESTONE
- TCP ownership/refcount model implemented on struct tcp_conn (refs, retain on alloc/acquire, release only on last owner does real close).
- This was the direct fix for the "network-teardown triple fault" (UAF/bad-handle in tcp_close during proc_free shared_release for real SOCK_STREAM on killed helper with many TCP conns — first exercised when Claude does API calls).
- Input pass-through in Shell process_input_stream made stricter for child in alt-screen/raw takeover (force to term_child_pid, Enter standardized to \r, gate relaxed so ascii=0/specials forward to child).
- Multiple clean rebuilds (Rule 12, all exit 0) + headed launches.
- Evidence: TBPROCIN beacons now show Enter (0a/00 -> \r) and letters reaching RCLAUDE pid (delivery improved); Shell still eats some controls in samples; RCLAUDE loads but its threads heavy on futex timeouts + occasional sig=11 (SEGV) in its own bad-RIP code (same pattern as starvation era).
- User drove (mouse click in TUI input area + type prompt + Enter) across attempts.
- Result: TUI still not giving visible "accept" (no echo/update on typed text, Enter does little per user reports) despite some bytes landing on child. App internal state (futex spin, faults) appears to be the blocker for the TUI reacting.
- The TCP fix is in place. The verify (drive a prompt that visibly triggers network + helper kill + teardown, confirm clean/no panic thanks to refs) was not fully completed because keyboard input in the Claude TUI wasn't giving the user usable submit feedback.
- QEMU -D log remained empty (not useful for input path).

## WHAT WAS DONE (changes + WHY)
**TCP ownership (main fix for teardown panic):**
- netcore.c: added refs to tcp_conn; init=1 on tcp_connect/tcp_listen; tcp_conn_retain/release helpers; tcp_close now delegates to release (only last ref does FIN/state/active=0); accessors hardened.
- process.c shared_release (teardown site): calls now via tcp_close with comment.
- syscall.c (SYS_CLOSE, lx_close, dup2 close, etc.): parallel sites updated with comments.
- WHY: previous direct tcp_close on last sk ref had no protection on conn side; bulk close on killed proc with real TCP (Claude helper) hit UAF/double-close in tcp_close callers. Refs + release centralize ownership; safe idempotent; matches resource patterns (g_pipes, SPCEN, locked exits).

**Input pass-through (to enable driving the test case that hits the teardown):**
- shell.c process_input_stream (the key forwarding for child):
  - Relaxed ascii!=0 gate for children (forward on PRESSED even ascii=0).
  - Enter (0/\n/\r) standardized to \r for child (standard terminal submit).
  - Force target = forward_pid ? forward_pid : term_child_pid (catch cases where caller uses 0 but child has terminal via term_child_pid).
- WHY (in code comments + this log): prior gate + some paths caused Shell consumption (beacons to pid=13 for controls) even in takeover; child needs all pressed keys (incl. ascii=0 for specials) + proper Enter so TUI can accept/submit. Force ensures pure pass-through when term_child_pid set (alt-screen child). Matches handoff on chain stopping in alt-screen/raw.

Multiple clean rebuilds + headed launches (exact prior handoff template).

Evidence captured: live serial samples after each drive (TBPROCIN to RCLAUDE for some user keys + Enter; Shell still some; app futex/SEGV in own code; load done).

## CURRENT STATE
- TCP refcount/ownership: in (netcore + process + syscall sites).
- Input: force pass-through in (should give child all keys, Enter as \r).
- TUI (Claude main UI): loaded (LINUXELF), some bytes reach its pid per beacons (incl. Enter now standardized), but user reports "still nothing" / "TUI not accepting typing" / "Enter does little" despite mouse clicks in the prompt/input area + type + Enter. Shell consumption visible in some beacons. App threads heavy futex timeout spin + sig=11 in its own code (bad RIP pattern).
- No full network + helper kill + teardown observed in final attempts (user couldn't get visible submit in TUI).
- No new kernel panic observed in these runs (fix is in; full test case not driven to teardown).
- QEMU -D empty (not logging input).
- GUI intact (no regression).

## OPEN / BLOCKER (same as handoff + this session)
The top open remains driving a real prompt in the Claude TUI (main UI) that causes network/API + spawns/kills helper, then observe the teardown (SPCEN for pids, confirm no panic thanks to the refcount changes).
- Keyboard in TUI still not giving user visible accept/echo or submit (despite delivery improvements per beacons). Possible: TUI focus (mouse click inside TUI widget may be needed beyond window), app internal state (futex + own SEGV on input), TUI expects richer input (full key events/escapes rather than ascii byte), or single-byte delivery + current translation doesn't match what the app's input handler wants in raw/alt takeover.
- If keyboard remains insufficient for visible interaction: use mouse *inside the Claude TUI* (click prompt field + buttons like "for shortcuts"/"for agents") to drive the action that triggers the network call, even if typed text isn't echoing visibly. The serial will still show activity from the RCLAUDE pid, helper spawn, kill, and teardown behavior.
- Further input work if needed for usable keyboard: forward full tw_msg_key_t (code + flags) or proper terminal escapes for child in alt-screen; ensure mouse events also pass through to child for internal focus.

Repro the teardown case now that the fix is in (expect clean).

## HOW TO CONTINUE / DRIVE THE TEST
1. (Re)launch headed (same exact command as this session and prior handoff).
2. Open Shell, `claude`, trust -> main UI (120x36).
3. Mouse-click the input/prompt field *inside the Claude TUI* (the specific area with the prompt template).
4. Type a real prompt that triggers network (the one that spawns the helper and will cause kill/teardown).
5. Hit Enter (or use mouse in TUI to submit if keyboard not giving feedback).
6. Watch serial for: RCLAUDE activity (TBSTDOUT, network), helper spawn (new pid), kill, SPCEN exits, and teardown (no kernel panic — the refcounts should make tcp_close safe on the sockets).
7. If TUI keyboard still "nothing": fall back to mouse in TUI to drive the network-triggering action. Serial will still capture the pids and teardown.
8. Sample live: grep TBPROCIN /tmp/serial.log (confirm to RCLAUDE only); grep for tgid=42 or the RCLAUDE pid, LINUXELF, SPCEN, !EXC, PANIC.
9. If panic reappears: use -d int,cpu_reset -D for deeper (but ask per prior Rule 11 note; serial is primary).

Build/run mechanics: identical to this session and HANDOFF_claude_interactive_2026-06-15.md (STAGE_CLAUDE=1 clean rebuild; headed setsid with -serial /tmp/serial.log etc.; user drives mouse/keyboard in QEMU window + clicks *inside* TUI; serial is the source of truth for delivery + app behavior).

## FILES CHANGED THIS SESSION (for the log)
- netcore.c (struct + alloc + release + tcp_close + accessors)
- process.c (shared_release teardown)
- syscall.c (close paths)
- shell.c (process_input_stream pass-through + Enter map)
All with WHY comments. Multiple clean rebuilds.

Nothing committed (per standing).

## WHERE I LEFT IT / GOTCHAS (for next)
- TCP fix in and built — should make teardown safe.
- Input delivery improved (force + map) — some bytes now reach child (beacons), Shell consumption should be eliminated by term_child_pid force.
- But user reports TUI still not accepting (no visible typing/Enter effect). App internal (futex spin + own SEGV) is the visible "shit" in serial.
- QEMU -D empty for input (use guest serial).
- Drive the full test case as above to validate the TCP fix.
- If TUI keyboard remains unusable for submit, use mouse in TUI to trigger network action for the teardown test.

Handoff complete per user request. Session wrap.
