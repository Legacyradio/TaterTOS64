# TaterTOS64v3

**TaterTOS64v3** is a complete, free and open-source 64-bit operating system,
written entirely from scratch. The kernel, device drivers, networking and TLS
stack, filesystems, GUI, and web browser are all original code — with no Linux
or Windows source in the OS itself.

It is built for digital sovereignty: a computing platform you fully own and
control. It boots via UEFI on real hardware (Dell Precision 7530) and in QEMU.

## The operating system

Every subsystem is original:

- **Kernel** — x86-64 long mode, paging/VMM, physical memory manager, preemptive
  SMP scheduler, ACPI with an AML bytecode interpreter, IRQ/APIC, HPET, and the
  `.fry` (userspace) and `.tot` (module) binary formats.
- **Device drivers** — NVMe, Intel WiFi 802.11ac, HD Audio, USB 3.0/xHCI, PS/2
  keyboard/mouse, e1000/RTL8169 Ethernet, framebuffer/GOP.
- **Networking** — an original TCP/IP stack with DHCP/DNS and **TLS 1.3**.
- **Filesystems** — a VFS layer with the custom extent-based **ToTFS**, plus
  FAT32, mounted from an EFI-built ramdisk and/or NVMe.
- **Graphics & UI** — a GUI compositor with the **TaterWin** IPC protocol.
- **Web** — **TaterSurf**, an original web browser with HTML5/CSS3, a JavaScript
  engine, and image/video/audio support.
- **Userspace** — a custom `libc` and system utilities.

## Linux compatibility (optional subsystem)

TaterTOS additionally includes a Linux compatibility layer — a syscall
translation subsystem that can host unmodified Linux x86-64 binaries, without
TaterTOS becoming Linux (the same approach FreeBSD takes with its linuxulator).
It currently runs dynamically-linked glibc programs with real threads, futexes,
and epoll-based async I/O. This is an optional capability layered on top of the
OS; the operating system stands entirely on its own.

## Build & run

Requires an `x86_64-elf` cross toolchain (`tools/host/`), `mtools`, `xorriso`,
and the `edk2`/OVMF firmware images.

```sh
# Build the bootable ISO (does a clean build)
. tools/host/env.sh
PATH="/opt/cross/bin:$PATH" /bin/bash build_iso.sh

# Boot in QEMU (UEFI)
cp /usr/share/edk2/x64/OVMF_VARS.4m.fd /tmp/vars.fd
qemu-system-x86_64 -machine q35 -m 4G \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=/tmp/vars.fd \
  -cdrom out/tatertos64v3.iso
```

## Licensing

TaterTOS64v3 is free software, released under the **GNU General Public License
v3.0**. You may use, modify, and redistribute it under the terms of the GPL v3.
See [LICENSE](LICENSE).

Bundled third-party libraries retain their own upstream licenses (all
GPL-v3-compatible free software). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
for the full inventory.

## Contributing & security

- See [CONTRIBUTING.md](CONTRIBUTING.md) before opening issues or PRs.
- See [SECURITY.md](SECURITY.md) to report a vulnerability.
- See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community expectations.

## Support

If you'd like to support development, see [funding.json](funding.json)
(Ko-fi / Patreon).

## Contact

Zackery Sayers — TaterLabs
