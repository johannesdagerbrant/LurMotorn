# Research: Glyph / Text Renderer for the LurMotorn engine

**Date:** 2026-07-14

## Problem Statement

The engine has **no text rendering**. The renderer draws textured/tinted quads only. Issue #22
(on-screen W/L/D + match-result HUD) is blocked on a minimal glyph renderer, which must land as
reusable engine HUD infrastructure (`Modules/Hud`), not a Chess-only hack.

Two coupled decisions dominate:
1. **Glyph representation** — how a glyph is stored (bitmap / raster atlas / SDF / MSDF / vector).
2. **Renderer feed** — how per-glyph UVs reach the GPU given the current draw API, which passes only
   `(mesh, material, modelMatrix)` and has **no per-draw UV or color override**, plus a hard
   `MaxMaterials = 32` descriptor-pool cap.

Constraints that bind: no third-party libraries (incl. build-time font libs, per policy — see caveat
below); single Vulkan pipeline on the portability subset (MoltenVK on iOS + native on Android);
cook-at-build asset pipeline with no runtime decoders. Text is **local-only** — it never touches the
BLE wire, so the slim-payload rule does not apply to glyphs; atlas memory and draw/state-change count
are the real budgets, and both are tiny at HUD scale.

## Option Comparison — glyph representation

Numbers: "digit set" ≈ 16 glyphs (`0-9 W L D : - /`); "full ASCII" = 95 printable. Payload only.

| Option | Mem (digits) | Mem (full ASCII) | Special shader? | Scaling | AA | Cook complexity (no font lib) |
|--------|-------------|------------------|-----------------|---------|----|-------------------------------|
| **1. Hand-authored / BDF bitmap** | ~0.1-0.25 KB | ~0.75-1.5 KB | **No** | Crisp at **integer** scale only | None | **Lowest** — no rasterizer needed |
| **2. Cooked raster atlas (A8)** | ~16 KB @32px | ~95 KB @32px | No | Blurs magnified, shimmers minified | Excellent @native | Low-med — needs offline rasterizer |
| **3. SDF** (Green 2007) | ~16 KB @32px | ~95 KB @32px, one atlas all sizes | **Yes** (smoothstep+fwidth) | **Crisp any scale** | Good; rounds corners | Medium — hi-res raster + distance transform |
| **4. MSDF** (Chlumský) | ~48-64 KB | ~290-390 KB | **Yes** (median+fwidth) | Crisp + sharp corners | Best field-based | Med-high — edge-coloring |
| **5. Vector/analytic** (Loop-Blinn/Slug) | ~0.3-2 KB | ~10-50 KB | **Most complex** | Perfect | Best | **Highest** |

**The dominating fact for a no-library engine:** every option *except* the hand-authored/BDF bitmap
needs glyph outlines from a TrueType/vector source, i.e. a rasterizer. If "no third-party lib" extends
to build-time tooling, options 2-5 mean hand-writing a TTF `glyf` parser + scan-conversion rasterizer
(plus a distance transform for SDF/MSDF, an accel structure for vector) *before* you can cook anything.
The bitmap option skips all of it: a public-domain BDF is plain text you parse in a dozen lines, or you
hand-draw ~16 glyphs.

## Detailed Findings

### 1. Hand-authored / BDF bitmap font — best fit for a fixed-size HUD
Glyphs as literal pixel masks (BIOS/VGA/8-bit lineage). IBM VGA 8×16 = 16 B/glyph; full 256-char page
= exactly 4 KB. No shader work — sample a 1-bit/coverage mask with nearest (or reuse the existing
alpha-blend + tint path). Perfectly crisp at integer scale; jaggy at fractional scale/rotation; no AA
by construction (irrelevant for fixed-size HUD). **Uniquely needs zero font tooling.** BDF is trivially
parseable: per glyph `ENCODING`, `BBX w h xoff yoff`, then `h` hex rows (MSB=left, right-padded to a
byte). unscii-16 (public domain) and pcface Modern-DOS (MIT/CC0, pre-baked integer arrays) are
commercial-safe sources. Refs: OSDev VGA Fonts; unscii (viznut.fi/unscii); pcface; spleen; Cozette.

