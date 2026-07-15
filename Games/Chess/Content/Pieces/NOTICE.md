# Chess piece art — provenance & license

Source set: **rhosgfx** chess pieces by **RhosGFX** (https://rhosgfx.itch.io/).

License: **CC0 1.0 Universal (Public Domain Dedication)** — no attribution
required, no ShareAlike, unrestricted commercial use (safe to ship on Google
Play and the Apple App Store).

Obtained from the lichess `lila` repository, whose `COPYING.md` documents this
set's license:
- Files: https://github.com/lichess-org/lila/tree/master/public/piece/rhosgfx
- License manifest: https://github.com/lichess-org/lila/blob/master/COPYING.md
  (`public/piece/rhosgfx | RhosGFX | CC0 1.0`)

The only committed art is the **6 `w*.png` masks** (the white pieces, square
grayscale+alpha, one shared resolution) — that is the whole content set. There are
NO black-piece files and NO committed SVGs: `scripts/gen-piece-pngs.py` re-authors
the PNGs on demand by fetching the upstream rhosgfx SVGs straight from the lichess
URL above, so the vector source lives there, not in this repo. The cook then reads
**only** the local PNGs (never the network) and asserts they honour the convention.

The app does **not** load SVG or PNG at runtime: the cook extracts two single-byte
channels per piece — **coverage** (silhouette alpha) and **shade** (the art's
tonal luminance) — embedded as raw bytes and uploaded as an R8G8 texture. BOTH
piece colours come from those 6 masks + the material tint at draw time: the shader
keeps the fills as tint×shade (so the form's shading survives) and flips the dark
ink band to the tint's complement (so black pieces get white outlines) — one mask,
two tints, no per-colour art (issue #30). See `scripts/gen-piece-pngs.py` (author),
`scripts/gen-piece-masks.ps1` (cook), `Games/Chess/View/Private/PieceMasks.h` and
the `Sprite.frag` tint logic.
