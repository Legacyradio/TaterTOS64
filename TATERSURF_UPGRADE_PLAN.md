TaterSurf Browser Upgrade Plan — From Basic to Professional
=====================================================================
DATE: 2026-04-03
STATUS: Planning complete, ready to execute step by step

Each step is an independent, testable upgrade. Steps are ordered by
dependency (later steps need earlier ones) and impact (biggest
site-fix-per-change first).


STEP 1 — Unknown Tag Passthrough
=====================================================================
FILE: ts_layout.h  (ts_doc_build function, ~line 498-1171)

PROBLEM: The tag matching in ts_doc_build is a hardcoded if/else chain
for ~30 known HTML tags. ANY tag not in that list (ytd-app, figure,
details, summary, video, audio, time, mark, abbr, fieldset, legend,
label, dl, dt, dd, address, dialog, picture, source, template, slot,
plus every web component and framework tag) is silently dropped along
with ALL content inside it.

That means if YouTube sends:
  <ytd-app><div><h2>Trending</h2></div></ytd-app>
the <ytd-app> is unknown → entire subtree dropped → 0 nodes.

FIX: Add a default fallback at the end of the open-tag if/else chain.
Unknown tags get treated as generic containers:
  - If the tag name is in ts_html_is_block() → emit BREAK + push style
  - Otherwise → push style only (inline container)
  - Apply CSS normally via ts_doc__apply_element_css
  - Children render inside the pushed style context

Add matching fallback in the close-tag handler: pop style, emit BREAK
if it was block-level.

Also: stop skipping <noscript> content. Since our JS engine can't run
most real JS anyway, the noscript fallback content is often MORE useful
than the JS-dependent content.

IMPACT: Massive. Every site with custom/unknown elements goes from
blank to showing content. This is the single highest-ROI fix.

RISK: Low — purely additive, doesn't change behavior of known tags.

TEST: Load a page with <figure><figcaption>Text</figcaption></figure>
and verify "Text" renders. Load YouTube and see if footer text appears.


STEP 2 — DOM→Render Tree Sync
=====================================================================
FILES: tatersurf.c (~line 1962-1965), ts_layout.h

PROBLEM: TaterSurf maintains TWO separate trees:
  g_doc (struct ts_document) — render tree, painted to screen
  g_dom (struct ts_dom_ctx) — JavaScript DOM tree

When JS modifies g_dom (innerHTML, appendChild, createElement), it
sets g_dom->dirty = 1. But tatersurf.c just clears the flag:

  if (g_dom->dirty) {
      g_dom->dirty = 0;
      needs_redraw = 1;  // repaints OLD g_doc nodes!
  }

Nothing rebuilds g_doc from the modified g_dom.

FIX: Add ts_doc_build_from_dom() — a new function in ts_layout.h that
walks the DOM tree (g_dom) and emits render nodes into g_doc, similar
to how ts_doc_build walks HTML tokens. This is more efficient than
serializing DOM→HTML→re-parse.

In tatersurf.c, when g_dom->dirty:
  1. ts_doc_init(g_doc)  — clear old render nodes
  2. ts_doc_build_from_dom(g_doc, g_dom)  — walk DOM, emit nodes
  3. ts_doc_layout(g_doc, viewport_w())  — re-layout
  4. needs_redraw = 1

IMPACT: High. JavaScript-created content actually renders. Required
for any page that uses JS to build its UI.

RISK: Medium. Need careful tree walking. CSS matching needs to work
against DOM nodes (which have tag/class/id) instead of tokens.

TEST: Create a test page: <div id="target"></div> + script that does
document.getElementById("target").innerHTML = "<h1>Hello</h1>".
Verify "Hello" appears.


STEP 3 — CSS Selector Engine Fix
=====================================================================
FILE: ts_css.h, ts_layout.h

PROBLEM: ts_doc__match_apply_css() only matches the LAST part of
compound selectors (line 438-441):

  struct ts_css_selector_part *part = &sel->parts[sel->part_count - 1];
  if (ts_css_match_part(part, tag_name, cls, id_attr)) {
      // apply!
  }

So for selector "div .content > p.intro", it only checks if the
current element is "p.intro" — it NEVER verifies that there's a
.content ancestor or div grandancestor. This means:
  - Rules match too broadly (wrong elements get styled)
  - Descendant/child combinators are completely ignored
  - display:none from ancestor rules may not propagate correctly

