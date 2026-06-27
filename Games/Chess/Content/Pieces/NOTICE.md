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

These `.svg` files are the source of truth. The app does **not** load SVG or PNG
at runtime: a host step rasterizes the white variants and extracts single-channel
silhouette coverage masks, embedded as raw bytes. Piece colour (white vs black)
comes from the material tint at draw time, so we ship 6 masks, not 12 textures.
See `scripts/gen-piece-masks.ps1` and `Games/Chess/Android/app/src/main/cpp/PieceMasks.h`.