### 2. Cooked raster atlas — best AA at one size, but the scaling problem
Rasterize a font once at build time into an A8 coverage atlas; runtime = alpha-blended quads (the
"traditional" glyph cache; Aras Pranckevičius 2017). Excellent AA *at the baked size*; blurs when
magnified, shimmers when minified → needs per-size atlases or mips. This is essentially what the chess
piece pipeline already does (SVG→proxy→coverage bytes). Memory ~1 KB/glyph @32px.

### 3. SDF — one small atlas, crisp at any scale
Green, *Improved Alpha-Tested Magnification*, SIGGRAPH 2007 (Valve/TF2). Store signed distance to the
nearest edge (0.5 = on the outline); bilinear-filtering *distance* keeps magnified edges crisp. Valve:
a 64×64 8-bit field reconstructs edges comparable to a 4096² source. Simple shader
(`smoothstep(0.5-w,0.5+w,d)`, `w=fwidth(d)`); near-free outlines/glows. Downside: single scalar field
**rounds sharp corners** and degrades below field resolution. Medium cook (hi-res raster + distance
transform, hand-writable offline).

### 4. MSDF — SDF that keeps sharp corners
Chlumský, msdfgen / CGF 2018. Three channels; render distance = `median(r,g,b)`; corners survive as
the intersection of two straight iso-lines. Best field-based quality, ~3× SDF memory, and the
edge-coloring/shape-decomposition cook is hard to reproduce without msdfgen. Overkill unless
sharp-cornered letterforms at large scale are a stated requirement.

### 5. Vector / analytic — resolution-independent, heaviest everything
Loop-Blinn 2005 / Slug / GLyphy. Store outline control points, evaluate coverage per pixel. Most
compact payload, perfect scaling, exact corners — but the most complex shader + heaviest offline
tooling (outline extraction + acceleration structure). Over-engineered for a HUD.

## Renderer-feed options (given no per-draw UV, MaxMaterials=32)

To draw N glyphs from one atlas the mechanism must get per-glyph UVs to the GPU. Four ways:

| Feed | Draw calls | Renderer change | MoltenVK verdict | Notes |
|------|-----------|-----------------|------------------|-------|
| **A. Push-constant uvRect** | 1/glyph | +`vec4` push const (80→96 B) + vertex-shader remap + SPIR-V recook | **Fine** — 96 B ≪ 128 B portable floor (MoltenVK reports 4096); push-const is MoltenVK's *fast* path | Smallest, cleanest reusable extension; one atlas texture + one material |
| **B. Baked-UV mesh per glyph** | 1/glyph | **None** — pure content; one static quad mesh per glyph w/ atlas UVs baked, positioned by Model | Fine | Zero renderer change; needs a small mesh cache; awkward for proportional/kerned text |
| **C. Material per glyph** | 1/glyph | None, but burns 1 material/glyph | Fine ≤ cap | 0-9 = 10 mats OK; full font blows the 32-cap; per-draw descriptor rebind is MoltenVK's *slow* path |
| **D. Dynamic vertex buffer** (ImGui) | 1 total | +per-frame host-visible VB w/ baked per-vertex UVs; needs per-vertex UV+color already in `Vertex` (they exist) | Fine — UMA, ring/N-buffer per frame in flight | Most flexible (proportional, kerning, color); most new code |
| **E. Instancing** | 1 total | +per-instance attrs | Fine if `firstInstance=0` | Overkill <50 glyphs/frame |