FIX:
  a) Pass an ancestor chain to the CSS matcher (array of tag/class/id
     for each parent up to <html>)
  b) Walk selector parts right-to-left, matching each part against
     ancestors:
     - ' ' (descendant): search up the chain for a match
     - '>' (child): check immediate parent
  c) Add '+' (adjacent sibling) and '~' (general sibling) combinators
  d) Implement attribute selectors: [attr], [attr=val], [attr~=val],
     [attr^=val], [attr$=val], [attr*=val]
  e) Track !important priority — currently stripped and ignored

ALSO FIX: The class matcher only handles single .class in selectors.
Need to support .class1.class2 (multiple classes on one part).

IMPACT: High. CSS rules apply correctly, fixing layout and visibility
on virtually every real site. Many sites use descendant selectors for
their core styling.

RISK: Medium. Ancestor chain must be maintained during document build.
Need to add a parent context parameter to ts_doc__match_apply_css.

TEST: CSS rule "div.container > p { color: red }" only applies to <p>
directly inside <div class="container">, not to any random <p>.


STEP 4 — Box Model
=====================================================================
FILE: ts_layout.h

PROBLEM: The current layout is a simple x/y cursor flow. There is NO:
  - margin (space outside element)
  - padding (space inside element, around content)
  - border (drawn edge of element)
  - width/height (explicit element sizing)
  - display: block vs inline vs none computation
  - content vs border-box sizing

Everything is treated as inline text runs that wrap at viewport edge.
Block elements just emit a BREAK node (line break).

FIX: Refactor layout into a proper tree-based layout engine:

  a) Change the node model from flat array to a tree:
     - Each element has parent, first_child, next_sibling
     - Each element has: margin[4], padding[4], border_width[4],
       content_width, content_height, computed_x, computed_y
     - Block elements: width = parent width - margins; height = sum
       of children
     - Inline elements: width = text content width; flow left-to-right
       within parent's content area

  b) Layout algorithm (two-pass):
     Pass 1 (top-down): resolve widths
       - Block elements: width = parent content width - margin-left - margin-right
       - Explicit width: override if set
       - min-width, max-width constraints
     Pass 2 (bottom-up): resolve heights
       - Block: height = sum of children heights + gaps
       - Explicit height: override if set
       - Margin collapse: adjacent vertical margins merge (take larger)

  c) Painting: render backgrounds, borders, then content per element

NOTE: This is the biggest single refactor. It changes the fundamental
data model from flat render nodes to a layout tree. All downstream
features (flexbox, grid) need this.

IMPACT: Transformative. Pages go from "text dump" to "actual web page
layout" with proper spacing, widths, and visual hierarchy.

RISK: High. Major refactor. Must be done carefully to not break
existing simple-page rendering.

TEST: A <div> with style="width:300px; padding:20px; margin:10px;
border:2px solid red" renders as a 300px-wide box with visible red
border, 20px inner spacing, 10px outer spacing.


STEP 5 — Flexbox
=====================================================================
FILE: ts_layout.h

PROBLEM: No flexbox. Most modern sites use display:flex for layout.

FIX: When a box has display:flex, use the flex layout algorithm:

  a) Determine main axis (flex-direction: row | column)
  b) Compute child sizes:
     - flex-basis (initial size along main axis)
     - flex-grow (how to distribute remaining space)
     - flex-shrink (how to shrink when overflowing)
  c) Position children:
     - justify-content: flex-start|center|flex-end|space-between|space-around
     - align-items: stretch|flex-start|center|flex-end
     - align-self per child
  d) Handle flex-wrap: wrap | nowrap
  e) Handle gap property

REQUIRES: Step 4 (box model) — flex items need computed widths/heights.

IMPACT: Huge. Flexbox alone enables correct layout for ~80% of modern
sites including navbars, card grids, sidebars, media objects.

RISK: High. Flex layout is complex but well-specified (CSS Flexbox
Level 1 spec). The algorithm has specific steps for resolving flexible
lengths.

TEST: A flex container with 3 children and justify-content:space-between
positions children at left, center, right edges.


STEP 6 — CSS Grid
=====================================================================
FILE: ts_layout.h

PROBLEM: No CSS Grid.

FIX: Implement CSS Grid Level 1:
  - grid-template-columns / grid-template-rows (explicit tracks)
  - grid-column / grid-row (item placement)
  - gap / grid-gap
  - Auto placement algorithm
  - fr unit (fractional)
  - repeat() and minmax()

REQUIRES: Step 4 (box model)

IMPACT: Significant for dashboard-style layouts, image galleries,
complex multi-column designs.

RISK: Very high. Grid is the most complex CSS layout algorithm.
Can be deferred if flexbox covers enough sites.

TEST: A 3-column grid (grid-template-columns: 1fr 1fr 1fr) positions
3 children in equal-width columns.


STEP 7 — Position + Float
=====================================================================
FILE: ts_layout.h

