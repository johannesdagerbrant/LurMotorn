#pragma once
// BeginDevGuiLayer — the ONE place the dev-GUI render pass is entered, wrapped so it
// vanishes in shipping WITHOUT a call-site branch (#113, spec Addendum A.5). Host frame
// composers (DesktopMain, RpsMain) call this between the game's GUI submission and
// EndFrame; in a shipping build it compiles to nothing, so the frame issues exactly the
// world + game-GUI passes and no dev-layer symbols are reachable. This keeps the §0
// contract — no `#if` in game/sim code; the single guard lives here.
#include "Lur/Render/Renderer.h"

namespace Lur::Render {

inline void BeginDevGuiLayer(IRenderer* R) {
#if !LUR_SHIPPING
    if (R) R->BeginDevGui();
#else
    (void)R;
#endif
}

}  // namespace Lur::Render
