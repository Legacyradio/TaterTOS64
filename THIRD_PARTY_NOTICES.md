# Third-Party Notices

TaterTOS64v3 itself is licensed under the **GNU General Public License v3.0**
(see [LICENSE](LICENSE)). The kernel, drivers, networking/TLS stack,
filesystems, GUI, `libc`, and the TaterSurf browser engine are original code.

To build the browser and a few userspace components, the project bundles the
third-party libraries listed below. Each is free/open-source software under a
license compatible with the GPL v3, and each retains its own upstream license
text in its directory.

> Note: the Linux compatibility subsystem copies a stock host glibc and
> `ld-linux` into the boot image **at build time**; those binaries are **not**
> contained in this repository.

## Bundled libraries

| Component | Location | License | License file |
|---|---|---|---|
| FreeType 2.13.2 | `src/user/ports/freetype-2.13.2/` | FreeType License (FTL) **OR** GPL-2.0 | `LICENSE.TXT` |
| libjpeg (jpeg-9e) | `src/user/ports/jpeg-9e/` | IJG (Independent JPEG Group) License | `README` |
| libpng 1.6.40 | `src/user/ports/libpng-1.6.40/` | PNG Reference Library License (libpng) | `LICENSE` |
| libwebp 1.3.2 | `src/user/ports/libwebp-1.3.2/` | BSD-3-Clause | `COPYING` |
| zlib 1.3.1 | `src/user/ports/zlib-1.3.1/` | zlib License | `LICENSE` |
| BearSSL | `src/user/apps/bearssl/` | MIT — © 2016 Thomas Pornin | `LICENSE.txt` |
| Mbed TLS | `src/user/apps/mbedtls/` | Apache-2.0 **OR** GPL-2.0-or-later — © The Mbed TLS Contributors | `LICENSE` |
| QuickJS | `src/user/apps/quickjs/` | MIT — © Fabrice Bellard, Charlie Gordon | `LICENSE` |
| OpenH264 | `src/user/apps/openh264/` | BSD-2-Clause — © 2013 Cisco Systems | `LICENSE` |
| Opus | `src/user/apps/opus/` | BSD-3-Clause — © Xiph.Org Foundation, Skype Limited, et al. | `COPYING` |
| Ladybird Rust vendor crates | `src/user/apps/ladybird-rust-vendor/vendor/` | Predominantly MIT **OR** Apache-2.0; also Unicode-3.0, MPL-2.0, Zlib, BSD-3-Clause, 0BSD, Unlicense | per-crate `LICENSE*` |

All bundled licenses above are OSI-approved / FSF-recognised free software
licenses and are compatible with redistribution of the combined work under the
GPL v3. Refer to each component's license file for the authoritative terms.