PROBLEM: No positioning or floats.

FIX:
  a) position: relative — offset from normal flow position
  b) position: absolute — positioned relative to nearest positioned ancestor
  c) position: fixed — positioned relative to viewport
  d) position: sticky — switch between relative and fixed at scroll threshold
  e) z-index stacking order
  f) float: left | right — text wraps around floated elements
  g) clear: left | right | both

REQUIRES: Step 4 (box model)

IMPACT: Medium-high. Many sites use position:absolute for dropdowns,
modals, tooltips. Floats are still used for text-wrapping images.

TEST: A position:absolute div with top:10px; right:10px renders in
the top-right corner of its positioned parent.


STEP 8 — Overflow + Scrolling
=====================================================================
FILE: ts_layout.h

PROBLEM: No per-element overflow control. Content just extends past
its parent bounds.

FIX:
  - overflow: visible | hidden | scroll | auto
  - overflow-x / overflow-y
  - Clip rendering to element bounds when overflow:hidden
  - Scroll containers with scrollbar rendering
  - scroll event dispatching

REQUIRES: Step 4 (box model)

IMPACT: Medium. Needed for any page with scrollable regions (code
blocks, chat windows, dropdown menus).


STEP 9 — DOM API Completions
=====================================================================
FILES: ts_dom.h, ts_dom_bindings.h

PROBLEM: Limited DOM API surface. Many JS operations fail silently.

FIX: Add missing DOM methods:
  - element.classList: add(), remove(), toggle(), contains(), item()
  - element.dataset (data-* attributes)
  - element.style property access (element.style.color = "red")
  - element.children (element children only, skip text nodes)
  - element.childNodes (all children including text)
  - element.parentElement
  - element.closest(selector)
  - element.matches(selector)
  - element.cloneNode(deep)
  - element.remove() (self-removal)
  - element.replaceChild()
  - element.replaceWith()
  - element.before() / element.after()
  - element.append() / element.prepend()
  - element.outerHTML
  - element.getBoundingClientRect() (from layout data)
  - element.scrollIntoView()
  - element.focus() / element.blur()
  - document.createDocumentFragment()
  - document.createTextNode()
  - document.createComment()
  - document.readyState / DOMContentLoaded event
  - window.getComputedStyle()

IMPACT: High. More JS code runs without errors.

RISK: Medium. Many small additions, each fairly straightforward.


STEP 10 — Web APIs
=====================================================================
FILE: ts_dom_bindings.h, possibly new ts_fetch.h

PROBLEM: No browser-standard web APIs beyond basic DOM.

FIX:
  - fetch() API — wraps ts_http for JS-accessible HTTP requests
    Returns Promise with .json(), .text(), .blob() methods
  - XMLHttpRequest — legacy but still widely used
  - localStorage / sessionStorage — file-backed key/value store
  - URL / URLSearchParams
  - FormData
  - TextEncoder / TextDecoder
  - DOMParser / XMLSerializer
  - atob() / btoa() — base64
  - encodeURIComponent / decodeURIComponent (QuickJS has these)
  - performance.now()
  - history.pushState / popState
  - navigator object (userAgent, language, etc.)
  - ResizeObserver / MutationObserver / IntersectionObserver

IMPACT: High. fetch() alone enables most modern API-driven sites.


STEP 11 — Custom Elements v1
=====================================================================
FILES: ts_dom.h, ts_dom_bindings.h

PROBLEM: Custom elements (<yt-app>, <my-component>) have no
lifecycle support. They're just empty unknown tags.

FIX:
  - customElements.define(name, class, options)
  - Element upgrade: when define() is called, find and upgrade all
    existing instances
  - Lifecycle callbacks:
    connectedCallback — called when element inserted into DOM
    disconnectedCallback — called when removed
    adoptedCallback — called when moved to new document
    attributeChangedCallback — called when observed attr changes
  - observedAttributes static getter
  - :defined / :not(:defined) pseudo-classes

REQUIRES: Step 9 (DOM completions for classList, etc.)

IMPACT: High for modern frameworks. YouTube uses yt-* custom elements
extensively. Lit, Stencil, and many framework-less sites use them.

RISK: High. Lifecycle timing must be correct per spec.


STEP 12 — Shadow DOM
=====================================================================
FILES: ts_dom.h, ts_dom_bindings.h, ts_layout.h

PROBLEM: No shadow DOM support. Components can't encapsulate styles.