**Batching does not matter at HUD scale** (5-20 glyphs/frame). Vendor guidance (NVIDIA/AMD/Arm): the
cost is `vkCmdBindPipeline`/descriptor rebinds and submits, not extra `vkCmdDraw`. Tens of draws is
free. **The rule that does matter: keep all glyphs on one atlas texture / one descriptor set / one
pipeline** — then 1 draw vs 20 is irrelevant, and you avoid MoltenVK's documented per-draw-rebind slow
path (issue #2549: bound-buffer-per-draw is ~3-10× slower than push constants on Apple).

So the feed choice collapses to: **A (push-constant uvRect)** for the cleanest minimal reusable
extension, or **B (baked-UV mesh per glyph)** for zero renderer change, both drawing one glyph per call
from a single atlas material. C is the chess-piece pattern but caps out. D is the "real" text engine if
proportional/colored/long strings are ever wanted. The existing `Vertex` already carries per-vertex
`Uv` and `Color`, so D is *possible* without a vertex-format change — only the draw path is missing.

## Production References
- **Dear ImGui**: one atlas (stb_rect_pack skyline), per-glyph UV rect + advance stored once; `AddText`
  bakes 4 verts+6 idx/glyph with UVs *in the vertex buffer* into shared buffers → whole HUD in 1-3
  draws. `ImDrawVert` = pos(8)+uv(8)+col(4)=20 B; 16-bit indices. UVs live in vertices, not a uniform —
  why it composes with any "draw textured quad" API.
- **Quake/Doom**: `conchars` 128×128 = 16×16 grid of 8×8 glyphs; `Draw_Character` derives cell by
  `row=num>>4, col=num&15`; GL port sets UV `(col/16, row/16)`, extent `1/16`, one quad/char. Simplest
  thing that works with a fixed-width grid.
- **PICO-8 / GBA Tonc / SSD1306**: fixed bitmap font, per-glyph {offset,width,advance}, blit from one
  source. Through-line everywhere: *cook once, store per-glyph UV/offset + advance, place quads from one
  atlas.* Only variation is where the UV lives.

## Integration Considerations (LurMotorn specifics)
- **Reuse the existing pipeline unchanged for representation 1/2**: `Sprite.frag` already does
  `texture*tint` alpha compositing; a coverage-alpha glyph atlas renders through it with a tint material
  exactly like piece masks. SDF/MSDF (3/4) would need a *new* pipeline + SPIR-V cook.
- **Cook like PieceMasks**: `scripts/gen-piece-masks.ps1` → embedded `unsigned char[...]` header is the
  template. A `gen-font.ps1` → `FontAtlas.h` (coverage bytes + per-glyph cell/UV table) mirrors it. BDF
  parse (unscii-16) or hand-authored bytes both avoid a rasterizer; the web-proxy route (representation
  2) reuses existing infra but reintroduces a build-time rasterizer dependency (the proxy).
- **Widget shape = `LinkStatusBar`**: `CreateResources(renderer)` once; `Draw(renderer, …, X,Y,W,H)`
  each frame inside BoardView's BeginFrame/EndFrame using the pixel-space ortho cam. New home:
  `Modules/Hud/Public/Lur/Hud/*.h` + `Private/*.cpp`, e.g. `Lur::Hud::TextBar` / `DigitStrip`.
- **Screen-fixed placement**: reuse BoardView's top-margin math (link bar sits at `Y=Inset`, ignores
  board flip). A score line slots beside/below it in pixel coords.
- **Data**: `ChessMatchState::Record()` → `ChessRecord{WinsLower,WinsHigher,Draws}`; orient mine/theirs
  via `IsLocalLower()`; `LastResult()` → `EGameResult{Ongoing,Checkmate,Stalemate,DrawFiftyMove,
  DrawInsufficientMaterial}` (winner inferred from Checkmate + side-to-move).
- **If feed A (push-constant uvRect)**: bump `PushConstants` to add `vec4 UvRect`, remap UVs in
  `Sprite.vert`, recook `Sprite.vert.inc` via `gen-shaders.ps1`, keep the C++ `VkPushConstantRange` in
  lockstep. 96 B is safe on the portable 128 B floor. Consider bumping `MaxMaterials` regardless if any
  per-glyph-material path is used.

## GAP RESEARCH (post-pivot 2026-07-14): scalable text, no font library

User pivoted: text must be **scalable + always smooth**, support **multiple selectable fonts** (incl. a
**sharp squared "digital clock" font**), and a **TextField with word-wrap + overflow warnings**. This
rules out bitmap and raster-atlas; the choice is within the **distance-field family**. Key findings:

### The corner problem dissolves — the two fonts land on opposite easy sides
- **Single-channel SDF rounds sharp corners** (a corner can't be sharper than ~1 SDF texel; rounding
  arc ≈ `output_px / sdf_cell_px` texels — invisible at ≤bake size, glaring at large magnification).
  "Hi-res SDF" only shrinks the arc, never makes a true point. Only **MSDF (median-of-3)** or analytic
  geometry gives genuinely sharp corners at any scale.
- **MSDF-from-scratch is a research project**, not plumbing: edge-coloring (~150-250 LOC) + per-channel
  analytic Bézier distances w/ pseudo-distance (~300-500 LOC, bug-prone) + Chlumský's clash-resolution
  pass. Faithful quality ≈ 800-1500 LOC / 1-2 weeks. Naive ≈ 400-600 LOC / 3-5 days w/ corner glitches.
- **But we never need it**, because the requirements don't coincide:
  - **Digital/segmented clock font = union of axis-aligned rectangles.** Signed distance to a box is
    exact/analytic (Inigo Quilez 2D SDFs); SDF-of-union = `min` of box fields. Perfect corners, **no
    rasterizer, no font parse, no MSDF, ~1 day.** (Or skip the atlas and draw the rectangles in-shader.)
  - **UI font = mostly curves, few true sharp corners** → single-channel SDF rounding is **invisible**.
  So sharp corners live only in the font that's trivially sharp; curves live only in the font where
  rounding doesn't show. **MSDF is avoidable entirely.** (Validate with one baked test glyph first;
  the "SDF good enough" claim depends on max magnification ratio.)

### Where UI-font glyph shapes come from with NO font library — ranked
- **(e) Offline authoring tool, commit the output.** Run `msdf-atlas-gen`/`msdfgen` **once** on a dev
  machine; commit raw atlas bytes + cooked metrics header. Nothing links/invokes/ships it — it's an art
  tool like Blender, output is a data asset. **Hours of work, professional MSDF, perfect corners.**
  Precedent in-repo: piece art is already cooked via a *third-party network service* (`images.weserv.nl`)
  at build time; a local offline binary is strictly more conservative. Purist counter: the atlas is
  "library output as data." A user constraint call (CLAUDE.md: raise library needs with the user).
- **(c) Hand-written minimal TrueType `glyf` parser.** Cleanest strict-purism path: all code is ours,
  the `.ttf` is permissively-licensed *data*. Parse `head/cmap(fmt4)/loca/glyf/hmtx`, tessellate
  quadratic Béziers → hi-res coverage bitmap → distance transform. **~300-600 LOC, 2-4 days.** Avoid
  CFF/OpenType-PostScript outlines (Type2 charstring interpreter, 1000+ LOC). ASCII-only contains edge
  cases (composites, cmap). Yields single-channel SDF (rounded corners — fine on curves).
- **(d) Hershey vector fonts.** Public-domain `.jhf` polylines, full ASCII. Glyphs are **stroke/
  centerline**, not filled → compute an **analytic stroked SDF** (dist to polyline − half stroke width),
  no bitmap/DT. **~1-2 days.** Engraved/single-stroke CAD look (not a normal typeface); pairs well with
  a segmented digital font aesthetically.
- **(a) Hand-authored hi-res bitmaps.** Trivial code (just the DT) but hand-drawing a whole typeface is
  days-to-weeks of *art* and won't look professional. Only for a bespoke pixel face.
- **(b) Per-glyph SVG via the weserv proxy.** Not an independent shape source — the SVG outlines still
  must come from (c) or an offline tool. It's only a rasterization transport we already have.

### Distance transforms writable from scratch (for paths c/a, single-channel SDF)
- **Dead Reckoning** (Grevera 2004, ~200 LOC single-header reference to rewrite) or **8SSEDT**
  (two-pass 8-point sweep). ~1 day, couple hundred LOC, good quality. Low risk. Single-channel only.

