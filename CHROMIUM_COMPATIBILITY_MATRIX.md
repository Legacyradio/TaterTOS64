# TaterTOS64 Chromium Compatibility Matrix

Date: 2026-05-13
Scope: Linux-style APIs added or exposed during the Chromium/Ladybird port
Related baseline: `CHROMIUM_PORT_AUDIT_REPORT.md`

## Classification Key

- `Native`: fits TaterTOS architecture and should remain part of the normal ABI.
- `Compat`: acceptable compatibility API, but must be documented as such.
- `Temporary`: enough for compile/link or smoke tests, but not enough to trust for Chromium runtime behavior.
- `Debt`: violates the desired boundary or hides incomplete semantics.

## Summary

The current compatibility layer is uneven. `pipe2`, `accept4`, `memfd_create`, and `sendfile` are close to defensible compatibility APIs. `eventfd`, `timerfd`, `epoll`, `signalfd`, and `inotify` need explicit semantic review before Chromium runtime work depends on them.

The largest risk is not the existence of Linux-style names by itself. The risk is exposing those names in public headers while some implementations are intentionally simplified or incomplete.

## Matrix

| API | Public Header | Kernel Object | Current Semantics | Classification | Keep Public? | Required Action |
| --- | --- | --- | --- | --- | --- | --- |
| `pipe2` | `unistd.h` | pipe fd pair | Creates pipe with `O_NONBLOCK` / `O_CLOEXEC`. Shares core pipe implementation. | Compat | Yes | Keep. Add tests for close-on-exec once exec exists. |
| `dup3` | `unistd.h` | fd table entry | Duplicates fd with `O_CLOEXEC`, but currently returns success when `oldfd == newfd`. Linux returns `EINVAL` for that case. | Debt | Yes, if semantics corrected | Fix `oldfd == newfd` to return `EINVAL`, then update smoke test. |
| `accept4` | `sys/socket.h` | socket fd | Accepts TCP sockets and applies `SOCK_NONBLOCK` / `SOCK_CLOEXEC`. Peer address is mostly zero-filled. | Compat | Yes | Keep. Improve peer address reporting before network-heavy browser tests. |
| `epoll` | `sys/epoll.h` | `FD_EPOLL` | Maintains watched fd list and calls `poll_check_fd`. Edge-trigger/one-shot constants exist but behavior is level-triggered only. Invalid operations may silently succeed. | Temporary | Maybe | Document supported subset, reject unsupported flags, fix ctl error behavior. |
| `eventfd` | `sys/eventfd.h` | `FD_EVENTFD` | Counter fd with read/write and semaphore mode. Blocking read currently returns `EAGAIN` instead of blocking. | Temporary | Yes | Decide whether TaterTOS supports blocking eventfd. If yes, implement wait/wake. If no, document nonblocking-only behavior. |
| `timerfd` | `sys/timerfd.h` | `FD_TIMERFD` | One-shot/periodic timers tracked in ms. Read on unexpired blocking timer returns `EAGAIN`; `old_value` is zeroed rather than real. | Temporary | Yes | Implement blocking read or document nonblocking subset. Return real old timer state. |
| `signalfd` | `sys/signalfd.h` | `FD_SIGNALFD` | API exists, but kernel comments say async user signal delivery is not implemented. Poll marks signalfd readable to avoid event-loop hangs. | Debt | No, not as normal ABI | Quarantine as Chromium shim or implement real pending signal semantics. |
| `inotify` | `sys/inotify.h` | `FD_INOTIFY` | Watch list and event queue exist. Events only come from operations routed through TaterTOS VFS hooks. Many Linux flags are exposed. | Temporary | Maybe | Document subset, reject unsupported flags, verify VFS event generation paths. |
| `memfd_create` | `sys/memfd.h` | `FD_MEMFD` | Memory-backed fd supports read/write/lseek/ftruncate/mmap path. Header exposes sealing/hugetlb flags, but syscall only accepts close-on-exec value mismatch. | Compat with header bug | Yes | Fix header/kernel flag values and reject or implement sealing flags consistently. |
| `sendfile` | `sys/sendfile.h` | fd transfer helper | Copies through kernel bounce buffer from file/memfd to file/memfd/socket/pipe. Capped at 1 MiB per call. | Compat | Yes | Keep. Document cap and test file-to-socket/pipe behavior. |

## Immediate Corrections

1. Fix `dup3(oldfd == newfd)`.
2. Fix `memfd_create` flag mismatch between `sys/memfd.h` and `SYS_MEMFD_CREATE`.
3. Mark `signalfd` as temporary or remove it from the default public surface until real signal semantics exist.
4. Make `epoll_ctl` return errors for duplicate add, missing mod/delete targets, invalid ops, and unsupported event modes.
5. Make blocking `eventfd` and `timerfd` behavior intentional instead of returning `EAGAIN` for both blocking and nonblocking paths.

## Compatibility Policy Draft

TaterTOS may expose Linux-style APIs only when all of these are true:

1. The API is needed by a serious port such as Chromium or Ladybird.
2. The implementation has documented semantics that match the exposed header.
3. Unsupported flags return `EINVAL`.
4. Compile-only shims are quarantined and named as temporary.
5. No new `linux/` or `asm-generic/` public header namespace is added without an explicit compatibility note.

## Recommended Order

1. Patch the current WebRTC `network.cc` blocker with `WEBRTC_TATERTOS`.
2. Correct the low-risk ABI mismatches: `dup3` and `memfd_create` flags.
3. Tighten `epoll` validation because Chromium event loops may lean on it.
4. Decide whether `signalfd` is a real API or a quarantined build shim.
5. Review `timerfd` and `eventfd` blocking semantics before browser runtime testing.

## Notes

This matrix does not say all compatibility APIs are bad. It says the project needs an explicit contract. Some compatibility is practical for a browser port, but every exported API must either be real enough to run Chromium or clearly labeled as a temporary shim.
