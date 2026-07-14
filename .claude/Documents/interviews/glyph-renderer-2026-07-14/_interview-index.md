# Interview: Glyph / Text Renderer for the LurMotorn engine

**Date:** 2026-07-14
**Status:** Complete

## Context

Issue #22 needs an on-screen W/L/D record + match-result HUD in `Games/Chess`, but
there is **no text rendering** in the engine yet — the renderer draws textured/tinted
quads only (`Render/Sprite2D.h` + the piece "tint trick" in `BoardView`). Before #22
can ship we need a minimal **glyph renderer**, and it must land as reusable **engine HUD
infrastructure** (`Modules/Hud`, alongside `LinkStatusBar`) rather than a Chess-only hack.

The goal of this session (per the `research-and-plan` skill): research the glyph-rendering
solution space, interview the user on tradeoffs, and produce an implementation plan.
**No production code is written in this session.**

### Hard engine constraints (from CLAUDE.md)
- No third-party libraries (only OS frameworks + Vulkan/MoltenVK). No FreeType, no stb_truetype,
  no font libs — hand-rolled per project policy.
- One shared pure-C++ core; single Vulkan backend targeting the **portability subset**
  (triangle-list, standard formats, no geometry/tess shaders).
- Slim-payload / low-latency ethos — but note text is rendered locally, it does NOT go on the
  BLE wire, so the wire constraint does not bind here. Memory/atlas size and draw-call count do.
- Cross-platform Android + iPhone; the same cooked assets and C++ must run on both.
- PascalCase naming; Unreal-style Public/Private folder layout; fixed-capacity hot-path containers.

## Themes Discovered
(updated as interview progresses)

## Synergies

**No existing font/glyph/text code anywhere in the repo** — this is greenfield. But three
existing systems are the exact templates to build on:

1. **Tint-trick shader** (`Sprite.frag`): `OutColor = texture(BaseColor, Uv) * InColor`. A
   coverage mask stored as RGBA (RGB=white, alpha=coverage) renders in any color via material
   `Tint`. Glyphs are just silhouettes — same trick applies verbatim.
2. **Piece-mask cook pipeline** (`scripts/gen-piece-masks.ps1` → `PieceMasks.h`): rasterizes SVGs
   (via the `images.weserv.nl` web proxy — no local rasterizer) and emits single-channel coverage
   bytes into an embedded C++ header. A font atlas can be cooked the same way. No runtime decoder.
3. **`LinkStatusBar` widget shape** (`Modules/Hud`): `CreateResources(renderer)` builds meshes/
   materials once; `Draw(renderer, state, X,Y,W,H)` runs each frame inside an existing
   BeginFrame/EndFrame using the pixel-space ortho camera. A text widget mirrors this.

**The decisive constraint — the renderer API.** `IRenderer` exposes exactly one per-primitive
draw: `DrawMesh(MeshHandle, MaterialHandle, Mat4 Model)`. There is **no per-draw UV override and
no per-draw color override** — tint lives on the *material*, UVs are baked into the *mesh*. Plus a
hard `MaxMaterials = 32` cap on the single descriptor pool (BoardView already uses ~15). So to draw
N glyphs, the options are constrained to:
- **(a) Baked-UV mesh per glyph + one shared atlas material** — one texture, one material, N meshes
  (one static unit quad per glyph with its atlas sub-rect UVs baked in); position each via Model.
  Stays under the material cap; needs a small mesh cache.
- **(b) One material per glyph** (chess-piece style) — burns one material per distinct glyph;
  0–9 = 10 materials, fine for digits, but blows the 32-cap for a full font.
- **(c) Extend the renderer** — add per-draw UV (via push constant) or an instanced text path.
  Cleanest long-term, but touches the Vulkan backend + shader + push-constant layout.

**Data source correction** (issue #22 text is slightly off): there is no `Record(Wins…)` accessor —
`ChessMatchState::Record()` returns a `ChessRecord` struct with public fields `WinsLower`,
`WinsHigher`, `Draws` (all `uint8_t`). Orient "mine vs theirs" via `IsLocalLower()`. `LastResult()`
returns `EGameResult { Ongoing, Checkmate, Stalemate, DrawFiftyMove, DrawInsufficientMaterial }` —
winner is inferred from Checkmate + side-to-move, there is no explicit Win/Loss enum.

**Cook constraint:** no local rasterizer and no font lib. Cooking a raster atlas means either the
web-proxy trick (SVG/font → PNG → alpha bytes) or hand-authoring a bitmap font as literal bytes.

## Themes to resolve in interview
1. Glyph representation: hand-authored bitmap font vs cooked raster atlas vs SDF/MSDF.
2. Renderer feed: baked-UV mesh-per-glyph (a) vs material-per-glyph (b) vs API extension (c).
3. Charset scope: digits + `W/L/D/-/:` only (issue #22) vs full ASCII (engine reusability).
4. Cook path: reuse web-proxy pipeline vs hand-authored bytes vs commit a prebuilt atlas.
5. Scaling/quality: crisp at one HUD size vs arbitrary scale (drives SDF-vs-bitmap).

## Decisions (locked)
1. **Charset:** full printable ASCII (95).
2. **Representation:** **MSDF** for all fonts (scalable + always-smooth + sharp corners), cooked offline.
   Fallback if the tool exception had been declined: single-channel SDF + analytic-rectangle clock font.
3. **Cook tool — SANCTIONED BUILD-TOOL EXCEPTION:** `msdf-atlas-gen` (MIT) used ONLY as an offline
   dev-machine/CI cooker; never linked into the app or its CMake build. Commit atlas + cooked metrics.
   Runtime = our own Vulkan MSDF pipeline + hand-written median+fwidth shader. To be documented in
   CLAUDE.md as distinct from MoltenVK (which is a *runtime* dep). Precedent: images.weserv.nl cooks
   piece art.
4. **Fonts:** OFL only (commercial-safe). Default UI font = a simple default (Inter) for now; final
   aesthetic deferred to the HTML whole-game-look prototype → new issue created. Future speed-chess
   clock = OFL **DSEG** cooked through the same pipeline (analytic-rectangle special case dropped).
5. **Scope/sequencing:** build the reusable engine text system + `TextField` (greedy wrap + dev
   overflow warning), ship #22 on it. Cook **two** fonts in v1 (UI + DSEG clock) to prove the
   multi-font seam; the clock *UI* waits for speed chess. **CUT (YAGNI):** kerning application,
   auto-shrink-to-fit, ellipsis truncation.
6. **Architecture:** one-atlas-per-font; `Font` asset {em-normalized metrics + glyph table + atlas}
   owned by a `FontRegistry` (→ `FontHandle`); separate immutable `Font` from computed `LayoutResult`.
   Greedy word-wrap + char-break fallback; layout in em, snap to px at emission only.
7. **Renderer feed:** dynamic per-draw vertex buffer with baked per-vertex UVs (ImGui pattern) + new
   MSDF pipeline bound to the font atlas; one draw per font. (Vertex already carries Uv + Color.)

## Files Created
- `_interview-index.md` (this file)
- `scope-and-strategy.md` — charset, scalable requirement, multi-font + TextField scope
- `representation.md` — distance-field family; per-font corner strategy
- `cook-and-library-exception.md` — msdf-atlas-gen offline-tool boundary; OFL fonts; verified facts
- `../../research/glyph-renderer.md` — full option comparison + gap research (shared research doc)
