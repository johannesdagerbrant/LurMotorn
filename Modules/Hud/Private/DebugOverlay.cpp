#include "Lur/Hud/DebugOverlay.h"

#include <cstdio>

#include "Lur/Text/BuiltinFonts.h"

namespace Lur::Hud {

namespace {
const char* LinkStr(Lur::Net::ELinkState S) {
    switch (S) {
        case Lur::Net::ELinkState::Searching:       return "searching";
        case Lur::Net::ELinkState::Handshaking:     return "handshaking";
        case Lur::Net::ELinkState::Linked:          return "linked";
        case Lur::Net::ELinkState::Disconnected:    return "disconnected";
        case Lur::Net::ELinkState::VersionMismatch: return "ver-mismatch";
    }
    return "?";
}
}  // namespace

void DebugOverlay::CreateResources(Lur::Render::IRenderer* Renderer) {
    Font.Init(Lur::Text::InterFont());
    Font.UploadAtlas(*Renderer);
    Text.CreateResources(Renderer, &Font);
    Ready = true;
}

void DebugOverlay::Draw(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx,
                        const DebugStats& Stats) const {
    if (!Ready) return;
    (void)HeightPx;

    char Lines[5][64];
    std::snprintf(Lines[0], sizeof(Lines[0]), "frame %.1f ms", Stats.FrameMs);
    std::snprintf(Lines[1], sizeof(Lines[1]), "link  %s", LinkStr(Stats.Link));
    std::snprintf(Lines[2], sizeof(Lines[2]), "rx    %llu ms ago",
                  static_cast<unsigned long long>(Stats.NsSinceRecv / 1000000ull));
    std::snprintf(Lines[3], sizeof(Lines[3]), "tx %u  rx %u", Stats.Sent, Stats.Recv);
    std::snprintf(Lines[4], sizeof(Lines[4]), "peer %s",
                  (Stats.PeerShort != nullptr && Stats.PeerShort[0] != '\0') ? Stats.PeerShort : "-");

    const float Size = 15.0f;
    const float LineH = Size * 1.35f;
    const float X = 8.0f;
    const Lur::Render::Color Shadow{0.0f, 0.0f, 0.0f, 0.85f};
    const Lur::Render::Color Ink{0.35f, 1.0f, 0.45f, 1.0f};  // terminal-green, reads on both squares

    for (int i = 0; i < 5; ++i) {
        const float Y = 6.0f + LineH * static_cast<float>(i);
        // Shadow then ink (1px offset) so it reads on light and dark squares alike.
        Text.Draw(Renderer, Lines[i], X + 1.0f, Y + 1.0f, WidthPx, LineH, Size, Shadow,
                  Lur::Text::EHAlign::Left, Lur::Text::EVAlign::Top, false);
        Text.Draw(Renderer, Lines[i], X, Y, WidthPx, LineH, Size, Ink,
                  Lur::Text::EHAlign::Left, Lur::Text::EVAlign::Top, false);
    }
}

}  // namespace Lur::Hud
