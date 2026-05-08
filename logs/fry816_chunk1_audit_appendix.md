# Ladybird → TaterTOS64v3 Port — Chunk 1 Audit

**Upstream snapshot:** `~/ladybird-upstream/` (shallow clone, main, 2026-04-25)
**Target:** Replace TaterSurf with Ladybird's LibWeb engine, ported on top of TaterTOS's libc shim and netcore.
**Scope of this audit:** AK + LibCore only. LibJS / LibWeb / LibGfx are later chunks.

---

## 1. Code size

| Module | Files (.h/.cpp) | LOC |
|---|---|---|
| AK | 196 | 43,168 |
| LibCore | 104 | 13,231 |

## 2. Layering

- **AK** has **zero** dependencies on LibCore. Only 5 C++ stdlib headers used (`initializer_list`, `iterator`, `memory`, `new`, `utility`) — all header-only template glue, no libstdc++ runtime needed.
- **LibCore** depends on AK + `LibUnicode`, `LibURL`, `LibTextCodec`, `Threads::Threads` (pthreads).
  - LibUnicode/LibURL/LibTextCodec are themselves transitively in scope for full LibWeb. For Chunk 2 we can stub them with empty translation units and bring them in for Chunk 4.

## 3. Third-party headers (must vendor)

Required by AK:
- `fmt/format.h` — fmtlib. Header-only build available.
- `fast_float/fast_float.h` — header-only.
- `simdutf.h` — single-source amalgamated build available.

Action: vendor all three under `src/user/apps/ladybird/3rd/` exactly like BearSSL/mbedTLS are vendored.

## 4. POSIX / libc surface

### 4a. Headers used (already shimmed for openh264 = ✅, missing = ⚠️)

| Header | AK | LibCore | TaterTOS shim status |
|---|---|---|---|
| `errno.h` | ✅ | ✅ | ✅ exists |
| `math.h` | ✅ | — | ✅ exists |
| `stdarg.h` | ✅ | ✅ | ✅ exists |
| `stdio.h` | ✅ | ✅ | ✅ exists |
| `stdlib.h` | ✅ | ✅ | ✅ exists |
| `string.h` | ✅ | ✅ | ✅ exists |
| `time.h` | ✅ | ✅ | ✅ exists |
| `limits.h` | — | ✅ | ✅ exists |
| `fcntl.h` | — | ✅ | ⚠️ NEW |
| `signal.h` | — | ✅ | ✅ exists |
| `unistd.h` | — | ✅ | ⚠️ NEW (or merge into existing) |
| `dirent.h` | — | ✅ | ⚠️ NEW |
| `pthread.h` | ✅ | ✅ | ⚠️ NEW (largest single shim — see §5) |
| `spawn.h` | — | ✅ | ⚠️ stub-only (no subprocess on TaterTOS) |
| `termios.h` | — | ✅ | ⚠️ stub-only (no TTY discipline) |
| `sys/types.h` | — | ✅ | ✅ exists |
| `sys/stat.h` | — | ✅ | ⚠️ NEW |
| `sys/mman.h` | — | ✅ | ⚠️ NEW (mmap/munmap → kernel page mapper) |
| `sys/ioctl.h` | — | ✅ | ⚠️ stub-only |
| `sys/select.h` | — | ✅ | ⚠️ NEW (select → netcore poll) |
| `sys/socket.h` | — | ✅ | ⚠️ NEW (socket → netcore) |
| `sys/time.h` | — | ✅ | ✅ exists |
| `sys/inotify.h` | — | ✅ | ⚠️ skip — file goes to FileWatcherUnimplemented |

### 4b. Top POSIX call sites (cumulative across AK + LibCore)

```
110 socket    90 read       71 close      58 stat       43 connect
 40 write     32 open       29 fcntl      25 accept     22 ioctl
 22 bind      19 listen     17 mmap       16 munmap     15 getpid
 14 send      14 fstat      12 setsockopt  9 select       9 getsockopt
  8 sigaction  8 sendto      8 recvfrom    8 readlink    8 pipe
  8 mkdir      8 getaddrinfo 8 dup         7 pthread_*   7 poll
  7 getcwd     6 waitpid     6 unlink      6 rmdir       6 chdir
  5 symlink    5 rename      5 recv        5 kill        4 tcsetattr
  4 tcgetattr  4 fork        4 dup2        3 getuid      2 readdir
```

## 5. Required new shim work (grouped by effort)

### Group A — small, mechanical (≈1 day each)
- `fcntl.h` — `O_RDONLY`, `O_WRONLY`, `O_CREAT`, `O_TRUNC`, `O_NONBLOCK`; `open`, `fcntl(fd, F_SETFL, ...)` for non-blocking.
- `unistd.h` — `read`, `write`, `close`, `lseek`, `dup`, `dup2`, `pipe`, `getpid`, `getuid`, `chdir`, `getcwd`, `unlink`, `rmdir`, `readlink`, `symlink`, `rename`. All map cleanly to TaterTOS file/process syscalls.
- `dirent.h` — `DIR`, `opendir`, `readdir`, `closedir`. Map to TotFS directory iteration.
- `sys/stat.h` — `struct stat`, `stat`, `fstat`, `mkdir`. Map to TotFS metadata.
- `sys/mman.h` — `mmap`/`munmap`. TaterTOS already has page mapper in kernel; expose as syscall.

