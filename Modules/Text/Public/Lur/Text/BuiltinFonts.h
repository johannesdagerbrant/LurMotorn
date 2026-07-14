#pragma once
#include "Lur/Text/CookedFont.h"

namespace Lur::Text {

// The built-in placeholder UI font: Inter (OFL), cooked to an MSDF atlas at build
// time by scripts/gen-font.ps1. The aesthetic is provisional — the final whole-game
// look (and hence the shipping font) is decided in issue #23; swapping it is just a
// re-cook. Additional fonts (e.g. the DSEG clock font, #28) register alongside this.
const CookedFont& InterFont();

} // namespace Lur::Text
