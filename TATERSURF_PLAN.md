TaterSurf — Native Web Browser for TaterTOS64v3
=====================================================================
DATE: March 27, 2026
TARGET HW: Dell Precision 7530 (Intel i5-8300H, I219-LM Ethernet)
PROJECT DIR: /home/jjsako/TaterTOS64v3
PARENT: planv3.txt (all RULES apply without exception)
=====================================================================

RULES INHERITED FROM planv3.txt
=====================================================================
1) Follow the plan step by step. No shortcuts or cut corners.
2) No "minimal" implementations; do thorough, functional work.
3) No Linux or Windows code inside the OS.
4) Log every completed step in its own logs/fryN.txt with timestamps.
5) If you change code, include a brief "why" in the log.
6) Don't pause after saying you'll continue; just continue.
7) Branding is TaterTOS64v3. Browser name is TaterSurf.
8) All apps must follow the TaterWin protocol.
9) Hardware-agnostic. Dell 7530 is dev machine only.
10) Default rebuild: make clean + build_iso.sh


GOAL
=====================================================================
A working web browser that can load and interact with YouTube,
Facebook, and TikTok. Single .fry binary. TaterWin app.
HTTPS, JavaScript (ES2023), HTML/CSS rendering, video playback,
audio playback. No Firefox, no Chromium, no WebKit.


EXTERNAL CODE (all permissive licensed, zero Linux code)
=====================================================================
mbedTLS     Apache 2.0    TLS 1.2 + 1.3 stack
QuickJS     MIT           ES2023 JavaScript engine
OpenH264    BSD 2-Clause  H.264 software video decoder
libopus     BSD           Opus audio decoder
minimp4     MIT           Fragmented MP4 demuxer (single header)

All are portable C. All compile with -ffreestanding or equivalent.
All have their Linux/Windows-specific modules disabled or excluded.
No GPL code anywhere.


LIBC GAPS (must be filled before external code compiles)
=====================================================================
qsort           ~80 LOC    heapsort, needed by mbedTLS + QuickJS
setjmp/longjmp  ~50 LOC    x86_64 asm, needed by QuickJS dtoa.c
assert macro    ~5 LOC     header, needed by QuickJS + mbedTLS debug
%f/%g/%e        ~300 LOC   float formatting in vsnprintf, QuickJS needs it


PHASE A — FOUNDATIONS
=====================================================================
Libc gaps, TLS, HTTP client, URL parser.
After this phase: TaterTOS64v3 can fetch HTTPS pages.

A1. qsort implementation
    File: src/user/libc/libc.c (add to existing)
    Algorithm: heapsort (in-place, no malloc, O(n log n) worst case)
    Interface: void qsort(void *base, size_t nmemb, size_t size,
                          int (*compar)(const void *, const void *))
    Also add: void *bsearch(const void *key, const void *base,
                            size_t nmemb, size_t size,
                            int (*compar)(const void *, const void *))
    Log: fry761

A2. setjmp/longjmp
    File: src/user/libc/setjmp.S (new, x86_64 assembly)
    Header: src/include/setjmp.h (new)
    Save/restore: RBX, RBP, R12-R15, RSP, return address
    jmp_buf: array of 8 uint64_t values
    Log: fry762

A3. assert macro
    File: src/include/assert.h (new)
    Implementation: macro that calls __assert_fail(expr, file, line)
    __assert_fail prints message and calls fry_exit(127)
    Log: fry763

A4. vsnprintf float formatting (%f, %g, %e)
    File: src/user/libc/libc.c (modify existing vsnprintf)
    Support: %f (fixed), %e (scientific), %g (auto), with precision
    Also add: %o (octal), %n (store count, stubbed for security)
    Log: fry764

A5. mbedTLS integration
    Directory: src/user/apps/mbedtls/ (stripped source tree)
    Config: src/user/apps/mbedtls/mbedtls_config.h
    Disabled modules: NET_C, FS_IO, TIMING_C, ENTROPY_PLATFORM,
                      HAVE_TIME (or shim to fry_clock_gettime)
    Custom implementations:
      mbedtls_hardware_poll() → fry_getrandom()
      Send/recv callbacks → fry_send()/fry_recv()
    Enabled: TLS 1.2, TLS 1.3, AES-GCM, ChaCha20-Poly1305,
             ECDHE-RSA, ECDHE-ECDSA, X.509 cert parsing
    Certificate validation: accept all (no CA bundle yet)
    Status bar shows "TLS (unverified)" for HTTPS pages
    Log: fry765

A6. URL parser
    File: src/user/apps/ts_url.h (header-only)
    Parses: scheme, host, port, path, query, fragment
    Resolves: relative URLs against base URL
    Formats: ts_url back to string
    Log: fry766

