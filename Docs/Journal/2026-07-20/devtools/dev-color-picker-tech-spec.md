# Tech Spec — Dev Color Picker Widget

*2026-07-20. Status: draft for review. Companion to `dev-console-cvar-tech-spec.md` (this expands Addendum E.3). The color picker is the one dev widget with real UX depth — HSV geometry, drag math, hex round-tripping — so it gets its own spec. Grounded in `Modules/Render/Renderer.h` at `d1dbb76`.*

> **Governing constraints (inherited, not re-litigated):** dev-only (`#if !LUR_INTERNAL`, absent from shipping); built on our own `DevGui` layer + `DevTheme` (no IMGUI lib); engine-owned and identical across games (Addendum D.0); edits a `CVar<Color>` and commits through the shared CVar path (persist locally per Addendum B; almost always `AffectsGameplay = false`, so no wire/sync/hash involvement).

## 1. Goal

A **UX-friendly** color picker in the desktop settings panel (and available as a `DevGui` widget anywhere in the dev layer) for tuning `CVar<Color>` values — unit tints, HUD accents, debug-draw colors — with immediate visual feedback. "UX-friendly" concretely: pick a hue and a shade by *pointing*, not by typing numbers; see the result live on a swatch and in the game (left half of the tune window); still type exact hex/RGBA when precision matters.

## 2. Rendering reality (what the renderer gives us)

Verified in `Renderer.h`:

- **`DrawInstances`** takes `InstanceData` with a **single `Color` tint per quad** — no per-corner color. Good for flat rects (swatch, handles, borders), **not** for gradients.
- **`DrawGlyphs`** takes a **`Vertex{ Vec2 Pos; Vec2 Uv; Vec4 Color }` array** with **per-vertex color**, from a per-frame arena. This is the gradient primitive: a gradient rect = two triangles with different corner colors. (Despite the name, it's a general per-vertex-colored triangle path; the SV square and hue strip are drawn through it.)
- No HSV, clamp, or lerp helpers exist in `Modules/Math` yet → the picker carries its own small color-math header (§4). Colors are `Render::Color { float R,G,B,A }` in 0..1.

**Consequence:** the picker is built from two draw idioms already present — per-vertex-colored triangles (gradients) via the glyph/vertex path, and flat tinted quads (handles/swatch/borders) via instances/quads. **No new renderer capability is required**; if anything, a tiny `DrawVerts(Vertex*, count, noTexture)` convenience may be wanted so the gradient path doesn't route through a font atlas — confirm whether `DrawGlyphs` can draw untextured (flat) or if a 1px white texture is the trick (§7 open item).

## 3. Anatomy & interaction

### 3.1 v1 — simple (acceptance-blocking, ~1 day)

Reuses existing widgets entirely; no gradients, no HSV:
- Four **RGBA sliders** (the Addendum E.2 slider) 0..1, live swatch, hex + RGBA `TextInput` (console field + `FromString<Color>`).
- Ships the whole `CVar<Color>` feature as usable; everything below (v2) is a pure UX upgrade behind the same value/commit path.

### 3.2 v2 — the UX-friendly target

Classic HSV picker, all on the primitives in §2:

- **SV square** (saturation × value): drawn as a **two-layer gradient** — the correct, primitive-only construction:
  1. **Layer 1 — hue→white horizontal gradient:** a quad whose left edge is white and right edge is the full-saturation hue `HSVtoRGB(H,1,1)`, interpolated left→right. This is the *saturation* axis at full value.
  2. **Layer 2 — transparent→black vertical overlay:** a second quad on top, fully transparent at the top edge and opaque black at the bottom, interpolated top→bottom. This darkens toward the bottom — the *value* axis.
  The composited result **is** the definition of HSV S (horizontal) and V (vertical) — correct, not approximate. Both layers are per-vertex-colored quads via the vertex path (§2); layer 2 needs alpha blending (standard). No shader work, no per-hue texture bake.
- **Hue strip** (vertical or horizontal bar): a long quad split into 6 segments (R→Y→G→C→B→M→R) with per-vertex colors at the segment boundaries → smooth hue ramp via interpolation. (A ring is nicer but more math; **strip for v2**, ring is a v3 flourish.)
- **Alpha slider**: a bar from transparent→opaque over a checkerboard backing (checkerboard = a few flat quads or a tiny tiled texture); handle shows current A.
- **Swatch**: current color over a checkerboard (so alpha reads); optionally split old|new.
- **Hex + RGBA fields**: `#RRGGBB`/`#RRGGBBAA` and 0..1 (or 0..255 — §7) numeric entry; round-trips with the visual controls.
- **Handles/cursors**: a small reticle on the SV square (at S,V), a slider handle on hue and alpha — flat quads via instances, DevTheme accent.

### 3.3 Drag model

- Uses the dev layer's pointer input (Addendum A.4: pointer pos + down/up; on desktop, mouse). Immediate-mode hit-testing per widget rect, hot/active id like other `DevGui` widgets.
- **Press inside the SV square** → active; **drag** maps cursor→(S,V) clamped to the square; releasing ends the drag. Same for hue and alpha bars (1-D map). Clicking jumps the handle to the cursor (no "grab the handle exactly" requirement — point-anywhere is friendlier).
- **Live commit while dragging** for a color CVar is fine (non-`AffectsGameplay` → local only, no wire) — the swatch and the game update every frame the value changes. (Contrast the E.2 determinism note: that applied to *gameplay* sliders that sync; a color picker doesn't sync, so continuous drag-update is free and desirable.)
- Editing a text field commits on Enter/focus-loss and moves the SV/hue/alpha handles to match (full round-trip both directions).

## 4. Color math (self-contained, dev-only)

A small `Lur::DevGui::ColorMath` (or inline in the widget), since `Modules/Math` has none:
- `HSVtoRGB(h,s,v) -> Color` and `RGBtoHSV(Color) -> (h,s,v)` — standard formulas, float 0..1 (h in 0..1 or 0..360, pick one internally; 0..1 keeps it uniform).
- `ClampColor01`, `Lerp`, hex parse/format (`#RRGGBB[AA]` ⇄ `Color`).
- The **picker holds working HSV state** while open, not just the RGBA it's bound to — because RGB→HSV is lossy at edges (S=0 loses hue; V=0 loses hue+sat). If it recomputed HSV from RGB every frame, the hue handle would jump to red whenever you drag value/sat to a corner. **So: keep H,S,V as the widget's live state; write RGBA to the CVar; only re-derive HSV from the CVar when the bound value changes externally** (e.g. hex typed, or CVar reset). This is the single most important UX correctness point — it's why naive pickers feel broken.

## 5. CVar integration

- Bound to a `CVar<Color>` (Addendum E.3). Reads current value on open (derive initial HSV once); writes RGBA on every change.
- Commit path is the shared one: set → persist to the per-game `cvars.cfg` as a local (non-`AffectsGameplay`) override (Addendum B) → **no** sync (Addendum C skipped for non-gameplay). If some color were ever `AffectsGameplay`, the picker would route through the tick-stamped sync and should then commit on **release**, not per-drag-frame (mirrors E.2) — but that's not the expected case.
- Console parity (Addendum E.3): the same CVar is editable as `name r g b a` or `name.r 0.5`; picker and console are two views of one `CVar<Color>`.

## 6. Phasing & acceptance

- **v1 (blocking):** sliders + swatch + hex field; full read/commit/persist. This is what "color CVars work" depends on.
- **v2 (UX target, tracked separately):** SV square + hue strip + alpha + reticle/handles + HSV working-state correctness (§4). The estimate is real: gradient meshes, cursor→HSV maps, drag, hex round-trip, DevTheme-consistent look, and the lossy-edge state handling.
- **v3 (flourish, optional):** hue ring instead of strip; eyedropper (sample a pixel from the game half); saved swatches; old|new split.

Acceptance (v2):
- [ ] SV square + hue strip render as smooth gradients (via the per-vertex path), alpha over checkerboard.
- [ ] Point-anywhere drag on each control; handles track; live swatch + live game update.
- [ ] Hue does **not** jump when dragging S/V into white/black/gray corners (working-HSV-state correctness).
- [ ] Hex and RGBA entry round-trip with the visual controls both directions.
- [ ] Bound `CVar<Color>` updates live and persists locally; console `.r/.g/.b/.a` edits reflect in the open picker.
- [ ] Widget is DevTheme-styled, dev-only, absent from shipping.

## 7. Open questions

1. **Untextured gradient draw:** can `DrawGlyphs` (the per-vertex path) render **without** a font atlas bound, or do we add a tiny `DrawVerts`/use a 1px-white texture for flat/gradient triangles? (Small renderer clarification; decide before v2.)
2. **Numeric range convention:** RGBA entry in **0..1 floats** (canonical, matches `Render::Color`) — also accept **0..255 ints** / hex only? Recommend: hex + 0..1 floats; 0..255 optional.
3. **Hue geometry v2:** strip (spec'd) vs ring — confirm strip for v2, ring deferred to v3.
4. **Eyedropper from the game viewport** (v3): worth it? (Very nice for matching an on-screen color; needs a framebuffer read of the left half.)
