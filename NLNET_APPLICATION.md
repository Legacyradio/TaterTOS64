# NLnet NGI Zero Commons Fund — Application Draft
## TaterTOS64v3: A Ground-Up Open-Source Operating System for Digital Sovereignty
## Applicant: Zackery Sayers / TaterLabs
## Date: April 11, 2026

---

## Contact Information

- **Name:** Zackery Sayers
- **Organisation:** TaterLabs — Legacy Artist Productions LLC
- **Country:** United States
- **Website:** https://github.com/legacyindiesubmissions-ai

---

## Proposal Name

TaterTOS64v3 — A Ground-Up Open-Source Operating System for Digital Sovereignty

---

## Thematic Call

NGI Zero Commons Fund (13th call, deadline June 1, 2026)

---

## Abstract

TaterTOS64v3 is a fully independent 64-bit operating system written entirely from scratch. Every subsystem — kernel, device drivers, TCP/IP networking stack, TLS 1.3 implementation, DNS/DHCP, a web browser with a JavaScript engine, a custom extent-based filesystem (ToTFS), a GUI compositor with structured IPC, and userspace libraries — is original code. No line of Linux or Windows source exists in the system outside of a single WiFi firmware interface required by Intel's closed specification.

The system boots on real hardware (Dell Precision 7530, Intel Core i5-8300H) and in QEMU. It implements subsystems rarely found in independent OS projects: a full ACPI 6.2 AML bytecode interpreter that executes firmware control methods, an Intel WiFi 802.11ac driver with WPA2 authentication, an NVMe storage driver with Intel VMD (Volume Management Device) support, and a web browser (TaterSurf) capable of HTML5 parsing, CSS layout (including flexbox and grid), JavaScript execution via QuickJS, TLS-secured HTTP, TrueType font rendering, H.264 video decoding, and Opus audio playback.

The project demonstrates that sovereign computing infrastructure — free from inherited architectural assumptions, inherited vulnerabilities, and inherited licensing constraints of existing platforms — is achievable by independent developers on consumer-grade hardware costing $140.

This proposal requests funding to complete five development milestones over 12 months: (1) harden the ToTFS custom filesystem and build an NVMe installer for persistent installations, (2) complete the TCP/IP state machine with proper connection teardown, congestion control, and window scaling, (3) add form input, HTTPS POST, and cookie persistence to TaterSurf to enable authentication flows, (4) extend the SMP scheduler for multi-core load balancing, and (5) produce documentation and community onboarding materials to make the project accessible to other developers and researchers.

All code is released under the GNU General Public License v3.0.

---

## Prior Involvement

The applicant has built and shipped multiple open-source software systems:

- **TaterTOS64v3** (2024-present): The subject of this proposal. Three major versions, currently in active development. Boots on bare metal, runs networked applications.

- **Legacy Indie Radio** (2025-present): A complete AI-powered internet radio automation platform running 24/7 on the same $140 hardware. Demonstrates full-stack systems integration: audio engine, MP3 streaming server, AI DJ (bundled LLM), voice cloning, music analysis, web dashboard, Android listener app. Open source components released separately.

- **Embodied LLM Research** (2026): Published research integrating Claude Opus into the FlyWire Drosophila connectome simulation (138,639 neurons). 6,110+ inference cycles demonstrating emergent motor behavior in LLMs. Paper published on Zenodo (DOI: 10.5281/zenodo.19500055), code on GitHub (MIT license).

All work is conducted independently without institutional affiliation on a $140 refurbished Dell Precision 7530.

---

## Requested Amount

**EUR 35,000** (twelve months)

---

## Budget Breakdown

| Milestone | Description | Duration | Amount |
|-----------|-------------|----------|--------|
| M1 | ToTFS filesystem hardening + NVMe installer | 2 months | EUR 7,000 |
| M2 | TCP/IP stack completion + network security | 2 months | EUR 7,000 |
| M3 | TaterSurf form input + HTTPS POST + auth flows | 3 months | EUR 9,000 |
| M4 | SMP scheduler + multi-core stability | 2 months | EUR 5,000 |
| M5 | Documentation, packaging, community onboarding | 2 months | EUR 5,000 |
| HW | Hardware upgrade (GPU for development/testing) | — | EUR 2,000 |
| **Total** | | **12 months** | **EUR 35,000** |

### Milestone Details