A7. HTTP/1.1 client
    File: src/user/apps/ts_http.h (header-only)
    Features:
      HTTP and HTTPS (via mbedTLS)
      Chunked transfer decoding
      Content-Length body collection
      Redirect following (301/302/303/307/308, max 5)
      Cookie jar (in-memory, per-session)
      User-Agent: TaterSurf/1.0
      Gzip/deflate decompression (stripped zlib, ~2K LOC)
    Interface:
      ts_http_init, ts_http_get, ts_http_post, ts_http_poll,
      ts_http_follow_redirect, ts_http_free
    Log: fry767

A8. Rebuild + QEMU test: fetch https://example.com, print headers
    Log: fry768


PHASE B — THE VIEWER
=====================================================================
HTML tokenizer, CSS parser, box model layout, TaterWin browser app.
After this phase: text-based web browsing with links and styling.

B1. HTML tokenizer
    File: src/user/apps/ts_html.h (header-only)
    Zero-copy: tokens are pointer+length into source buffer
    Token types: TEXT, TAG_OPEN, TAG_CLOSE, TAG_SELF_CLOSE,
                 COMMENT, DOCTYPE, EOF
    Attribute extraction from tags
    Entity decoding (&amp; &lt; &gt; &quot; &#NNN; &#xHHH;)
    Script/style content isolation
    Log: fry769

B2. CSS parser
    File: src/user/apps/ts_css.h (header-only)
    Parses: <style> blocks and style="" attributes
    Selector matching: tag, .class, #id, tag.class, descendant
    Properties: color, background-color, background, margin (all),
      padding (all), border (all), width, height, max-width,
      display (block/inline/inline-block/none), font-size,
      font-weight, font-style, text-decoration, text-align,
      line-height, list-style-type, white-space, overflow,
      visibility, float (basic), position (static/relative)
    Color parsing: named colors (all 140), #hex, rgb(), rgba()
    Unit parsing: px, em, rem, %, auto
    Cascade: inline > #id > .class > tag
    Log: fry770

B3. Layout engine
    File: src/user/apps/ts_layout.h (header-only)
    Box tree: parent/child/sibling links
    Block layout: vertical flow, width inherits from parent
    Inline layout: horizontal flow with word-wrap
    Margin collapsing
    Text measurement: 8x16 font * scale factor
    Scaled text rendering (for headings): pixel-double the font
    Content height computation for scrolling
    Link hit-testing
    Log: fry771

B4. TaterSurf main application
    File: src/user/apps/tatersurf.c
    TaterWin app: 900x600 initial, resizable
    UI: toolbar (back, forward, reload, URL bar), viewport, status bar
    Navigation: type URL + Enter, click links, back/forward
    Scrolling: mouse wheel, Page Up/Down, Home/End, arrow keys
    Status: connection state, page title, bytes loaded
    History: 32-entry URL stack
    Pipeline: URL → HTTP fetch → tokenize → parse CSS → layout → render
    poll() loop: TaterWin input + socket I/O
    Log: fry772

B5. Makefile + build_iso.sh integration
    Add tatersurf target, copy TATERSURF.FRY to all app dirs
    gui.c app discovery picks it up automatically
    Log: fry773

B6. Rebuild + QEMU test: browse http://example.com and
    https://example.com, verify text renders, links work,
    back/forward works, scroll works
    Log: fry774


PHASE C — JAVASCRIPT
=====================================================================
QuickJS integration, DOM bridge, Web API shims.
After this phase: JavaScript-heavy SPA pages can load.

C1. QuickJS integration
    Directory: src/user/apps/quickjs/
    Files: quickjs.c, quickjs.h, cutils.c, cutils.h,
           libregexp.c, libregexp.h, libunicode.c, libunicode.h,
           libbf.c, libbf.h
    Compile flags: -DCONFIG_VERSION=\"2025-09-13\"
                   -Wno-sign-compare -Wno-unused-parameter
    Skip: quickjs-libc.c (OS-specific, not needed)
    Verify: compiles and runs "1+1" eval
    Log: fry775

C2. DOM bridge — Core tree
    File: src/user/apps/ts_dom.h (header-only)
    Node types: Element, Text, Comment, DocumentFragment
    Tree ops: appendChild, removeChild, insertBefore, replaceChild,
              cloneNode, contains
    Properties: parentNode, childNodes, firstChild, lastChild,
                nextSibling, previousSibling, nodeType, nodeName,
                textContent, ownerDocument
    Element: tagName, id, className, classList (add/remove/toggle/
             contains), setAttribute, getAttribute, removeAttribute,
             hasAttribute, innerHTML (triggers re-parse), style
    Document: createElement, createTextNode, createComment,
              createDocumentFragment, getElementById, querySelector,
              querySelectorAll, body, head, title
    Window: location (href, hostname, pathname, search, hash),
            innerWidth, innerHeight, scrollTo, scrollBy,
            navigator.userAgent
    QuickJS bindings: register all above as C functions on
    JS prototypes via JS_SetPropertyFunctionList
    Log: fry776

C3. DOM bridge — Events
    File: ts_dom.h (extend)
    Event system: bubbling + capturing phases
    addEventListener, removeEventListener, dispatchEvent
    Event types: Event, MouseEvent, KeyboardEvent, FocusEvent,
                 CustomEvent
    Properties: target, currentTarget, type, bubbles, cancelable,
                preventDefault, stopPropagation, defaultPrevented
    TaterWin key/mouse/wheel events → DOM events
    Log: fry777

C4. DOM bridge — CSS selector engine
    File: ts_dom.h (extend)
    querySelector / querySelectorAll implementation
    Selectors: tag, #id, .class, tag.class, tag#id,
               descendant (space), child (>), attribute ([attr],
               [attr=val]), :first-child, :last-child, :nth-child
    element.matches(selector), element.closest(selector)
    Log: fry778

C5. Web API shims
    File: src/user/apps/ts_webapi.h (header-only)
    Timer APIs: setTimeout, setInterval, clearTimeout, clearInterval,
                requestAnimationFrame (driven by poll loop)
    queueMicrotask (immediate job queue)
    fetch() → Promise wrapping ts_http internally
    Response, Headers, Request objects
    AbortController / AbortSignal
    URL / URLSearchParams
    TextEncoder / TextDecoder (UTF-8)
    console.log/warn/error → debug panel (F12 toggle)
    history.pushState, replaceState, popstate event
    localStorage / sessionStorage (in-memory Map, per-session)
    document.cookie (in-memory, per-session)
    getComputedStyle() → stub returning current style
    getBoundingClientRect() → return layout-computed box
    matchMedia() → stub, always returns false for queries
    IntersectionObserver → stub that fires callback immediately
    MutationObserver → real implementation (watch DOM mutations)
    ResizeObserver → stub, fire on window resize
    Log: fry779

C6. Web Components (YouTube requirement)
    File: src/user/apps/ts_webcomp.h (header-only)
    customElements.define(name, constructor)
    Custom element lifecycle: connectedCallback, disconnectedCallback,
                              attributeChangedCallback
    element.attachShadow({mode: 'open'})
    Shadow root: acts as separate DOM subtree
    <template> element: content property, cloneNode
    <slot> element: slot assignment, slotchange event
    CSS scoping within shadow roots (basic: :host selector)
    Log: fry780

C7. Iterative site testing
    Load youtube.com, capture errors, implement missing APIs
    Load facebook.com, capture errors, implement missing APIs
    Load tiktok.com, capture errors, implement missing APIs
    Each round gets its own fry log
    Logs: fry781+


PHASE D — AUDIO
=====================================================================
Intel HDA kernel driver, audio syscalls, Opus decoder.
After this phase: TaterTOS64v3 can play sound.

D1. Intel HDA kernel driver
    File: src/drivers/audio/hda.c (new)
    Header: src/drivers/audio/hda.h (new)
    PCI detection: class 0x0403 (audio controller)
    BAR0 MMIO mapping
    Controller reset (GCTL.CRST)
    CORB/RIRB setup: DMA ring buffers for codec commands
    Codec discovery: STATESTS register
    Widget tree enumeration: walk audio function group
    Output path: find DAC → Pin (headphone/speaker)
    Stream setup: allocate DMA buffer (BDL entries),
                  set format (48kHz, 16-bit, stereo),
                  connect DAC to stream
    Playback: RUN bit, fill DMA buffer with PCM
    Interrupt or polling for buffer position
    Log: fry7xx

D2. Audio syscalls
    File: src/kernel/proc/syscall.c (extend)
    New syscalls:
      SYS_AUDIO_OPEN    — open audio output stream
      SYS_AUDIO_WRITE   — write PCM samples to stream
      SYS_AUDIO_CLOSE   — close audio stream
      SYS_AUDIO_INFO    — get format info (rate, channels, bits)
    Userspace wrappers in libc:
      fry_audio_open, fry_audio_write, fry_audio_close, fry_audio_info
    Log: fry7xx

D3. libopus integration
    Directory: src/user/apps/opus/ (stripped decoder-only source)
    Build: compile as separate objects, link into tatersurf
    Configure: fixed-point build (FIXED_POINT define),
               no malloc mode (NONTHREADSAFE_PSEUDOSTACK)
    Verify: decode a test Opus frame to PCM
    Log: fry7xx

D4. Audio playback from TaterSurf
    Userspace audio pipeline:
      Opus frame → libopus decode → PCM buffer → fry_audio_write
    Threaded: audio decode runs on separate thread, feeds PCM
    Log: fry7xx

D5. Rebuild + test: play audio from an Opus stream
    Log: fry7xx


PHASE E — VIDEO
=====================================================================
DASH streaming, fMP4 demuxing, H.264 decode, display.
After this phase: TaterSurf can play video with audio.

E1. XML parser (for DASH MPD)
    File: src/user/apps/ts_xml.h (header-only)
    Minimal: parse element tree, attributes, text content
    No DTD validation, no namespaces, no entities beyond basic
    ~500 LOC
    Log: fry7xx

E2. DASH manifest parser
    File: src/user/apps/ts_dash.h (header-only)
    Parse MPD: Period → AdaptationSet → Representation
    Extract: mimeType, codecs, bandwidth, width, height
    Segment template: compute segment URLs from template
    Quality selection: pick representation by bandwidth
    Log: fry7xx

E3. fMP4 demuxer
    File: src/user/apps/minimp4.h (MIT, single header, as-is)
    Extract: H.264 NAL units from mdat boxes
    Parse: moov/moof/mdat box structure
    Extract: SPS/PPS from avcC box in init segment
    Extract: Opus/AAC frames from audio track
    Log: fry7xx

E4. OpenH264 integration
    Directory: src/user/apps/openh264/ (decoder source only)
    Build: compile decoder objects, link into tatersurf
    Interface: feed NAL units, get decoded YUV frames
    Profile: Constrained Baseline to High (what YouTube serves)
    Verify: decode a test H.264 stream to YUV
    Log: fry7xx

E5. YUV → RGB conversion + framebuffer blit
    File: src/user/apps/ts_video.h (header-only)
    BT.601 or BT.709 color matrix (match YouTube's encoding)
    Output: 32-bit BGRX pixels into SHM buffer
    SIMD: SSE2 intrinsics for the conversion hot path
    Scaling: nearest-neighbor downscale to fit viewport
    Log: fry7xx

E6. Framebuffer write-combining optimization
    File: src/kernel/mm/vmm.c (modify)
    Set framebuffer pages to Write-Combining via PAT
    This is a kernel change: modify page table entries for
    the framebuffer region to use WC cache attribute
    Expected speedup: 2-5x for large memcpy to framebuffer
    Log: fry7xx

E7. TaterWin video surface
    Double-buffered SHM for video region within browser window
    Video decode thread writes to back buffer
    Main thread flips and sends TW_MSG_DAMAGE_RECT for video area
    Avoids full-window re-render on every video frame
    Log: fry7xx

E8. A/V sync
    Audio clock is the master (HDA DMA position register)
    Video frames displayed when their PTS matches audio clock
    Drop frames if video falls behind
    Repeat frames if video is ahead
    Log: fry7xx

E9. Rebuild + test: play a DASH video stream with audio
    Log: fry7xx


PHASE F — THE BIG THREE
=====================================================================
End-to-end testing with YouTube, Facebook, TikTok.

F1. YouTube end-to-end
    Navigate to youtube.com
    Search for a video
    Click video
    Video plays with audio
    Log every issue found and fixed in its own fry log

F2. Facebook end-to-end
    Navigate to facebook.com
    Load news feed
    Scroll through posts
    Log every issue found and fixed

F3. TikTok end-to-end
    Navigate to tiktok.com
    Load For You feed
    Scroll through videos
    Log every issue found and fixed


TESTING
=====================================================================
QEMU command for network testing:
  qemu-system-x86_64 -m 4G -machine q35,accel=tcg -cpu max -smp 4 \
    -drive if=pflash,format=raw,readonly=on,\
      file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=OVMF_VARS_copy.fd \
    -cdrom out/tatertos64v3.iso \
    -nic user,model=e1000 \
    -serial file:logs/serial.log \
    -device intel-hda -device hda-output

Note: -nic user,model=e1000 provides NAT networking via QEMU's
user-mode stack. The e1000 device is supported by our e1000 driver
(currently a stub — may need to be implemented, or use i219 with
a different QEMU NIC model). Verify which NIC model QEMU exposes
that matches our working drivers.

For bare-metal testing on Dell Precision 7530:
  Wired Ethernet via I219-LM (working driver)
  Boot from USB with tatertos64v3.iso
