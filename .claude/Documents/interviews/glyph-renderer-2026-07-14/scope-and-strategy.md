# Scope & Strategy — Glyph/Text renderer

**Date:** 2026-07-14 · Interview round 1

## Key Points (decided)
- **Charset:** full printable ASCII (95, 0x20–0x7E) for the first cook. A real reusable engine font,
  not a #22-only digit strip.
- **Representation family:** MUST be **scalable + always smooth** across resolutions/aspect ratios.
  → **Plain bitmap font is RULED OUT** (crisp only at integer scale, no AA).
- **Multiple selectable fonts** is a first-class requirement — this is a *font system*, not one font.
  Explicit example: a "classic digital square-looking" font for a speed-chess clock, distinct from the
  general UI font. Implies per-font assets + metrics + a font-selection API.
- **TextField is a layout widget, not just a glyph blitter.** Required behaviors:
  - Automatic newlining / **word-wrap** when text exceeds the field width.
  - **Overflow warning/highlight** when text cannot fit inside the field size (dev-facing signal).

## Details / implications
- Scalable + smooth ⇒ SDF or MSDF (or vector/analytic). Raster atlas is out too (blurs when scaled).
- The square/digital clock font has **sharp corners**. Single-channel **SDF rounds corners**; **MSDF
  preserves them**. So the sharp-cornered-font requirement is a strong vote for **MSDF** over SDF —
  *pending* the cook-feasibility question below.
- "Choose between fonts" ⇒ the atlas/cook pipeline is per-font; the runtime needs a `Font` handle/asset
  and the `TextField`/`TextBar` takes a font selector. Metrics (advance, bearing, line height, ascent/
  descent) must be cooked per font, per glyph — not just UV rects.
- The TextField layout logic (wrap + overflow) is largely **independent of the glyph representation** —
  it operates on per-glyph advance metrics, so it can be designed once regardless of SDF vs MSDF.

## Open Questions (driving Phase 3b gap research + next interview rounds)
1. **SDF vs MSDF vs multi-channel variants** given a required sharp-cornered digital font — how bad is
   SDF corner-rounding in practice, and is MSDF the clear call?
2. **CRUX — cooking SDF/MSDF with NO font library** (no msdfgen, no FreeType, no runtime rasterizer):
   - Where do glyph shapes come from without a TTF parser? (hand-authored hi-res bitmaps / per-glyph
     SVG via the existing web-proxy raster path / checked-in prebaked atlas / hand-written TTF glyf
     parser?)
   - Distance-transform algorithms writable offline (8SSEDT, dead-reckoning) for SDF.
   - Is a from-scratch **MSDF edge-coloring/shape-decomposition** generator realistic to hand-write, or
     is that the point where we'd need to reconsider (accept SDF, or design fonts to dodge rounding)?
3. **Multi-font architecture** — how to structure N font assets + selection cleanly in the engine.
4. **Text layout** — word-wrap/line-break + overflow-detection approach for game UI.

## Deferred (revisit after representation is locked)
- Renderer feed (push-constant uvRect vs baked-UV mesh vs material-per-glyph vs dynamic VB vs instancing)
  — user explicitly deferred until representation is settled. Note: SDF/MSDF need a NEW pipeline/shader
  regardless, which reshapes this decision vs the bitmap case.
- Font source (BDF vs hand-authored vs custom art) — deferred; now reframed by the no-library SDF/MSDF
  cook question above.
