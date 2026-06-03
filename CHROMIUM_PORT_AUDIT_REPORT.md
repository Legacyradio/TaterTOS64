# TaterTOS64 Chromium Port Audit

Date: 2026-05-13
Scope: TaterTOS64 userspace/kernel ABI surface, Chromium patch footprint, current WebRTC blocker

## Executive Summary

The tree is muddied. The public BSD/POSIX-style network headers are cleaner than expected, but the userspace compatibility layer, several kernel syscalls, and parts of the Chromium port have drifted toward a Linux-compatibility strategy that is only partially abstracted. The current state is recoverable, but it is not a clean "native TaterTOS platform port" yet.

The most important pattern is inconsistency:

- Some areas correctly avoid importing Linux socket ABI into public headers.
- Other areas add Linux-style APIs directly into the TaterTOS libc and syscall surface for Chromium/Ladybird compatibility.
- Chromium itself has partial `TATERTOS` platform guards, but many files still assume generic POSIX implies Linux/BSD facilities.

## High-Confidence Findings

### 1. Public socket/TCP headers are mostly clean

Evidence:

- `src/include/sys/socket.h` exposes a limited socket API and does not define `IP_TOS`, `SO_TIMESTAMP`, or Linux interface flags.
- `src/include/netinet/tcp.h` currently only defines `TCP_NODELAY`.

Assessment:

This part supports the handoff claim that some Linux-specific socket constants were removed from the public OS headers.

### 2. The libc surface has been expanded with Linux-specific extensions

Evidence:

- `src/user/libc/posix.c` includes `sys/epoll.h`, `sys/eventfd.h`, `sys/timerfd.h`, `sys/signalfd.h`, `sys/inotify.h`, `sys/memfd.h`, and `sys/sendfile.h`.
- `src/include/sys/epoll.h` says "POSIX/Linux-compatible epoll declarations backed by TaterTOS syscalls".
- `src/include/sys/eventfd.h` says "Linux-compatible eventfd declarations backed by TaterTOS SYS_EVENTFD".
- `src/include/sys/inotify.h` says "POSIX/Linux-compatible inotify declarations backed by TaterTOS syscalls".
- `src/include/sys/signalfd.h` says "POSIX/Linux-compatible signalfd declarations backed by SYS_SIGNALFD".

Assessment:

This is the clearest evidence that the OS surface was widened to satisfy Chromium rather than keeping a narrower native ABI and isolating compatibility elsewhere.

### 3. Linux header namespaces were added inside the OS include tree

Evidence:

- Untracked headers exist under `src/include/linux/`:
  - `linux/auxvec.h`
  - `linux/futex.h`
  - `linux/unistd.h`
  - `linux/random.h`
  - `linux/magic.h`
  - `linux/kdev_t.h`
- `src/include/asm-generic/unistd.h` also exists.
- `src/include/sys/auxv.h` includes `<linux/auxvec.h>` and describes itself as a "Linux auxiliary vector stub".

Assessment:

This is direct Linux namespace bleed-through inside the TaterTOS SDK. Even if some of these are harmless compatibility stubs, they violate the architectural direction in `planv3.txt` unless they are explicitly quarantined as a hosted compatibility layer rather than treated as normal OS headers.

### 4. Kernel syscall work was explicitly shaped around Chromium needs

Evidence:

- `src/kernel/proc/syscall.c` states that the signalfd surface exists so "Chromium/Ladybird code compile and link".
- The same file states that `memfd_create` is used by Chromium for shared memory, ELF loading, Mojo shared buffers, and graphics allocation.
- The syscall switch labels a section "Phase 9: Chrome port - accept4, timerfd, signalfd, inotify".

Assessment:

This confirms the implementation direction was "add the Linux-like API Chromium expects" rather than "finish a TaterTOS-native abstraction and map Chromium onto it cleanly".

### 5. Some compatibility is semantic, not just syntactic

Evidence:

- `src/kernel/proc/syscall.c` says TaterTOS does not deliver async signals to user space, but still exposes `signalfd`.

Assessment:

This matters because it means some APIs were added as compile/link shims with reduced semantics. That is not automatically wrong, but it is exactly the sort of shortcut that can accumulate hidden runtime failures later in Chromium.

### 6. Chromium has a large, broad, and partially platform-aware patch set

Evidence:

- `git diff --stat` in `chromium-tatertos/src` shows 199 modified files.
- The patch set spans `base`, `build`, `chrome`, `components`, `crypto`, `third_party`, `ui`, and Rust toolchain files.
- `third_party/webrtc/BUILD.gn` defines `WEBRTC_TATERTOS`.
- `rtc_base/ifaddrs_converter.h` and `rtc_base/ifaddrs_converter.cc` already special-case `WEBRTC_TATERTOS`.
- `rtc_base/network.cc` still includes `<net/if.h>` under generic `WEBRTC_POSIX` and directly uses `IFF_LOOPBACK`, `IFF_RUNNING`, and `getifaddrs`.

