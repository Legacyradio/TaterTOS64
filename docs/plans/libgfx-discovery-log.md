# LibGfx Integration Discovery Log

## [2026-04-27 05:15] Task Initiation
- **Goal**: Establish a TaterTOS graphics backend for Ladybird.
- **Baseline**: POSIX integration complete, `ak_libcore_smoke` verified.

## [2026-04-27 05:52] Discovery: Pixel Format
- **TaterTOS Format**: ARGB8888 (Little-endian: 0xAARRGGBB).
    - Red: bits 16-23
    - Green: bits 8-15
    - Blue: bits 0-7
    - Alpha: bits 24-31 (used as 0xFF for opaque in Lerp/Fill).
- **Transparency Quirk**: `gfx_draw_char` uses `0xFF000000` as a magic "transparent" value instead of 0x00.
- **Interoperability**: Ladybird's `BGRA8888` (memory order) matches TaterTOS `ARGB8888` if we consider endianness correctly. 0xAARRGGBB in memory is [BB, GG, RR, AA] on little-endian.
- **Next Step**: Create the formal integration plan and identify files to copy from upstream.
