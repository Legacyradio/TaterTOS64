# POSIX Integration Plan

## Background & Motivation
The TaterTOS64 userland currently attempts to provide POSIX compatibility by defining standard POSIX types (e.g., `struct stat`, `time_t`) and functions within `libc.h`. This approach leads to "header hell"—conflicts between `libc.h` and the actual system headers in `src/include/` (like `<sys/stat.h>` or `<time.h>`) when third-party applications (like QuickJS or Ladybird) include both. To bypass these conflicts, a brittle workaround using `-D` flags in the `Makefile` (`LADY_POSIX_FLAGS`) was employed to remap standard POSIX calls to `posix_` or `fry_` prefixed wrappers. This violates the "no shortcuts" policy and makes the system fragile.

## Scope & Impact
This plan affects the core C library (`libc.h`, `libc.c`, `posix.c`, and a new `fry.h`) and the system headers in `src/include/`. It will also impact the build system (`Makefile`). By establishing a true POSIX layer, we ensure that native apps and ported software (like Ladybird) compile against standard symbols without header pollution or macro mapping hacks.

## Proposed Solution
1.  **Separate Public and Private APIs**: Split the ambiguous `libc.h` into clear domains:
    *   **Public POSIX/libc headers**: Located in `src/include/*.h` (e.g., `<sys/types.h>`, `<unistd.h>`).
    *   **Private Syscall ABI**: Located in a private header such as `src/user/libc/fry.h` (containing only `fry_` definitions).
    *   **Private libc Internals**: If needed, a strictly internal header for libc implementation details.
2.  **Empower System Headers**: Source files must include the correct public headers (`<sys/types.h>`, `<time.h>`, `<fcntl.h>`, `<unistd.h>`, etc.) instead of relying on an overloaded `libc.h`.
3.  **Robust System Headers**: Each public header in `src/include/` must be self-contained and include-order independent.
4.  **Real POSIX Symbols**: Implement standard POSIX functions directly in `src/user/libc/posix.c` (or similar files) using their canonical signatures.
5.  **Explicit ABI Translation**: `posix.c` must explicitly translate between POSIX structs (e.g., `struct stat`, `struct sockaddr`) and `fry_` ABI structs at the syscall boundary, rather than relying on accidental layout compatibility.
6.  **Remove Macro Hacks**: Eliminate `LADY_POSIX_FLAGS` from the `Makefile`. Applications will link against real POSIX symbols.

## Alternatives Considered
-   **Continuing with `-D` mappings**: Rejected because it requires maintaining a massive, error-prone list of macros for every ported application, and it doesn't solve the underlying header conflicts when types are redefined.
-   **Prefixing all system headers**: e.g., `<tater/stat.h>`. Rejected as it breaks upstream compatibility for ports like Ladybird, requiring extensive patching of third-party source code.

## Implementation Plan

### Phase 1: Header Extraction (Syscall ABI vs Public Headers)
-   Create `src/user/libc/fry.h` to house all `fry_` prefixed ABI structs, constants, and syscall wrappers. **Do not expose `fry.h` to third-party app include paths unless absolutely necessary; it is for libc/userland-internal use.**
-   Strip `src/user/libc/libc.h` of all POSIX types, structs, and function declarations. Public C/POSIX declarations must move to, or remain in, `src/include/`.
-   Update all TaterTOS userland source files to include the correct public headers (`<sys/types.h>`, `<time.h>`, `<fcntl.h>`, `<unistd.h>`, etc.) and/or the private `fry.h` as appropriate.

### Phase 2: Harden System Headers
-   Audit and fix headers in `src/include/` (e.g., `<sys/socket.h>`, `<netdb.h>`, `<poll.h>`, `<sys/stat.h>`) to ensure they contain all necessary standard POSIX definitions that were previously hiding in `libc.h`.
-   **Include Order Audit**: Create a temporary test harness to compile tiny files that include headers one at a time, and in common combinations, to prove they are self-contained and include-order independent:
    -   `<sys/types.h>` + `<sys/stat.h>`
    -   `<time.h>` + `<sys/time.h>`
    -   `<sys/socket.h>` + `<netinet/in.h>` + `<arpa/inet.h>` + `<netdb.h>`
    -   `<fcntl.h>` + `<unistd.h>`

### Phase 3: Implement Canonical POSIX Symbols
-   Update `src/user/libc/posix.c` (and related files like `netdb.c`, `pthread.c`, `time_ext.c`) to implement the canonical POSIX functions without any prefixes (e.g., `int open(const char *path, int flags, ...)`).
-   **Standardized Error Handling**: Implement a single helper function/macro in the libc internals that consistently converts TaterTOS negative return values into a `-1` return and sets `errno` appropriately. All POSIX wrappers must use this helper.
-   **Explicit Struct Translation**: Where POSIX structs differ from TaterTOS ABI structs (e.g., `struct stat` vs. `struct fry_stat`), the wrapper functions must explicitly map fields back and forth, guaranteeing ABI stability.
-   **Varargs Handling**: Ensure varargs functions exhibit canonical behavior. Specifically, `open(const char *path, int flags, ...)` must only attempt to read a `mode_t` argument if `O_CREAT` (or equivalent flags requiring a mode) is present in `flags`.

### Phase 4: Verification and Makefile Cleanup
-   **Targeted Checks (Required before Makefile cleanup)**:
    -   Use `x86_64-elf-nm` on the built libc archive (`libc.a` or `libc.o`) to verify that canonical POSIX symbols (`open`, `socket`, `stat`, etc.) exist and are properly exported.
-   **Clean Build System**:
    -   Remove `LADY_POSIX_FLAGS` from `TaterTOS64/Makefile`.
    -   Ensure `LIBCORE_CXXFLAGS` and `LIBURL_CXXFLAGS` no longer rely on these mappings.
-   **Compile-Command Checks**: Inspect the generated compile commands for Ladybird (and other apps) to confirm `-Dopen=...`, `-Dsocket=...`, etc., are completely absent.
-   **Full Build**: Run `build_iso.sh` to compile the entire OS and all applications. Success here is necessary but not sufficient without the `nm` and compile-command checks passing.

## Migration & Rollback
-   **Migration**: Existing TaterTOS apps relying on `_compat` aliases or `fry_` calls for standard operations should ideally be migrated to the new canonical POSIX names. Compatibility aliases can remain temporarily to ease the transition, but **only if they do not reintroduce macro remapping or duplicate public declarations.**
-   **Rollback**: The current state of the workspace should be committed to version control (Git) before beginning Phase 1. If the refactoring fails catastrophically, we will use Git to revert the working tree to this known-good state. Logs in `logs/fry852.txt` will serve as context for the attempt, but Git is the true rollback mechanism.