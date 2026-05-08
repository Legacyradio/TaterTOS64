# LibWeb & LibJS Integration Plan (Phase 5)

## Objective
Integrate a minimal HTML parser and DOM tree into the TaterTOS64v3 Ladybird port, enabling basic HTML-to-Gfx rendering.

## Challenges
1.  **Massive Dependency Graph**: LibWeb relies on hundreds of files and generated IDL bindings.
2.  **Code Generation**: Upstream uses Lagom (host build) to generate C++ from IDL/CSS files.
3.  **Math/Float Complexity**: Layout and JS require heavy floating-point support.

## Implementation Strategy: "Stub-and-Bridge"
- **Surgical Pull**: Do not copy full libraries. Pull only the manifest required for a "Hello Engine" smoke test.
- **Manual Binding Stubs**: Instead of porting the IDL generators, manually create minimal stubs for required DOM-to-JS bindings (e.g., `Window`, `Document`).
- **Gfx Integration**: Connect the LibWeb layout output to our existing `PainterTaterTOS` backend.
- **Memory Management**: Hook LibJS Garbage Collector into `fry_mreserve` / `fry_mcommit`.

## Milestones

### Phase 5.1: Foundational Headers & DOM
- Define the minimal manifest (Document, Node, Element).
- Resolve missing includes (stubs for generated headers like `HTMLElement.h`).
- Implement TaterTOS-specific stubs for `Math` and `Float` functions if needed.

### Phase 5.2: The HTML Parser
- Integrate `HTMLParser` and `HTMLTokenizer`.
- Implement a basic `DOM::Tree` builder.
- **Verification**: `web_smoke --parse "<h1>Hello</h1>"`

### Phase 5.3: Layout & Rendering
- Integrate a minimal CSS parser and layout engine (LibWeb/Layout).
- Connect `Layout::Box` painting to `PainterTaterTOS`.
- **Verification**: Render a single <h1> tag to the TaterTOS GUI.

### Phase 5.4: JS Virtual Machine (Optional for initial smoke)
- Initialize a `JS::VM` and `GlobalObject`.
- Run a minimal script: `console.log('TaterTOS Engine Alive')`.

## Verification & Testing
- **`web_smoke`**: A dedicated test app that initializes the engine, parses a string, and dumps the DOM tree or renders to a bitmap.
- **Memory Audit**: Verify that the JS heap isn't leaking or conflicting with the TaterTOS kernel.
