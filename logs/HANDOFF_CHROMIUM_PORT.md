TaterTOS Chromium Port - Handoff State
Date: 2026-05-07 16:00 CDT

=== Current State ===

gn gen succeeds with tatertos as target_os:
  gn gen out/tatertos --args='target_os="tatertos" target_cpu="x64" is_debug=false is_clang=false symbol_level=0'

ninja C++ compilation of //base:base succeeds for 2331/2336 targets.
The 5 remaining failures are Rust stdlib targets (core, alloc, std, etc.)
compiled for the x86_64-unknown-none target triple.

Files compiled: 138 .o files across 2331 targets.
Key archives: liballocator_base.a, liballocator_core.a, cpp.a (perfetto protos).

All 12 chrome_probe tests pass in QEMU. Kernel boots clean.

=== The Rust Block ===

The 5 Rust targets fail because rustc doesn't have System allocator trait
impl for x86_64-unknown-none. The error chain:
  //base:base → //build/rust:cxx_cppdeps → :cxx_rustdeps →
  //third_party/rust/cxx/v1:lib → ... → //build/rust/std/rules:core

These Rust targets are compiled by the HOST toolchain
(clang_x64_for_rust_host_build_tools) which targets the linux host.
The chain above is evaluated because cxx_cppdeps has a public_dep on
cxx_rustdeps when toolchain_has_rust is true.

The root cause: `.gni` files in GN are cached across toolchains.
gcc_toolchain.gni is parsed ONCE (in the default tatertos toolchain
context), and toolchain_has_rust gets cached as false. When the linux
host toolchain template runs, the cached false prevents rust tools
(rust_macro, rust_rlib) from being defined on that host toolchain.

Attempted fixes (all failed):
1. current_os != "tatertos" guard in rust.gni's enable_rust block
2. Override toolchain_has_rust in gcc_toolchain.gni template body
3. Separate local variable (gcc_has_rust) in template body
4. Override current_os = host_os for host build tools toolchain
None worked because .gni caching overrides all template-level changes.

=== Recommended Fix ===

The SIMPLEST approach: in //build/toolchain/gcc_toolchain.gni, at line 671
(the if (toolchain_has_rust) block), add a check:
    if (defined(toolchain_has_rust) && !toolchain_has_rust &&
        current_os != "tatertos") {
      toolchain_has_rust = true
    }
This forces toolchain_has_rust to true for any non-tatertos toolchain,
overriding the cached false from the .gni parse.

Alternative: move cxx_cppdeps outside the toolchain_has_rust check in
build/rust/BUILD.gn, and remove its dependency on cxx_rustdeps entirely
(already partially done — need to finish the cleanup).

=== Chromium Source Modifications (all in chromium-tatertos/src/) ===

Files that MUST be modified:
- build/config/rust.gni: Add is_tatertos case for rust_abi_target (line 358)
  (currently done via sed, reverts on git checkout)
- build/config/BUILDCONFIG.gn: is_tatertos, default toolchain, compiler config
- build/toolchain/tatertos/BUILD.gn: gcc_toolchain(x64) using x86_64-elf tools
- build/config/tatertos/BUILD.gn: compiler config, isystem paths
- build/config/tatertos/sysroot.gni: empty
- third_party/perfetto/include/perfetto/base/build_config.h: __tatertos__ block
- third_party/perfetto/src/base/periodic_task.cc: -Wuninitialized fix
- third_party/abseil-cpp/.../hash_function_defaults.h: wstring guards
- third_party/abseil-cpp/.../str_format/arg.h: wstring_view guards
- third_party/libxml/BUILD.gn: os_include=linux
- buildtools/third_party/libc++/__config_site: HAS_NO_* for tatertos
- BUILD.gn: minimal root with only //base:base
- partition_alloc build_config.h: PA_IS_TATERTOS → PA_IS_POSIX
- partition_alloc stack_trace_posix.cc: enum ternary fixes
- partition_alloc platform_thread_posix.cc: reinterpret→static cast

=== TaterTOS libc Gaps (in TaterTOS64/src/) ===

Already implemented:
- wchar.h, locale.h, link.h, elf.h, sys/cdefs.h, sys/syscall.h
- bits/types/mbstate_t.h
- errno codes: ENODEV, EPROTO, ETXTBSY, ENOTTY, ENOLINK
- math.h: all float/long-double variants, FP_* constants
- stdlib.h: strtold, mblen, system, mkstemp, mkdtemp, quick_exit, etc.
- string.h: wmem*, strcoll, strxfrm, strerror_r
- stdio.h: fgetpos, fsetpos, freopen, tmpfile, tmpnam, scanf variants
- time.h: mbstate_t, timespec_get, _POSIX_MONOTONIC_CLOCK
- pthread.h: pthread_attr_getstack, pthread_attr_getstacksize
- unistd.h: pread
- libc.c: strtoimax, strtoumax, wcstoimax, wcstoumax, exit_compat, etc.

=== Build Log Location ===

Full output of last ninja run: (no persistent log — captured inline)
The 138 .o files are in out/tatertos/obj/

=== Starting Fresh ===

To pick this up from a clean state:
1. cd ~/chromium-tatertos/src
2. git checkout -- .  (revert all changes)
3. Apply the patches listed above (13 files)
4. gn gen and ninja
