#include "Lur/Text/BuiltinFonts.h"

#include "Cooked/FontAtlas_Inter.h"
#include "Cooked/FontAtlas_Dseg7.h"

namespace Lur::Text {

const CookedFont& InterFont() { return Cooked::Inter; }
const CookedFont& Dseg7Font() { return Cooked::Dseg7; }

} // namespace Lur::Text
