#pragma once
#include "Lur/Hud/TextField.h"
#include "Lur/Render/Renderer.h"
#include "Lur/Text/Font.h"
#include "Rps/Snapshot.h"

namespace Rps {

// The RPS presentation layer — draws one Snapshot to an IRenderer. Talks only to the
// renderer interface (no Vulkan), so it builds on the host too and is shared by the
// desktop and (later) the phone mains. Mirrors chess's BoardView in spirit.
//
// This is the BRING-UP renderer: one DrawMesh per unit, positions interpolated on the
// CPU (Prev->Pos by alpha). It is deliberately non-instanced — proving the whole
// window -> renderer -> SimRunner -> snapshot pipeline end-to-end on screen first. The
// design's one-instanced-draw + shader mix(prev,curr,alpha) replaces this inner loop
// later (a renderer extension), the same brute-force-then-optimise discipline the
// spatial grid used.
class GameView {
public:
    void CreateResources(Lur::Render::IRenderer* Renderer);

    // Draw the field + units + HUD for this snapshot. CameraY is the world-Y at the
    // bottom of the screen (the swipe scroll position); Alpha in [0,1] interpolates
    // Prev->Pos. Owns the full BeginFrame..EndFrame. Non-const: fills the instance
    // scratch buffer each frame (units draw as ONE instanced call, interpolated in the
    // vertex shader).
    void Render(Lur::Render::IRenderer* Renderer, const Snapshot& Snap, float Alpha,
                float CameraY, float WidthPx, float HeightPx);

    // World units visible vertically at this width — for the caller's camera clamp.
    static float VisibleWorldHeight(float WidthPx, float HeightPx);

private:
    Lur::Render::MeshHandle Quad = 0;  // one white unit quad; materials supply colour

    // Flat-colour materials (BaseColor 0 = white, Tint = the colour).
    Lur::Render::MaterialHandle Background = 0;
    Lur::Render::MaterialHandle CampMat[2] = {};
    Lur::Render::MaterialHandle TreeMat = 0;
    Lur::Render::MaterialHandle HealthBg = 0;
    Lur::Render::MaterialHandle HealthFg = 0;

    Lur::Render::Color UnitColor[4][2] = {};          // [type][team] — per-instance tint
    Lur::Render::InstanceData Instances[MaxUnits];    // per-frame scratch (one instanced draw)

    Lur::Text::Font Font;
    Lur::Hud::TextField Text;
    bool Ready = false;
};

}  // namespace Rps
