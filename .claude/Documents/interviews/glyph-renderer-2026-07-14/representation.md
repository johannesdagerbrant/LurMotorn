# Representation decision — Glyph/Text renderer

**Date:** 2026-07-14 · Interview round 2 (in progress)

## Key Points (locked by research)
- **Family = distance-field text** (satisfies scalable + always-smooth). Bitmap and raster-atlas are out.
- **The corner problem dissolves via a per-font strategy** — the two font requirements sit on opposite
  easy sides:
  - **Digital/segmented clock font → analytic axis-aligned-rectangle SDF.** Exact, perfect corners, no
    rasterizer/parser/MSDF, ~1 day. (Segments are rectangles; SDF-of-union = min of box fields.)
  - **UI font (curvy, full ASCII) → single-channel SDF.** Corner rounding is invisible on curves.
  ⇒ **MSDF-from-scratch (1-2 week research project) is avoided entirely.**
- Text is local-only (never on the BLE wire) → wire-slimness doesn't bind; atlas KB + state changes are
  the only budgets, both trivial at HUD scale.

## The open call (needs the user — a "no third-party library" constraint decision)
How do the **UI font's curvy glyph shapes** get produced, given no font library and no runtime
rasterizer? This determines SDF-vs-MSDF and the whole cook pipeline:
- **(e) Offline authoring tool** (`msdf-atlas-gen`), commit atlas+metrics → best quality, perfect
  corners (free MSDF), runtime 100% ours; but a 3rd-party tool at *authoring* time. Precedent: piece art
  already cooked via the `images.weserv.nl` network service.
- **(c) Hand-written TTF `glyf` parser + our own SDF** → strictly all our code; TTF is just data; ~3-5
  days; single-channel SDF (rounded corners, fine on curves).
- **(d) Hershey stroked-SDF** → PD polyline data, ~1-2 days, engraved look (not a normal typeface).

## Settled regardless of the above
- Digital clock font: analytic rectangle SDF.
- Multi-font: one atlas per font; `Font` asset {metrics + glyph table + atlas} owned by a `FontRegistry`
  handing out `FontHandle`; metrics stored em-normalized; separate `Font` (asset) from `LayoutResult`.
- TextField: greedy word-wrap + char-break fallback; `OverflowX/Y` flags; dev-build red-outline warning
  + one-shot log; all layout in em, snap to pixels at emission only.
- A new SDF text pipeline + fragment shader (smoothstep/fwidth AA) is needed regardless.

## Open Questions (later rounds)
- Renderer feed (dynamic VB vs instanced vs push-constant uvRect) — now rides on the new text pipeline.
- Sequencing / YAGNI: ship #22 on a minimal slice first, or build the font system fully up front?
