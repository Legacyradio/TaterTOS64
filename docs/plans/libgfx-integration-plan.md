# LibGfx Integration Plan

## Objective
Implement a TaterTOS-specific graphics backend for the Ladybird engine by bridging `LibGfx::Painter` to the TaterTOS `src/user/libc/gfx.h` software rendering library.

## Key Files & Context
- **Upstream Source**: `/home/legacyindieradio/ladybird-upstream/Libraries/LibGfx/`
- **Target Location**: `TaterTOS64/src/user/apps/ladybird/Libraries/LibGfx/`
- **TaterTOS Gfx API**: `src/user/libc/gfx.h` (`gfx_ctx_t`, `gfx_fill`, `gfx_blit`, etc.)

## Implementation Plan

### Phase 1: Pull Core Components
- Create the target `LibGfx` directory in the port.
- Copy the following essential files from upstream:
    - `Color.h`, `Color.cpp`
    - `Point.h`, `Point.cpp`, `Size.h`, `Size.cpp`, `Rect.h`, `Rect.cpp`
    - `Bitmap.h`, `Bitmap.cpp`
    - `Painter.h`, `Painter.cpp`
    - `AffineTransform.h`, `AffineTransform.cpp`
    - `Path.h`, `Path.cpp`
    - `Forward.h`, `ScalingMode.h`, `CompositingAndBlendingOperator.h`, `WindingRule.h`
- **Porting Adjustments**:
    - Stub out `Core::AnonymousBuffer` usage in `Bitmap.cpp` if it causes heavy dependencies.
    - Ensure `Color.h` correctly maps its components to TaterTOS ARGB8888.

### Phase 2: Implement TaterTOS Backend
- Create `src/user/apps/ladybird/Libraries/LibGfx/PainterTaterTOS.h` and `PainterTaterTOS.cpp`.
- Class `PainterTaterTOS` will inherit from `Gfx::Painter`.
- **Implementation Mapping**:
    - `clear_rect` / `fill_rect` -> Map to `gfx_fill`.
    - `draw_bitmap` -> Map to `gfx_blit`.
    - `stroke_path` / `fill_path` -> Initial stubs (will need a software rasterizer for paths later).
    - `save` / `restore` -> Implement a state stack for clipping and transforms.
- Update `Painter::create` in `Painter.cpp` (ported) to return a `PainterTaterTOS` instead of `PainterSkia`.

### Phase 3: Build & Verification
- Update `TaterTOS64/Makefile` to include the new `LibGfx` objects.
- Create `src/user/apps/ladybird/test/gfx_smoke.cpp`:
    - A minimal app that creates a `Gfx::Bitmap`.
    - Creates a `Gfx::Painter` for that bitmap.
    - Draws a few rectangles and colors.
    - Blits the result to the TaterTOS framebuffer using `gfx_blit`.
- Verify the build and visual output in QEMU.

## Verification & Testing
- **Linkage**: Confirm `LibGfx.a` (or its equivalent objects) links into the smoke test without undefined references.
- **Rendering**: Confirm that rectangles and colors drawn via `LibGfx::Painter` appear correctly on the TaterTOS screen.
