#pragma once
// Lur::Hud::ShortGuid — a device GUID rendered as something a human can read off a screen and
// compare against another screen. Lives here, next to Dropdown, because it exists to LABEL a
// dropdown row and both games need the identical rendering: two phones showing the same peer
// must show the same string, or the label is worse than useless.
//
// Was an anonymous-namespace helper inside Chess's BoardView. RPS needed it too (its linked-
// opponent row shows the peer's GUID rather than the generic "Linked opponent"), and
// Games/* cannot depend on Games/*, so the choice was to copy it or to lift it. Copied
// formatting drifts — one game gains a separator or a case change and the two stop matching —
// and the whole point is cross-device comparison.
#include <cstddef>
#include <string>

namespace Lur::Hud {

// First 12 hex of a GUID as three upper-case groups: "7F3A-C9E1-04B2".
//
// ASCII '-' as the separator ON PURPOSE: the MSDF atlas is cooked from the glyphs we ship, so a
// middot or an en dash is not guaranteed to be in it and would render as a hole. Short strings
// are padded with '0' rather than truncated to whatever arrived, so the label is always the same
// width and a column of them stays a column.
inline std::string ShortGuid(const std::string& G) {
    auto Up = [](char C) { return (C >= 'a' && C <= 'f') ? static_cast<char>(C - 32) : C; };
    std::string S;
    for (int Grp = 0; Grp < 3; ++Grp) {
        if (Grp) S += '-';
        for (int K = 0; K < 4; ++K) {
            const std::size_t Idx = static_cast<std::size_t>(Grp) * 4 + K;
            S += (Idx < G.size()) ? Up(G[Idx]) : '0';
        }
    }
    return S;
}

}  // namespace Lur::Hud