**M1 — ToTFS Filesystem Hardening + Installer (months 1-2)**
ToTFS is TaterTOS64v3's custom extent-based filesystem (4KB blocks, 256-byte inodes, up to 10 extents per file). The kernel can currently mount and read/write ToTFS partitions, but the filesystem lacks journaling or copy-on-write protection against unexpected power loss, and no installer exists to write the OS to NVMe from a live environment. This milestone delivers: journal or COW layer for crash consistency, GPT partition creation from userspace, and a graphical installer that writes kernel + ramdisk + user applications to persistent NVMe storage.

**M2 — TCP/IP Stack Completion (months 3-4)**
The current networking stack (netcore.c) handles ESTABLISHED TCP connections, DHCP, DNS, ARP, and ICMP. Missing: proper FIN/RST connection teardown (connections currently leak), TCP congestion control (Reno or CUBIC), TCP window scaling for high-bandwidth transfers, and IPv6 foundation. This milestone delivers a standards-compliant TCP implementation suitable for sustained real-world use, verified against Linux and BSD endpoints.

**M3 — TaterSurf Authentication Flows (months 5-7)**
TaterSurf currently renders web pages with full HTML/CSS/JS but cannot log into any service. Missing: HTML form elements (input, textarea, select), keyboard input routing to focused elements, HTTPS POST method, cookie persistence across requests, and basic iframe support for OAuth redirects. This milestone delivers the ability to log into standard web services (email, search engines, developer platforms) from TaterSurf, making the browser functional for daily use rather than read-only browsing.

**M4 — SMP Scheduler (months 8-9)**
The kernel currently boots all cores via SIPI but schedules processes on the BSP only. This milestone delivers: per-core run queues, load balancing across available cores, proper inter-processor interrupt (IPI) handling for cross-core scheduling events, and stability testing under concurrent workloads.

**M5 — Documentation + Community (months 10-11)**
Produce: architecture documentation covering all kernel subsystems, a developer guide for writing TaterTOS64v3 applications (.fry format, syscall API, TaterWin IPC protocol), build instructions for cross-compilation, and a contribution guide. Publish to the project website and engage with the OSDev.org community for feedback and adoption.

---

## Budget Usage

The requested EUR 35,000 covers 12 months of full-time independent development (approximately EUR 2,750/month after the EUR 2,000 hardware allocation). The rate reflects the applicant's situation as an independent developer in the United States without institutional salary or overhead.

The EUR 2,000 hardware allocation is for an RTX 3090 GPU to accelerate development iteration (faster QEMU emulation, CUDA-accelerated testing tools) and to support continued parallel work on related open-source projects.

---

## Other Funding Sources

- **Emergent Ventures (Mercatus Center):** Grant application submitted April 10, 2026 for a separate project (embodied LLM research). No overlap with this proposal.
- No other current or past funding for TaterTOS64v3 development.

---

## Comparison to Existing Efforts

**Linux / BSD:** Millions of lines of code accumulated over 30+ years. Extraordinary capability, but also extraordinary inherited complexity — legacy BIOS support, 32-bit compatibility layers, decades of backwards-compatible API surface. TaterTOS64v3 is clean-sheet: 64-bit only, UEFI only, no legacy BIOS, no POSIX compatibility layer. Every architectural decision reflects current hardware realities rather than historical constraints.

**Hobby/educational OSes (xv6, MINIX, intermezzOS):** Valuable for teaching but intentionally limited in scope. Most implement a basic kernel with serial output. None include WiFi drivers, ACPI AML interpretation, NVMe storage, or a usable web browser. TaterTOS64v3 implements all of these on real hardware.

**SerenityOS:** The closest comparable project — a ground-up OS with GUI and browser (Ladybird). SerenityOS targets a POSIX-compatible Unix-like environment and is built by a large community. TaterTOS64v3 takes a different approach: no POSIX layer, custom IPC protocol (TaterWin), custom binary format (.fry with CRC integrity checking), and a custom filesystem (ToTFS). SerenityOS also does not implement WiFi or target laptop hardware with ACPI power management.

**Research OSes (Fuchsia, Managarm, Redox):** Backed by corporate teams (Google) or larger developer communities. TaterTOS64v3 is built by a single developer on a $140 machine — demonstrating that the barrier to OS development is knowledge, not resources.

**Key differentiators:**
1. Full ACPI 6.2 AML bytecode interpreter — executes firmware control methods for power management, thermal monitoring, and hardware discovery. Most independent OSes stop at table lookup.
2. Intel WiFi 802.11ac driver with firmware loading and WPA2 — WiFi is almost universally absent from hobby OS projects.
3. Complete web browser with JavaScript, TLS, video/audio decoding — built on the OS's own TCP/IP stack and rendering pipeline, not ported from an existing browser engine.
4. Runs on a specific real laptop (Dell Precision 7530) with full hardware support including NVMe, HD Audio, USB 3.0 (XHCI), and battery/thermal monitoring via the Embedded Controller.

