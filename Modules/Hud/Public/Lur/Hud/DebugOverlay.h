#pragma once
#include <cstdint>

#include "Lur/Hud/TextField.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Renderer.h"
#include "Lur/Text/Font.h"

namespace Lur::Hud {

// A toggleable heads-up debug overlay (roadmap Phase 0.5, Review #2 §4.5): frame time,
// link state, time since the last datagram, and send/recv counters — top-left, over the
// GUI layer. "If you can't see it, you can't fix it": this replaces logcat squinting for
// ~80% of on-device questions. Engine-level and game-agnostic; desktop + phones alike.
struct DebugStats {
    float                FrameMs = 0.0f;
    Lur::Net::ELinkState Link = Lur::Net::ELinkState::Searching;
    uint64_t             NsSinceRecv = 0;
    uint32_t             Sent = 0;
    uint32_t             Recv = 0;
    const char*          PeerShort = "";  // short peer id, or "" when unlinked
};

class DebugOverlay {
public:
    // Uploads the built-in UI font atlas + binds the text field. Call once when the
    // renderer is up.
    void CreateResources(Lur::Render::IRenderer* Renderer);

    // Draw the stats top-left. Call inside BeginGui()..EndFrame (pixel space).
    void Draw(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx,
              const DebugStats& Stats) const;

private:
    Lur::Text::Font Font;
    TextField       Text;
    bool            Ready = false;
};

}  // namespace Lur::Hud
