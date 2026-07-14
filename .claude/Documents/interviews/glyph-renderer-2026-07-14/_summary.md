# Summary: Glyph / Text Renderer for the LurMotorn engine

**Date:** 2026-07-14 · Status: Complete (plan ready for a separate implementation session)
**Immediate consumer:** #22 (on-screen W/L/D + match-result HUD). **Follow-on:** #23 (HTML look prototype).

## Problem
The engine has no text rendering — only textured/tinted quads. #22 (show the persisted per-opponent
W/L/D record + match result) is blocked on this, and the user wants it built as reusable, scalable
engine text infrastructure, not a Chess-only hack.

## Solution
A **scalable MSDF (multi-channel signed distance field) text system**. Font atlases are cooked
**offline** with `msdf-atlas-gen` (a sanctioned build-time tool) from **OFL** fonts; the runtime is
100% ours — a small Vulkan MSDF pipeline + a hand-written `median(r,g,b)` + `fwidth` antialiasing
shader sampling the committed atlas. A `Modules/Text` module owns the `Font` assets, a `FontRegistry`,
and resolution-independent layout (greedy word-wrap, measure, alignment, overflow detection). A
`Modules/Hud` `TextField` widget ties layout to the renderer and draws screen-fixed HUD text. #22 wires
a score line + result banner onto it.

## Key Decisions
| Decision | Choice | Rationale |
|----------|--------|-----------|
| Representation | **MSDF** (all fonts) | Scalable + always-smooth + sharp corners; the digital clock font needs sharp corners |
| Avoid hand-rolled MSDF | Use offline tool | From-scratch MSDF generator ≈ 1-2 wk research project; the tool does it in hours, offline |
| Cook tool | **`msdf-atlas-gen` (MIT), offline only** | SANCTIONED BUILD-TOOL exception. Never linked into the app/build; only its *output* (atlas + metrics) is committed. Precedent: `images.weserv.nl` cooks piece art. Distinct from MoltenVK (a runtime dep) |
| Runtime deps | **Zero third-party** | Our own pipeline + median/fwidth shader; sample committed atlas. Engine "no-libs" rule intact |
| Fonts | **OFL only** | Commercial-safe, embeddable, huge library. Default UI = Inter (placeholder); clock = **DSEG** |
| Charset | Full printable ASCII (95) | One-time cook, no ongoing cost; a real reusable font |
| Font count in v1 | **Two** (UI + DSEG clock) | Proves the multi-font seam; DSEG is now just another cheap MSDF cook |
| Atlas strategy | **One atlas per font** | Few fonts, independent cook/reload, avoids mobile max-texture pressure; batch per font |
| Metrics units | **em-normalized** (msdf style) | Multiply by runtime pixel size ⇒ resolution independence |
| Layout | **Greedy** wrap + char-break fallback | Knuth-Plass overkill for game UI |
| Overflow | `OverflowX/Y` flags + **dev warning** (red outline + one-shot log) | User wants a visible signal when text doesn't fit; behind a `TextDebugOverflow` flag |
| Renderer feed | **Dynamic per-draw vertex buffer, baked per-vertex UVs** (ImGui pattern) + MSDF pipeline | Handles variable-length wrapped strings; one draw per font; `Vertex` already has Uv+Color |
| CUT from v1 (YAGNI) | Kerning application, auto-shrink-to-fit, ellipsis | Negligible gain / not needed for #22; add later on same architecture |
| Font aesthetic | **Deferred to #23** | Swapping a font = a re-cook; decide the whole-game look in HTML first |

## Architecture

```
Modules/Text  (NEW, pure C++; host-testable)         depends: lur_render (TextureHandle), lur_math
  Public/Lur/Text/Font.h          GlyphMetrics{Advance, PlaneBounds, AtlasUV}; FontMetrics{LineHeight,
                                   Ascent, Descent, EmSize, DistanceRange}; Font{metrics, dense
                                   GlyphMetrics[], Codepoint->index map, atlas TextureHandle, W/H}
  Public/Lur/Text/FontRegistry.h  FontHandle Load/Find/Get; owns Font lifetimes
  Public/Lur/Text/TextLayout.h    LayoutResult{lines, WidthEm, HeightEm, OverflowX/Y}; greedy wrap +
                                   char-break, Measure(), alignment math — pure, no GPU (unit-tested)

Modules/Render  (EXTEND)
  Private/Vulkan/Shaders/Text.vert/.frag   MSDF: remap atlas UV; median(r,g,b) + fwidth AA;
                                           screenPxRange = DistanceRange * PixelSize. SPIR-V cooked
                                           via scripts/gen-shaders.ps1 (new .inc files)
  New MSDF pipeline + a font/text MaterialDesc kind; a dynamic-glyph draw entry
                                   (e.g. DrawGlyphs(Vertex* verts, idx..., MaterialHandle)) building a
                                   per-frame transient VB. Extend PushConstants with ScreenPxRange
                                   (keep 16-byte alignment). Consider MaxMaterials bump (2 fonts = 2)

Modules/Hud  (EXTEND)                                depends: lur_render, lur_text, lur_net
  Public/Lur/Hud/TextField.h      CreateResources(renderer, FontRegistry); Draw(renderer, FontHandle,
                                   text, X,Y,W,H, align) -> lays out via Lur::Text, builds glyph quads
                                   from the atlas, one draw per font; dev overflow warning. Mirrors
                                   LinkStatusBar's shape. Optionally a thin Label helper.

scripts/gen-font.ps1  (NEW)        Runs msdf-atlas-gen on an OFL .ttf -> atlas PNG + JSON; emits
                                   FontAtlas_<Name>.h (embedded atlas bytes + cooked FontMetrics/
                                   GlyphMetrics arrays), mirroring gen-piece-masks.ps1. Commit the OFL
                                   .ttf + its license under Content/Fonts/, plus generated headers.

Games/Chess/View  (WIRE #22)       BoardView hosts a Hud::TextField score line + result banner in the
                                   top margin near the link bar (screen-fixed, ignores board flip).
                                   Data: ChessMatchState::Record() -> ChessRecord{WinsLower,WinsHigher,
                                   Draws}; orient mine/theirs via IsLocalLower(); result banner from
                                   LastResult() (EGameResult); colour via MyColor().
```