### Multi-font architecture (canonical: BMFont `.fnt` + msdf-atlas-gen JSON)
- A cooked font = **one atlas texture + per-glyph metrics + font-global metrics + optional sparse
  kerning**. Store metrics **em-normalized** (msdf style, resolution-independent), multiply by runtime
  pixel size — this is what makes text scalable. Convert to one internal convention (baseline-relative,
  y-up) on import.
- Per-glyph: `advance`, plane bounds (baseline-relative em quad), atlas UV rect. Per-font: `lineHeight`,
  `ascent`, `descent`, `emSize`, `distanceRange` (→ shader `screenPxRange = distanceRange * pixelSize`).
- **Data model:** `Font` = {FontMetrics, dense GlyphMetrics[], codepoint→index map, optional kerning
  map, atlas TextureHandle}; owned by a `FontRegistry` handing out `FontHandle`; widgets select by
  handle. **Separate immutable `Font` (asset) from computed `LayoutResult`** (libGDX GlyphLayout
  pattern). **Kerning:** store but default-off for HUD (negligible gain, hot-loop cost).
- **Atlas: one-per-font** (not ImGui's shared mega-atlas). Few fonts, independent cook/reload, avoids
  mobile max-texture-dimension pressure; batch by sorting draws per `FontHandle`. Shared atlas only wins
  with many fonts + single-batch demand (an IMGUI concern, not a HUD one).

### Text layout: word-wrap + overflow
- **Greedy line-fill** (not Knuth-Plass — overkill for game UI). Tokenize into words, measure each once
  via summed advances, pack words, break before the word that would exceed width; **character-break
  fallback** for a single word wider than the field. Break opportunities = spaces/hyphens; consume
  trailing whitespace at the break.
- **Measure:** `lineWidthPx = pixelSize * Σ(advance + kerning)`; `blockHeight = pixelSize*lineHeight*N`.
- **Overflow:** layout returns `OverflowX/OverflowY` booleans for free. Production = clip (scissor) +
  optional ellipsis truncation; **dev build = red field outline + one-shot log** (what the user wants),
  gated behind a `TextDebugOverflow` flag. Auto-shrink-to-fit = binary search on scale, **re-wrapping
  each probe** (wrap points move with scale).
- **Alignment:** L/C/R = `x ∈ {0, (W-lw)/2, W-lw}`; vertical = `yTop ∈ {0,(H-bh)/2,H-bh}`, first
  baseline `= yTop + ascent*S`.
- **Scale independence:** all layout math in em; `maxWidthEm = fieldWidthPx / pixelSize`; snap to pixels
  ONLY at vertex emission, never during accumulation (rounding error would shift wrap points across
  resolutions).

### Feed decision, re-framed by the pivot
SDF/MSDF need a **new pipeline + fragment shader** (smoothstep/fwidth AA; median for MSDF) regardless —
the existing sprite pipeline can't be reused as-is (unlike the bitmap case). So the feed sub-decision
(push-constant uvRect vs baked-UV mesh vs dynamic VB vs instancing) now rides on top of "we're adding a
text pipeline anyway." The **dynamic-VB / instanced path (one atlas, per-vertex or per-instance UV)**
becomes the natural fit for variable-length wrapped strings, vs. the bitmap-era "one quad per glyph."

## Where sources disagree / uncertainty
- Valve's exact 64×64-from-4096² figures are "as reported" (PDF not machine-read line-by-line).
- SDF field-size / pxrange numbers are empirical + font-specific (sources give ranges, not laws).
- LOC/dev-day estimates for parsers/DT/MSDF are ±50%; the *ordering* (bitmap ≪ SDF ≪ MSDF) is solid.
- "Single-channel SDF good enough for the UI font" depends on max magnification — validate one glyph.
- Representation memory for options 2-5 varies widely with cell size/packing/distance-range — ballparks.
- Smallest usable SDF text size vs a raster atlas is genuinely unsettled (parameter-tuning territory).
- MoltenVK `firstInstance` base-instance support varies by GPU/OS gen → always pass `firstInstance=0`.
- #2549 perf ratios (push-const vs bound buffer, 3-10×) are from one workload — directional, not exact.
