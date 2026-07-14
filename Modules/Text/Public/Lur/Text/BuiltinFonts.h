#pragma once
#include "Lur/Text/CookedFont.h"

namespace Lur::Text {

// The built-in placeholder UI font: Inter (OFL), cooked to an MSDF atlas at build
// time by scripts/gen-font.ps1. The aesthetic is provisional — the final whole-game
// look (and hence the shipping font) is decided in issue #23; swapping it is just a
// re-cook.
const CookedFont& InterFont();

// DSEG7 Classic (OFL): a 7-segment digital display face for future speed-chess clocks.
// Cooked through the same MSDF pipeline (#28) — proves the multi-font seam. Covers the
// digits/symbols a segmented display has, not full ASCII.
const CookedFont& Dseg7Font();

} // namespace Lur::Text