---

## Technical Challenges

**1. TCP Reliability at Scale**
The current TCP implementation handles basic ESTABLISHED-state communication but lacks proper connection teardown (FIN/RST), congestion control, and window scaling. Real-world web browsing generates dozens of concurrent connections with varying latency characteristics. Implementing RFC-compliant TCP that performs well against Linux/BSD endpoints — without inheriting their code — requires careful attention to retransmission timing, fast retransmit, and selective acknowledgment.

**2. Filesystem Crash Consistency**
ToTFS currently has no protection against incomplete writes during power loss. Adding journaling (write-ahead log) or copy-on-write semantics to an extent-based filesystem without the abstractions that Linux's VFS provides requires designing the consistency mechanism from first principles. The constraint is maintaining ToTFS's simplicity (the entire driver is ~800 lines) while adding durability guarantees.

**3. Browser Form Input System**
Implementing HTML form elements requires keyboard event routing, text cursor management, selection/clipboard, focus/blur semantics, and form serialization for POST submission. Each of these interacts with the DOM, the layout engine, and the TaterWin IPC protocol. The challenge is integrating these without introducing the complexity that makes existing browser engines (Blink, Gecko) millions of lines of code.

**4. Multi-Core Scheduling**
Moving from single-core to multi-core scheduling introduces shared-state concurrency across the kernel. The current spinlock primitives are sufficient for interrupt protection but per-core run queues, cross-core migration, and IPI-based preemption require careful lock ordering to avoid deadlocks in interrupt context.

---

## Ecosystem

**Standards compliance:** TaterTOS64v3 implements or targets: UEFI 2.7 (boot), ACPI 6.2 (hardware abstraction), NVMe 1.2 (storage), 802.11ac (WiFi), TCP/IP (RFC 793, 5681, 7323), TLS 1.3 (RFC 8446), HTTP/1.1 (RFC 7230-7235), HTML5, CSS3, ECMAScript 2023.

**Community engagement:**
- OSDev.org — the primary community for independent OS developers. TaterTOS64v3's ACPI AML interpreter, WiFi driver, and browser would be valuable reference implementations for the community.
- GitHub — all source code published under GPL v3 for collaboration and review.
- Educational institutions — a complete, readable OS that boots on real hardware has value as a teaching resource for systems programming courses.

**Downstream potential:**
- The TCP/IP stack, TLS implementation, and ToTFS filesystem are modular enough to be extracted and reused in other bare-metal or embedded projects.
- TaterSurf's rendering pipeline demonstrates that a usable browser can exist outside the Chromium/Firefox duopoly — relevant to browser diversity efforts.
- The ACPI AML interpreter is a standalone subsystem that could benefit any OS project targeting real Intel hardware.

---

## European Dimension

The European Union's digital sovereignty agenda — expressed through GAIA-X, the EU Chips Act, the Cyber Resilience Act, and the push for open-source alternatives in public administration — rests on the premise that Europe should not depend on technology stacks controlled by non-European corporations.

TaterTOS64v3 provides an architectural proof of concept for this premise. It demonstrates that a complete, functional operating system — from UEFI boot to web browsing — can be built from scratch by a single developer without inheriting code, architectural assumptions, or supply chain dependencies from existing platforms. Every component is auditable, every design decision is documented, and the entire system is released under GPL v3.

The modular subsystems produced by this project (TCP/IP stack, filesystem, ACPI interpreter, browser rendering engine) are directly usable as open-source building blocks for European sovereign computing initiatives. They carry no licensing baggage from existing codebases and no inherited architectural constraints.

---

## AI Disclosure

This proposal was drafted with assistance from Claude (Anthropic, model: claude-opus-4-6). The applicant provided all technical details, architectural decisions, and project direction. Claude assisted with structuring the proposal text, researching NLnet requirements, and formatting. The applicant reviewed, revised, and approved all content before submission.

---

## Attachments (to include with submission)

1. TaterTOS64v3 source repository (GitHub link)
2. Screenshots: GUI desktop, TaterSurf browser rendering web pages, shell, system information
3. QEMU boot recording (if available)
4. CODEX_BRIEFING.md — current architecture overview
5. Build log excerpts demonstrating active development (fry400+ session logs)