### Group B — netcore wiring (≈3–5 days)
- `sys/socket.h`, `sys/select.h` — Map BSD sockets onto `src/drivers/net/netcore.c`. Need: `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, `setsockopt`, `getsockopt`, `select`, `poll`, `getaddrinfo`, `getsockname`, `getpeername`. Most of TaterSurf's existing TLS path already does this — extract & generalize.

### Group C — pthread shim (≈3 days, largest)
- `pthread_t`, `pthread_create`, `pthread_join`, `pthread_self`, `pthread_mutex_*`, `pthread_cond_*`, `pthread_rwlock_*`, `pthread_key_create`/`pthread_getspecific` (TLS), `pthread_once`.
- TaterTOS has kernel threads (used by gui/evloop). Map pthread API onto kernel thread + futex primitives.

### Group D — stub-only (returns ENOSYS / unused, ≈half-day)
- `spawn.h`, `termios.h`, `sys/ioctl.h`, `sys/inotify.h`. None of these are needed for in-page rendering. LibCore has `FileWatcherUnimplemented.cpp` we'll use for inotify; spawn is only used for IPC subprocesses (Ladybird's multi-process model — we'll run single-process for v1).

## 6. Files we exclude outright (Windows / macOS specific)

```
AK/DemangleWindows.cpp
Libraries/LibCore/AnonymousBufferWindows.cpp
Libraries/LibCore/EventLoopImplementationWindows.cpp
Libraries/LibCore/LocalServerWindows.cpp
Libraries/LibCore/TCPServerWindows.cpp
Libraries/LibCore/ProcessWindows.cpp
Libraries/LibCore/SocketWindows.cpp
Libraries/LibCore/SocketpairWindows.cpp
Libraries/LibCore/SystemWindows.cpp
Libraries/LibCore/UDPServerWindows.cpp
Libraries/LibCore/Platform/ProcessStatisticsMach.cpp
Libraries/LibCore/Platform/ScopedAutoreleasePool.mm
Libraries/LibCore/IOSurface.cpp                      (Apple-only)
Libraries/LibCore/MachPort.cpp                       (Apple-only)
Libraries/LibCore/TimeZoneWatcherFSEvents.cpp        (macOS)
```

We **keep** the `*Unix.cpp`, `*Inotify.cpp`, and unsuffixed cross-cutting files. `FileWatcherInotify.cpp` gets swapped for `FileWatcherUnimplemented.cpp`.

## 7. Build system

Ladybird uses CMake + Ninja + a vendored `Toolchain/BuildVcpkg.sh` for deps. We will **not** use that. We will:
- Add a single new Make target `tatersurf2.fry` to `Makefile`.
- List AK + LibCore source files explicitly under `src/user/apps/ladybird/`.
- Drive everything through the existing AUR `x86_64-elf-g++ 15.2.0`.

This matches how openh264, BearSSL, mbedTLS, QuickJS, Opus are integrated.

## 8. Chunk 2 entry conditions

Chunk 2 (shim layer) starts when this report is signed off. First file Chunk 2 will create:
```
src/user/apps/ladybird/shim/pthread.h
src/user/apps/ladybird/shim/pthread.c   (maps to TaterTOS kthread + futex)
src/user/apps/ladybird/shim/sys/socket.h
src/user/apps/ladybird/shim/sys/socket.c (maps to netcore)
... etc per §5
src/user/apps/ladybird/3rd/fmt/        (vendored)
src/user/apps/ladybird/3rd/fast_float/ (vendored)
src/user/apps/ladybird/3rd/simdutf/    (vendored)
```

End of Chunk 2 = `AK.a` and `LibCore.a` build clean against the cross-compiler with zero warnings outside the shim itself.

---

**Risks / unknowns flagged:**
1. **C++23 std lib glue.** AK uses `<initializer_list>`, `<iterator>`, `<memory>`, `<new>`, `<utility>`. These are part of libc++/libstdc++ headers. The cross-compiler ships its own libstdc++ headers — verify they work freestanding (`-ffreestanding -fno-exceptions -fno-rtti`). If not, vendor a minimal libc++ shim.
2. **Exceptions.** Ladybird builds with exceptions enabled. TaterTOS userland today probably doesn't have `__cxa_throw`/unwind. Decision needed: build Ladybird with `-fno-exceptions` (Serenity originally did this) or bring in libunwind. Recommend `-fno-exceptions` + audit the few `throw` sites.
3. **Multi-process IPC.** Real Ladybird sandboxes RequestServer / ImageDecoder / WebContent into separate processes connected by `LibIPC`. v1 on TaterTOS will be single-process. This is a Chunk 5/6 concern, not Chunk 2.
