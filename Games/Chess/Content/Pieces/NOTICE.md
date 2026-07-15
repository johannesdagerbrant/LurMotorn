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

The `.svg` files are the upstream vector source. The prepped, cook-ready content
is the committed `w*.png` set (square RGBA, one shared resolution) — authored once
from the SVGs by `scripts/gen-piece-pngs.py`. The cook reads **only** those local
PNGs (never the network) and asserts they honour the convention.

The app does **not** load SVG or PNG at runtime: the cook extracts two single-byte
channels per piece — **coverage** (silhouette alpha) and **shade** (the art's
tonal luminance) — embedded as raw bytes and uploaded as an R8G8 texture. Piece
colour (white vs black) comes from the material tint at draw time, which multiplies
the shade, so we ship 6 mask pairs (not 12 textures) yet keep the source art's
tones instead of a flat blob (issue #30). See `scripts/gen-piece-pngs.py` (author),
`scripts/gen-piece-masks.ps1` (cook) and `Games/Chess/View/Private/PieceMasks.h`.
