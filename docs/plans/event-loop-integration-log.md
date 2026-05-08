# Event Loop & Input Integration Discovery Log

## [2026-04-27 07:35] Discovery: Event Loop Architecture
- `LibCore::EventLoop` uses a provider model via `EventLoopManager` and `EventLoopImplementation`.
- `EventLoopImplementationUnix` is the primary reference.
- **TaterTOS Support**:
    - `poll()` is implemented in `libc/posix.c`.
    - `pipe()` / `read()` / `write()` for the wake-pipe are available.
    - `MonotonicTime` can be backed by `fry_gettime()`.
- **Strategy**: 
    1. Create `EventLoopImplementationTaterTOS` by adapting `EventLoopImplementationUnix`.
    2. Implement `EventLoopManagerTaterTOS`.
    3. Bridge `fry_kbd_event` and `fry_mouse_get` into the `pump()` phase or a dedicated Notifier.

## [2026-04-27 07:36] Next Steps
- Finalize the formal Integration Plan.
- Port necessary `LibCore` dependencies for the event loop.
- Implement the TaterTOS backend.
