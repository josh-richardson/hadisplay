# Color Output Spec

This document defines the changes required for `hadisplay` to render native color output via FBInk on the Kobo Clara Colour (`393`) while preserving grayscale fallback behavior on non-color targets.

## Goals

- Render the main app UI in color on Clara Colour class devices.
- Keep the current grayscale rendering path available as a fallback.
- Avoid depending on unmerged or private FBInk changes.
- Roll out color support in phases so the first version proves correctness before tuning refresh quality.

## Non-Goals

- Generalize the app for every Kobo model.
- Introduce image-heavy theming or a major visual redesign in the first pass.
- Depend on FBInk text/image APIs for the main UI scene graph.

## Current State

The app is already built against an FBInk `master` commit that recognizes Clara Colour `393`, but the app renderer itself is grayscale-only.

Current implementation constraints:

- `render_scene` allocates a `width * height` single-channel framebuffer.
- Scene colors are hardcoded as grayscale bytes in `scene_style.h`.
- Drawing helpers and icon helpers all accept `unsigned char` pixel values.
- `render()` passes the grayscale buffer directly to `fbink_print_raw_data`.
- The X11 mirror decodes dumped framebuffer content correctly, but it assumes the app generated grayscale input.

Relevant files:

- `src/app/main.cpp`
- `src/common/scene.h`
- `src/common/scene.cpp`
- `src/common/scene_draw.h`
- `src/common/scene_draw.cpp`
- `src/common/scene_icons.h`
- `src/common/scene_icons.cpp`
- `src/common/scene_style.h`
- `src/fbink_mirror/main.cpp`

## External Constraints

FBInk `master` already exposes the primitives needed for a first implementation:

- raw `RGB` / `RGBA` input via `fbink_print_raw_data`
- Kobo Kaleido waveform modes such as `WFM_GCC16` and `WFM_GLRC16`
- `has_color_panel` in `FBInkState`
- CFA post-processing options in `FBInkConfig`

This means the work is primarily in `hadisplay`, not in upstream FBInk.

## Functional Requirements

### Rendering Modes

The app must support three display modes:

- `auto`: use color when FBInk reports a color panel, otherwise grayscale
- `grayscale`: force the current grayscale path
- `color`: force the color path when supported, otherwise fall back to grayscale and log the downgrade

### UI Behavior

The first color-capable release must:

- preserve existing layout and interaction behavior
- render readable text and icons with no regression in hit targets
- use color only where it improves clarity
- keep sufficient contrast when color is quantized by the panel

Initial color usage should be intentionally limited:

- weather icon accents
- RGB preset buttons
- status chips and selected states
- battery / Wi-Fi / climate highlights where useful

The app must remain legible if the panel reduces saturation or the waveform weakens chroma.

### Refresh Behavior

The implementation must distinguish between two stages:

- MVP: prove correct color rendering, even if that means more full refreshes
- tuned mode: optimize waveform choice, CFA mode, and partial refresh strategy for acceptable ghosting and latency

## Technical Design

### 1. Add A Render Target Abstraction

Replace the implicit grayscale framebuffer contract with an explicit render target model.

Required types:

- `enum class PixelFormat { Gray8, RGBA32 }`
- `struct Color { std::uint8_t r, g, b, a; }`
- `struct RenderBuffer { PixelFormat format; int width; int height; std::vector<unsigned char> pixels; }`

Behavior:

- `Gray8` buffers store one byte per pixel
- `RGBA32` buffers store four bytes per pixel in RGBA order
- drawing helpers write through a shared API instead of assuming grayscale addressing

### 2. Add Device Display Capability Detection

Introduce a display capability structure populated from `fbink_get_state`.

Required fields:

- `bool has_color_panel`
- `FBINK_PXFMT_INDEX_T pixel_format`
- selected app output mode after resolving config and hardware support

This should be computed once at startup and passed into rendering and refresh selection code.

### 3. Generalize Scene Colors

Replace grayscale constants with semantic theme tokens.

Examples:

- `background`
- `surface`
- `surface_muted`
- `border`
- `text_primary`
- `text_muted`
- `accent_red`
- `accent_green`
- `accent_blue`
- `accent_yellow`

Requirements:

- one grayscale palette
- one color palette tuned for Kaleido panels
- no raw grayscale bytes in scene composition code outside the palette definitions

### 4. Generalize Drawing Primitives

Update drawing helpers in `scene_draw.*` and `scene_icons.*` so they can draw into either output format.

Required changes:

- `set_pixel`, `fill_rect`, `draw_rect`, `draw_line`, `draw_arc`, and text drawing must accept `Color` or a palette token
- pixel write logic must branch on `RenderBuffer.format`
- alpha can initially be treated as opaque-only for app-generated pixels

The app does not need antialiasing in the first pass.

### 5. Update Scene Composition

Change `render_scene` to return `RenderBuffer` instead of `std::vector<unsigned char>`.

The scene layer must:

- choose the palette based on resolved display mode
- render the same layout to either grayscale or RGBA
- keep semantic color choices centralized instead of inlining RGB literals throughout `scene.cpp`

### 6. Update FBInk Submission

Update `render()` in `src/app/main.cpp` to submit the correct byte count and choose waveform settings based on output mode.

Required behavior for the MVP:

- grayscale mode: preserve current submission behavior
- color mode: submit RGBA32 raw data
- color mode MVP: prefer safer full refresh behavior over aggressive partial refresh reuse

Initial waveform policy:

- grayscale full refresh: current behavior
- grayscale partial refresh: current behavior
- color refresh MVP: use `WFM_GCC16` for user-visible scene renders until tuned on-device

Future tuning knobs:

- `WFM_GLRC16` for highlight-oriented partial updates
- `cfa_mode`
- `saturation_boost`
- refresh cadence thresholds specific to color mode

### 7. Add Configuration

Add a new app config field:

- `display_mode`: `auto`, `grayscale`, or `color`

Requirements:

- persisted in the JSON config
- defaults to `auto`
- invalid values must fall back to `auto`

Optional second-phase settings:

- `color_refresh_mode`
- `cfa_mode`
- `saturation_boost`

These should not block the first implementation.

### 8. Update The Mirror And Smoke Targets

The debugging targets must reflect the new capabilities.

Required changes:

- `hello_fbink_linuxfb` should gain a color smoke mode or a dedicated color smoke binary
- `hello_fbink_mirror_x11` should render and present color app output when color mode is active

The smoke target should validate:

- raw RGBA submission succeeds on device
- red, green, blue, and neutral patches are visibly distinct
- text remains readable on colored surfaces

## Implementation Plan

### Phase 1: Plumbing

- add render target, color, and palette abstractions
- update drawing primitives and icon helpers
- keep scene visuals effectively grayscale while running through the new abstraction

Exit criteria:

- host build passes
- Kobo build passes
- grayscale output matches current behavior closely

### Phase 2: MVP Color Output

- detect color panel capability from FBInk
- add `display_mode`
- submit RGBA buffers in color mode
- apply a restrained color palette to selected UI elements
- add a color smoke validation path

Exit criteria:

- Clara Colour shows distinct color accents
- no broken layout or unreadable text
- grayscale fallback still works

### Phase 3: Refresh Tuning

- tune waveform selection for color mode
- experiment with `WFM_GCC16`, `WFM_GLRC16`, CFA modes, and saturation boost
- decide whether color mode can safely keep partial refreshes or should force more full refreshes

Exit criteria:

- acceptable ghosting
- acceptable latency for taps and state updates
- no obvious color corruption after repeated screen transitions

## Risks

### Refresh Quality Risk

The largest uncertainty is not buffer generation, but how the Clara Colour panel behaves with repeated app-style refreshes. The first version should optimize for correctness, not refresh speed.

### Contrast Risk

Kaleido panels reduce contrast compared with monochrome e-ink. Colors that look distinct in an emulator may become muddy on-device. The palette must prioritize readable text and large color blocks over subtle tints.

### Performance Risk

RGBA rendering increases framebuffer size by 4x. A full-screen Clara Colour buffer is still manageable, but this may require revisiting the existing redraw-on-every-render behavior if responsiveness degrades.

### Scope Risk

If color is applied too broadly in the first pass, palette tuning will dominate the work. The first implementation should add color accents, not redesign every surface.

## File-Level Change List

Expected code changes:

- `src/common/scene.h`
- `src/common/scene.cpp`
- `src/common/scene_draw.h`
- `src/common/scene_draw.cpp`
- `src/common/scene_icons.h`
- `src/common/scene_icons.cpp`
- `src/common/scene_style.h`
- `src/common/app_config.h`
- `src/common/app_config.cpp`
- `src/app/main.cpp`
- `src/fbink_mirror/main.cpp`
- `src/fbink_smoke/main.cpp`
- `README.md`

Potential optional changes:

- test helpers or host-only visualization utilities
- deployment docs for color smoke validation

## Acceptance Criteria

The feature is complete when all of the following are true on a Kobo Clara Colour running the current supported setup:

- `display_mode = auto` chooses color output
- `display_mode = grayscale` reproduces the current grayscale UI
- `display_mode = color` renders via RGBA submission without crashing or silently corrupting output
- at least red, green, blue, and yellow UI accents are visibly distinct on-device
- all text remains readable in dashboard, setup, and detail views
- touch interaction remains unchanged
- exiting the app still restores Nickel cleanly

## Open Questions

- Should MVP color mode force full refreshes for every scene change, or only for selected transitions?
- Is `WFM_GLRC16` materially better than `WFM_GCC16` for small highlight updates on Clara Colour?
- Should the app expose low-level color tuning in config, or keep those values hardcoded until a stable device profile exists?
- Do we want a single cross-platform scene renderer with runtime pixel format branching, or separate grayscale and RGBA fast paths once the design settles?
