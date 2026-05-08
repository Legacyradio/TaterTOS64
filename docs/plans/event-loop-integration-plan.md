# Event Loop & Input Integration Plan

## Objective
Implement a TaterTOS-specific event loop backend for `LibCore` and integrate system input events (keyboard/mouse) into the Ladybird event queue.

## Implementation Plan

### Phase 1: Event Loop Backend
- Create `TaterTOS64/src/user/apps/ladybird/Libraries/LibCore/EventLoopImplementationTaterTOS.{h,cpp}`.
- Implement `EventLoopImplementationTaterTOS`:
    - Use `poll()` for multiplexing.
    - Implement a `wake()` mechanism using a self-pipe (`pipe()`).
    - Back `pump()` and `exec()` with the TaterTOS syscalls.
- Implement `EventLoopManagerTaterTOS`:
    - Manage timers using a `BinaryHeap` of expirations.
    - Manage notifiers (file descriptor observers).
- Update `LibCore/EventLoop.cpp` (if needed) or `EventLoopImplementation.cpp` to use the TaterTOS manager.

### Phase 2: Input Plumbing
- Create a `TaterTOSInputNotifier` class:
    - This will be a `Core::Notifier` that monitors a special "input device" or polls `fry_kbd_event` / `fry_mouse_get` during the `pump()` phase.
    - Translate `fry_key_event` to `Core::KeyEvent`.
    - Translate `fry_mouse_state` to `Core::MouseEvent`.
- Register the input notifier with the main event loop.

### Phase 3: Build & Verification
- Update `TaterTOS64/Makefile` to include the new `EventLoop` source files.
- Create `TaterTOS64/src/user/apps/ladybird/test/event_smoke.cpp`:
    - Initialize a `Core::EventLoop`.
    - Register a timer that prints a message every second.
    - Register an input handler that quits the loop when 'q' is pressed.
    - Verify asynchronous behavior (timers firing while waiting for input).
- Verify the build and runtime behavior in QEMU.

## Verification & Testing
- **Timer Accuracy**: Confirm that 1s timers fire approximately once per second.
- **Responsiveness**: Confirm that keyboard/mouse events are processed without significant lag.
- **Wake Mechanism**: Confirm that `loop.wake()` correctly interrupts a sleeping `poll()`.