Dependency-rule check: `Modules/Text` and `Modules/Hud` never depend on `Games/*` — clean. Layout uses
floats, but it is **render-only** (never sim state), so CLAUDE.md's fixed-point rule does not apply.

## Implementation Order
1. **Cook pipeline + font asset format.** Stand up `msdf-atlas-gen` on dev/CI; write `gen-font.ps1`;
   cook the placeholder UI font (Inter) → `FontAtlas_Inter.h`. Define `Font`/`GlyphMetrics`/
   `FontMetrics`. Host-buildable. Files: `scripts/gen-font.ps1`, `Content/Fonts/`, `Modules/Text/Public/Lur/Text/Font.h`.
2. **`Modules/Text` layout core.** `FontRegistry`, `TextLayout` (greedy wrap + char-break, measure,
   alignment, overflow flags). Pure C++ + **host unit tests** (`build.ps1`) — no GPU needed. Files:
   `Modules/Text/{Public,Private}/...`, `Modules/Text/CMakeLists.txt`, tests.
3. **Render MSDF pipeline.** `Text.vert`/`Text.frag` (median+fwidth+screenPxRange), SPIR-V cook, atlas
   upload, font material kind, dynamic glyph-quad draw path, PushConstants extension. Files:
   `Modules/Render/Private/Vulkan/Shaders/Text.*`, `VulkanBackend.cpp`, `Renderer.h`, `gen-shaders.ps1`.
4. **`Modules/Hud::TextField`.** Layout→glyph-quads→draw; dev overflow warning (red outline + one-shot
   log behind `TextDebugOverflow`); alignment. Files: `Modules/Hud/{Public,Private}/Lur/Hud/TextField.*`.
5. **Wire #22.** Score line + result banner in `BoardView` from `ChessMatchState`, per-perspective,
   screen-fixed. Verify **on both phones** (Android logcat + iOS on-screen) that W/L/D matches each
   player's perspective and updates on match end. Files: `Games/Chess/View/Private/BoardView.cpp`.
6. **Second font (DSEG) — multi-font proof.** Cook DSEG via `gen-font.ps1`; register as a second
   `FontHandle`; smoke-render a sample string. (Actual clock UI = future speed chess.)
7. **Docs.** Amend **CLAUDE.md**: add the `msdf-atlas-gen` sanctioned **build-tool** exception (explicitly
   offline-only, output committed, not linked — contrast MoltenVK). Note font-asset layout + the MSDF
   shader in module docs.

## Expected Results
- Both phones show the same W/L/D from their own perspective, updating on match end (#22 acceptance).
- A short result indicator on checkmate/stalemate/draw before the next match (#22 acceptance).
- Reusable scalable text: any HUD can draw crisp, smooth text at any resolution via a `Hud::TextField`
  + a `FontHandle`; adding/swapping a font = a re-cook. No third-party code at runtime (#22 acceptance).
- DSEG cooked + registered, proving multi-font ahead of speed chess.

## Risks
- **msdf-atlas-gen setup** on Windows dev + macOS/Android CI (via vcpkg or prebuilt release) — one-time
  friction; mitigate by pinning a tool version and documenting the invocation in `gen-font.ps1`.
- **MSDF shader correctness** (screenPxRange scaling, gamma) — validate with one baked glyph at several
  sizes before wiring #22. `fwidth`/derivatives are core Vulkan, fine on the MoltenVK portability subset.
- **Atlas size as an embedded header** — an ASCII MSDF atlas (~256² RGB ≈ 190 KB) is chunky as a C
  header; acceptable (PieceMasks precedent) but consider a raw committed asset if size bothers. Note
  there's no runtime file-IO abstraction yet, so embedding is the path of least resistance.
- **Dynamic VB per frame** — N-buffer per frame-in-flight to avoid CPU/GPU write hazards (UMA on both
  targets makes host-visible buffers cheap; no staging needed).
- **New pipeline vs `MaxMaterials=32`** — two font atlases = two materials; well under the cap, but bump
  it if the text system grows.

## Provenance
Research: `.claude/Documents/research/glyph-renderer.md`. Decisions: this folder's
`scope-and-strategy.md`, `representation.md`, `cook-and-library-exception.md`, `yagni-and-layout.md`.