Assessment:

The Chromium port is not a tight, minimal platform port yet. It has valid platform hooks in some places, but the patch footprint is wide and there are clear signs of unfinished abstraction work.

### 7. Worktree hygiene is weak

Evidence:

- `git status` in `TaterTOS64` shows many modified public headers and libc files, plus many untracked `fryNNNN.txt` logs and newly added include trees.
- `git status` in `chromium-tatertos/src` shows a very large active patch set.

Assessment:

This makes it harder to distinguish deliberate architecture from emergency build fixes. Before deeper cleanup, the current state should be inventoried and categorized.

## Current WebRTC Blocker

Current blocker:

- `third_party/webrtc/rtc_base/network.cc` still assumes that `WEBRTC_POSIX` implies `<net/if.h>`, `IFF_*`, and `getifaddrs`.

Why it matters:

- TaterTOS already introduced a `WEBRTC_TATERTOS` escape hatch in `ifaddrs_converter`, but `network.cc` did not receive the same boundary treatment.
- This is a strong example of the current muddiness: partial platformization, then fallback to generic POSIX assumptions.

## Architectural Conclusion

The problem is not simply "Linux code exists". The problem is that Linux-compatibility choices are currently spread across three layers without a clear contract:

1. TaterTOS public SDK headers
2. TaterTOS libc and kernel syscall surface
3. Chromium/TaterTOS platform glue

That spread is what creates the muddy state. A clean port needs one explicit policy for where compatibility lives.

## Recommended Direction

Preferred rule set going forward:

1. Keep the core TaterTOS public ABI POSIX-first and TaterTOS-native.
2. Treat Linux-style APIs as explicitly documented compatibility extensions, not as the default identity of the OS.
3. Put Chromium-specific assumptions behind `IS_TATERTOS` / `WEBRTC_TATERTOS` guards in Chromium where possible.
4. Only extend TaterTOS kernel/libc when the API is genuinely needed and can be supported with clear runtime semantics.
5. Do not add more Linux namespace headers unless they are quarantined and justified as compatibility shims.

## Cleanup Plan

### Phase 0 - Freeze and Inventory

- Stop adding new compatibility APIs until the current surface is categorized.
- Snapshot the current TaterTOS and Chromium worktrees.
- Classify existing changes into:
  - Native TaterTOS platform work
  - Legitimate compatibility extension
  - Temporary build shim
  - Suspected shortcut / architecture debt

### Phase 1 - Define the Boundary

- Write a short compatibility policy for TaterTOS:
  - Which Linux-style APIs are intentionally supported
  - Which are temporary
  - Which must not live in public headers
- Decide whether `src/include/linux/*` is allowed at all.
- If allowed, move those headers under an explicitly documented compatibility area and keep them out of the core identity of the OS.

### Phase 2 - Fix the Current Blocker Cleanly

- Patch `third_party/webrtc/rtc_base/network.cc` for `WEBRTC_TATERTOS`.
- Do not add `<net/if.h>`, `ifreq`, `IFF_*`, or `getifaddrs` to TaterTOS just to satisfy this file.
- Make TaterTOS behavior explicit:
  - either no interface enumeration yet
  - or a TaterTOS-specific enumeration path

### Phase 3 - Audit the Existing Linux-Compatible APIs

- Review `epoll`, `eventfd`, `timerfd`, `signalfd`, `inotify`, `memfd_create`, `sendfile`, `accept4`, `pipe2`, and `dup3`.
- For each API, record:
  - Why it exists
  - Whether semantics are complete
  - Whether Chromium actually needs runtime support or only compile/link surface
  - Whether the API belongs in public headers

### Phase 4 - Reduce Chromium Patch Scatter

- Separate pure build-system enablement from source-level behavior changes.
- Prefer narrow `IS_TATERTOS` / `WEBRTC_TATERTOS` guards over broad POSIX edits.
- Review the 199-file Chromium patch set and mark:
  - keep
  - rewrite
  - drop

### Phase 5 - Resume Build With Discipline

- After the boundary is clear, resume the Chromium build one blocker at a time.
- Every new patch should answer:
  - Is this a Chromium platform fix?
  - Is this a TaterTOS ABI decision?
  - Is this a temporary compatibility shim?

## Immediate Next Actions

1. Approve this audit as the baseline.
2. Patch the current WebRTC `network.cc` blocker using `WEBRTC_TATERTOS`, not Linux header expansion.
3. Start a compatibility matrix for the Linux-style APIs already present in TaterTOS.

## Bottom Line

The tree is muddied, and your suspicion is justified. The contamination is not uniform, but it is real:

- the socket headers are cleaner than expected,
- the libc/kernel compatibility surface has clearly expanded toward Linux,
- and Chromium contains a broad patch set with unfinished platform boundaries.

The correct move now is not a blind rebuild. It is to use this audit as the control document, fix the current blocker cleanly, and then reduce the compatibility sprawl instead of extending it.
