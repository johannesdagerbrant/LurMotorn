# Cook pipeline & the library-exception question

**Date:** 2026-07-14 · Interview round 3 (in progress)

## What the user asked
"If a public-domain/open-source well-established MSDF [tool] with a rich compatible library of
free-for-commercial fonts exists, maybe that would be a good exception to the no-external-libs rule."
User is not interested in text-gen internals; wants it to **work well**.

## Verified facts (2026-07-14)
- **`msdf-atlas-gen`** (Viktor Chlumský): **MIT license**, **standalone offline CLI tool**. Emits an
  atlas image (PNG/BMP/TIFF/raw/…) + a **metrics file** (JSON: atlas settings, font metrics, per-glyph
  plane/atlas bounds, advances, kerning; also CSV/Artery). Depends on **FreeType + msdfgen** — but
  those live ONLY inside the offline tool. Source: github.com/Chlumsky/msdf-atlas-gen.
- **Runtime links NOTHING of it.** Rendering = sample the committed atlas with a shader we write
  (~10-line median-of-3 + fwidth AA). Confirmed by the tool's own docs ("you only need to sample the
  generated atlas texture with a shader").
- **OFL (SIL Open Font License 1.1)** = commercial use OK, embed in a program with **no bundled-license
  obligation** (only redistributing the font *itself* requires shipping the license). Vast OFL library
  (Google Fonts). **DSEG** (7-/14-segment display font) is OFL → the future speed-chess clock is solved
  by the SAME pipeline; the analytic-rectangle special case becomes unnecessary.

## Why this is barely an "exception"
The "no third-party libraries" rule protects the **engine + shipped app + its build**. msdf-atlas-gen is
a **build-time asset cooker** — same category as `images.weserv.nl`, already used to cook piece art (a
*network* call; a local offline binary is strictly more conservative). Nothing third-party enters the
shipped binary or the app's CMake build. Runtime stays 100% ours.

## Proposed boundary (pending explicit user sign-off)
- ADOPT `msdf-atlas-gen` (+ its msdfgen/FreeType deps) **as an offline authoring/cook tool only**,
  invoked on a dev machine or CI, NOT linked into the app and NOT part of the app's CMake build.
- COMMIT its outputs as engine assets: the MSDF atlas (as embedded bytes, PieceMasks-style, or a raw
  asset) + a cooked `FontMetrics`/`GlyphMetrics` header derived from the JSON.
- RUNTIME: our own Vulkan MSDF pipeline + hand-written median+fwidth shader; zero third-party code.
- FONTS: OFL only (commercial-safe). Default UI font TBD; DSEG reserved for the future clock.
- This is a per-font one-time cook; adding/swapping a font = re-run the tool, commit new assets.

## Decided
- Representation: **MSDF** (via the offline tool) for all fonts — supersedes the earlier
  single-channel-SDF + analytic-rectangle split (kept as the fallback if the exception is declined).
- Scope/sequencing (round 2): build reusable font system + one UI font + TextField(wrap/overflow), ship
  #22 on it; **defer the digital/clock font** (now trivially an OFL DSEG cook when speed chess lands).

## Open (this round): explicit confirmation of the exception + its documentation, and default font pick.