FIX:
  - element.attachShadow({ mode: 'open' | 'closed' })
  - Shadow root as document fragment
  - <slot> and named slots for content projection
  - Style encapsulation (shadow styles don't leak, host styles
    don't penetrate except via CSS custom properties)
  - :host, :host(), ::slotted() selectors

REQUIRES: Steps 9, 11 (DOM completions, custom elements)

IMPACT: Medium-high. Web components use shadow DOM for encapsulation.

RISK: Very high. Shadow DOM affects CSS matching, event bubbling,
and rendering. Significant architectural complexity.


STEP 13 — CSS Transforms, Transitions, Animations
=====================================================================
FILE: ts_layout.h, new ts_animation.h

PROBLEM: No visual effects.

FIX:
  - transform: translate, scale, rotate, skew, matrix
  - transition: property duration timing-function delay
  - animation / @keyframes
  - opacity
  - transform-origin

IMPACT: Medium. Many sites use transitions for hover effects,
animations for loading spinners, transforms for layout tricks.

RISK: Medium. Transforms need compositing support. Animations
need a timeline manager.


STEP 14 — Canvas 2D API
=====================================================================
FILES: ts_dom_bindings.h, new ts_canvas.h

PROBLEM: No <canvas> element support.

FIX:
  - <canvas> element with width/height
  - getContext('2d') returns CanvasRenderingContext2D
  - Drawing: fillRect, strokeRect, clearRect, fillText, strokeText
  - Paths: beginPath, moveTo, lineTo, arc, quadraticCurveTo, bezierCurveTo, closePath, fill, stroke
  - Images: drawImage (from img elements or other canvases)
  - Gradients: createLinearGradient, createRadialGradient
  - Patterns: createPattern
  - State: save, restore, translate, scale, rotate
  - Pixel manipulation: getImageData, putImageData, createImageData

Canvas renders to a pixel buffer that gets blitted into the render tree.

IMPACT: Medium-high. Many sites use canvas for charts, graphs, games,
visual effects.


STEP 15 — SVG Rendering
=====================================================================
FILES: ts_layout.h, new ts_svg.h

PROBLEM: No SVG support. Many sites use inline SVG for icons.

FIX:
  - Parse SVG elements within HTML
  - Basic shapes: rect, circle, ellipse, line, polyline, polygon
  - path element with d attribute (M, L, C, Z commands)
  - fill, stroke, stroke-width attributes
  - viewBox and preserveAspectRatio
  - text element
  - g (group) element
  - use element (with xlink:href)
  - Render SVG to pixel buffer, blit into render tree

IMPACT: Medium. SVG icons are everywhere on modern sites.


STEP 16 — Web Fonts
=====================================================================
FILES: ts_layout.h, new ts_font.h

PROBLEM: Only 8x16 bitmap font. No @font-face, no WOFF2/TTF.

FIX:
  - @font-face rule parsing
  - WOFF2 decompression (or just TTF)
  - TrueType/OpenType glyph rasterization (stb_truetype.h is ~4000
    lines, self-contained, public domain)
  - Font metrics: ascent, descent, line-height, kerning
  - font-family fallback chain
  - Multiple font sizes (currently faked via font_scale)

IMPACT: Huge for visual quality. Every site specifies custom fonts.
Without this, all text looks the same.

RISK: High. Font rendering is complex. stb_truetype helps enormously.


STEP 17 — HTTP/2
=====================================================================
FILE: ts_http.h

PROBLEM: HTTP/1.1 only. Many sites require HTTP/2. Single connection
per request.

FIX:
  - ALPN negotiation during TLS handshake (h2)
  - HTTP/2 frame parsing (DATA, HEADERS, SETTINGS, etc.)
  - HPACK header compression
  - Multiplexed streams on single connection
  - Server push handling

IMPACT: Medium-high. Performance improvement. Some sites may require
h2 (return 505 or degrade badly on HTTP/1.1).

RISK: High. HTTP/2 is a binary protocol with complex state management.


=====================================================================
EXECUTION ORDER
=====================================================================

Phase A — Content Visibility (Steps 1-2):
  Make existing content appear. Highest ROI, lowest risk.
  After this: most sites show SOMETHING instead of blank.

Phase B — Correct Styling (Step 3):
  CSS rules apply to the right elements.
  After this: sites look closer to correct.

Phase C — Proper Layout (Steps 4-5):
  Box model + flexbox = modern layout.
  After this: most sites actually look RIGHT.

Phase D — JavaScript Platform (Steps 9-11):
  DOM APIs + Web APIs + custom elements.
  After this: interactive sites work.

Phase E — Visual Quality (Steps 13, 15, 16):
  Animations, SVG icons, custom fonts.
  After this: sites look PROFESSIONAL.

Phase F — Advanced (Steps 6, 7, 8, 12, 14, 17):
  Grid, positioning, overflow, shadow DOM, canvas, HTTP/2.
  After this: full browser.
