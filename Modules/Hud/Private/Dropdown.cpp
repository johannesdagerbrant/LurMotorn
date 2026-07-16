#include "Lur/Hud/Dropdown.h"

#include <cmath>
#include <cstring>

#include "Lur/Render/Sprite2D.h"

namespace Lur::Hud {

using Lur::Math::Mat4;
using Lur::Render::Color;
using Lur::Render::MaterialDesc;
using Lur::Render::MeshHandle;
using Lur::Render::Vertex;

namespace {

// ---- Widget chrome palette (dark-glass, matches the HTML prototype) --------------
constexpr Color PanelCol   {0.11f, 0.12f, 0.14f, 0.97f};
constexpr Color LineCol    {0.21f, 0.23f, 0.26f, 1.00f};
constexpr Color AccentCol  {0.86f, 0.86f, 0.92f, 1.00f};  // selected-row left bar
constexpr Color TitleCol   {0.62f, 0.60f, 0.55f, 1.00f};
constexpr Color LabelCol   {0.93f, 0.90f, 0.85f, 1.00f};
constexpr Color LabelSelCol{1.00f, 1.00f, 1.00f, 1.00f};
constexpr Color SubCol     {0.60f, 0.58f, 0.53f, 1.00f};
constexpr Color HeaderCol  {0.46f, 0.45f, 0.41f, 1.00f};
constexpr Color ChevronCol {0.60f, 0.58f, 0.53f, 1.00f};
constexpr Color SplitLeft  {0.93f, 0.91f, 0.86f, 1.00f};  // "this device" light half
constexpr Color SplitRight {0.16f, 0.17f, 0.20f, 1.00f};  // "both sides" dark half

constexpr int DiscSegments = 24;
constexpr int MaxRows       = 10;   // capped; no scrolling (renderer has no scissor)

uint32_t PackRgba(Color C) {
    auto B = [](float V) {
        int I = static_cast<int>(V * 255.0f + 0.5f);
        if (I < 0) I = 0; else if (I > 255) I = 255;
        return static_cast<uint32_t>(I);
    };
    return (B(C.R) << 24) | (B(C.G) << 16) | (B(C.B) << 8) | B(C.A);
}

Vertex Vtx(float X, float Y, Color C) {
    return Vertex{{X, Y, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {C.R, C.G, C.B, C.A}};
}

// A unit quad (0,0)-(1,1) with one baked colour. Scaled to any rect at draw time.
MeshHandle MakeQuadMesh(Lur::Render::IRenderer* R, Color C) {
    const Vertex V[4] = {Vtx(0, 0, C), Vtx(0, 1, C), Vtx(1, 1, C), Vtx(1, 0, C)};
    const uint32_t I[6] = {0, 1, 2, 0, 2, 3};
    return R->CreateMesh(V, 4, I, 6);
}

// A filled disc centred in the unit box (radius 0.5), triangle-list (a fan expressed
// as a list — no triangle fans, per the portability subset).
MeshHandle MakeDiscMesh(Lur::Render::IRenderer* R, Color C) {
    std::vector<Vertex>   V;
    std::vector<uint32_t> I;
    V.push_back(Vtx(0.5f, 0.5f, C));   // centre = index 0
    for (int i = 0; i <= DiscSegments; ++i) {
        const float A = 6.2831853f * static_cast<float>(i) / DiscSegments;
        V.push_back(Vtx(0.5f + 0.5f * std::cos(A), 0.5f + 0.5f * std::sin(A), C));
    }
    for (int i = 0; i < DiscSegments; ++i) {
        I.push_back(0);
        I.push_back(static_cast<uint32_t>(1 + i));
        I.push_back(static_cast<uint32_t>(2 + i));
    }
    return R->CreateMesh(V.data(), static_cast<uint32_t>(V.size()),
                         I.data(), static_cast<uint32_t>(I.size()));
}

// A ring/annulus in the unit box (outer radius 0.5, inner 0.34), triangle-list.
MeshHandle MakeRingMesh(Lur::Render::IRenderer* R, Color C) {
    constexpr float Outer = 0.5f, Inner = 0.34f;
    std::vector<Vertex>   V;
    std::vector<uint32_t> I;
    for (int i = 0; i <= DiscSegments; ++i) {
        const float A = 6.2831853f * static_cast<float>(i) / DiscSegments;
        const float Cx = std::cos(A), Sy = std::sin(A);
        V.push_back(Vtx(0.5f + Outer * Cx, 0.5f + Outer * Sy, C));  // 2i   outer
        V.push_back(Vtx(0.5f + Inner * Cx, 0.5f + Inner * Sy, C));  // 2i+1 inner
    }
    for (int i = 0; i < DiscSegments; ++i) {
        const uint32_t O0 = 2 * i, I0 = 2 * i + 1, O1 = 2 * i + 2, I1 = 2 * i + 3;
        I.push_back(O0); I.push_back(I0); I.push_back(O1);
        I.push_back(I0); I.push_back(I1); I.push_back(O1);
    }
    return R->CreateMesh(V.data(), static_cast<uint32_t>(V.size()),
                         I.data(), static_cast<uint32_t>(I.size()));
}

// Two solid half-discs (hard vertical seam) — the "same device" marker.
MeshHandle MakeSplitDiscMesh(Lur::Render::IRenderer* R) {
    std::vector<Vertex>   V;
    std::vector<uint32_t> I;
    auto Half = [&](float A0, float A1, Color C) {
        const uint32_t Base = static_cast<uint32_t>(V.size());
        V.push_back(Vtx(0.5f, 0.5f, C));
        for (int i = 0; i <= DiscSegments; ++i) {
            const float A = A0 + (A1 - A0) * static_cast<float>(i) / DiscSegments;
            V.push_back(Vtx(0.5f + 0.5f * std::cos(A), 0.5f + 0.5f * std::sin(A), C));
        }
        for (int i = 0; i < DiscSegments; ++i) {
            I.push_back(Base);
            I.push_back(Base + 1 + i);
            I.push_back(Base + 2 + i);
        }
    };
    Half(1.5707963f, 4.7123890f, SplitLeft);    //  90° .. 270°  (left)
    Half(-1.5707963f, 1.5707963f, SplitRight);  // -90° ..  90°  (right)
    return R->CreateMesh(V.data(), static_cast<uint32_t>(V.size()),
                         I.data(), static_cast<uint32_t>(I.size()));
}

// A chevron triangle in the unit box, pointing Down or Up.
MeshHandle MakeChevronMesh(Lur::Render::IRenderer* R, bool Down) {
    const float Yt = Down ? 0.36f : 0.64f;
    const float Yp = Down ? 0.66f : 0.34f;
    const Vertex V[3] = {Vtx(0.18f, Yt, ChevronCol), Vtx(0.82f, Yt, ChevronCol),
                         Vtx(0.50f, Yp, ChevronCol)};
    const uint32_t I[3] = {0, 1, 2};
    return R->CreateMesh(V, 3, I, 3);
}

}  // namespace

void Dropdown::CreateResources(Lur::Render::IRenderer* Renderer, const Lur::Text::Font* Font) {
    Rend = Renderer;
    Text.CreateResources(Renderer, Font);
    White = Renderer->CreateMaterial(MaterialDesc{0, Color{1, 1, 1, 1}, false});

    QuadPanel   = MakeQuadMesh(Renderer, PanelCol);
    QuadLine    = MakeQuadMesh(Renderer, LineCol);
    QuadAccent  = MakeQuadMesh(Renderer, AccentCol);
    SplitDisc   = MakeSplitDiscMesh(Renderer);
    ChevronDown = MakeChevronMesh(Renderer, true);
    ChevronUp   = MakeChevronMesh(Renderer, false);
}

MeshHandle Dropdown::MeshFor(Color C, bool RingShape) {
    const uint32_t Key = PackRgba(C) ^ (RingShape ? 0x80000000u : 0u);
    auto It = ColorMesh.find(Key);
    if (It != ColorMesh.end()) return It->second;
    const MeshHandle M = RingShape ? MakeRingMesh(Rend, C) : MakeDiscMesh(Rend, C);
    ColorMesh.emplace(Key, M);
    return M;
}

void Dropdown::SetItems(const DropdownItem* NewItems, int Count) {
    Items.assign(NewItems, NewItems + Count);
    // Warm the colour caches now (outside the frame) so Draw allocates nothing.
    for (const DropdownItem& It : Items) {
        if (It.Header || It.Lead == ELeadStyle::None) continue;
        if (It.Lead == ELeadStyle::Dot) {
            MeshFor(It.LeadFill, false);
            if (It.Ring) MeshFor(It.RingColor, true);
        }
    }
    if (SelectedIdx >= Count) SelectedIdx = 0;
}

void Dropdown::SetSelected(int Index) {
    if (Index >= 0 && Index < static_cast<int>(Items.size())) SelectedIdx = Index;
}

bool Dropdown::TookSelection() {
    const bool Was = SelectionLatch;
    SelectionLatch = false;
    return Was;
}

void Dropdown::DrawBox(Lur::Render::IRenderer* R, MeshHandle M,
                       float X, float Y, float W, float H) const {
    if (M == 0 || W <= 0.0f || H <= 0.0f) return;
    R->DrawMesh(M, White, Mat4::Translation({X, Y, 0.0f}) * Mat4::Scale({W, H, 1.0f}));
}

// Draw one item's marker + label + sublabel inside a rect (used for both the pill and
// menu rows). Assumes the row background is already drawn.
void Dropdown::DrawRowContent(Lur::Render::IRenderer* R, int ItemIndex,
                              float X, float Y, float W, float RowH, bool IsSelected) {
    const DropdownItem& It = Items[ItemIndex];
    const float Pad = RowH * 0.30f;
    const float Dot = RowH * 0.50f;
    const float DotY = Y + (RowH - Dot) * 0.5f;

    if (It.Lead == ELeadStyle::Split) {
        DrawBox(R, SplitDisc, X + Pad, DotY, Dot, Dot);
    } else if (It.Lead == ELeadStyle::Dot) {
        DrawBox(R, MeshFor(It.LeadFill, false), X + Pad, DotY, Dot, Dot);
        if (It.Ring) {
            const float RS = Dot * 1.5f;   // ring sits around the dot with a gap
            DrawBox(R, MeshFor(It.RingColor, true),
                    X + Pad - (RS - Dot) * 0.5f, DotY - (RS - Dot) * 0.5f, RS, RS);
        }
    }

    const float TextX = X + Pad + Dot + Pad;
    const float TextW = W - (TextX - X) - Pad;
    const bool  HasSub = !It.Sublabel.empty();
    const Color LCol = IsSelected ? LabelSelCol : LabelCol;
    using Lur::Text::EHAlign;
    using Lur::Text::EVAlign;
    if (HasSub) {
        Text.Draw(R, It.Label.c_str(), TextX, Y + RowH * 0.12f, TextW, RowH * 0.46f,
                  RowH * 0.40f, LCol, EHAlign::Left, EVAlign::Middle, false);
        Text.Draw(R, It.Sublabel.c_str(), TextX, Y + RowH * 0.52f, TextW, RowH * 0.40f,
                  RowH * 0.26f, SubCol, EHAlign::Left, EVAlign::Middle, false);
    } else {
        Text.Draw(R, It.Label.c_str(), TextX, Y, TextW, RowH,
                  RowH * 0.40f, LCol, EHAlign::Left, EVAlign::Middle, false);
    }
}

void Dropdown::Draw(Lur::Render::IRenderer* Renderer, const char* Title,
                    float X, float Y, float W, float PillH) {
    RowRects.clear();
    RowItem.clear();
    if (Items.empty()) return;

    const float TitleH = PillH * 0.52f;
    const float Gap    = PillH * 0.12f;
    using Lur::Text::EHAlign;
    using Lur::Text::EVAlign;

    // Caption above the pill.
    if (Title != nullptr) {
        Text.Draw(Renderer, Title, X + PillH * 0.10f, Y, W, TitleH, PillH * 0.30f,
                  TitleCol, EHAlign::Left, EVAlign::Middle, false);
    }

    // Collapsed pill: panel + border + the current selection's content + chevron.
    const float PillY = Y + TitleH + Gap;
    PillRect = {X, PillY, X + W, PillY + PillH};
    DrawBox(Renderer, QuadPanel, X, PillY, W, PillH);
    const float T = PillH * 0.05f;                     // border thickness
    DrawBox(Renderer, QuadLine, X, PillY, W, T);
    DrawBox(Renderer, QuadLine, X, PillY + PillH - T, W, T);
    DrawBox(Renderer, QuadLine, X, PillY, T, PillH);
    DrawBox(Renderer, QuadLine, X + W - T, PillY, T, PillH);

    if (SelectedIdx >= 0 && SelectedIdx < static_cast<int>(Items.size()))
        DrawRowContent(Renderer, SelectedIdx, X, PillY, W - PillH, PillH, false);

    const float Ch = PillH * 0.5f;
    DrawBox(Renderer, Open ? ChevronUp : ChevronDown,
            X + W - PillH * 0.75f, PillY + (PillH - Ch) * 0.5f, Ch, Ch);

    if (!Open) return;

    // Open menu: measure total height (capped), draw the panel, then the rows.
    const float MenuY = PillY + PillH + Gap;
    const float RowH = PillH, HeaderH = PillH * 0.62f;
    const int   N = static_cast<int>(Items.size()) < MaxRows
                        ? static_cast<int>(Items.size()) : MaxRows;
    float Total = 0.0f;
    for (int i = 0; i < N; ++i) Total += Items[i].Header ? HeaderH : RowH;

    DrawBox(Renderer, QuadPanel, X, MenuY, W, Total);

    float Cy = MenuY;
    for (int i = 0; i < N; ++i) {
        const DropdownItem& It = Items[i];
        if (It.Header) {
            if (i > 0) DrawBox(Renderer, QuadLine, X, Cy, W, PillH * 0.03f);  // divider
            Text.Draw(Renderer, It.Label.c_str(), X + PillH * 0.30f, Cy, W, HeaderH,
                      PillH * 0.26f, HeaderCol, EHAlign::Left, EVAlign::Middle, false);
            Cy += HeaderH;
            continue;
        }
        const bool Sel = (i == SelectedIdx);
        if (Sel) DrawBox(Renderer, QuadAccent, X, Cy + RowH * 0.18f,
                         PillH * 0.07f, RowH * 0.64f);       // left accent bar
        DrawRowContent(Renderer, i, X, Cy, W, RowH, Sel);
        RowRects.push_back({X, Cy, X + W, Cy + RowH});
        RowItem.push_back(i);
        Cy += RowH;
    }

    // Menu border.
    DrawBox(Renderer, QuadLine, X, MenuY, W, T);
    DrawBox(Renderer, QuadLine, X, MenuY + Total - T, W, T);
    DrawBox(Renderer, QuadLine, X, MenuY, T, Total);
    DrawBox(Renderer, QuadLine, X + W - T, MenuY, T, Total);
}

bool Dropdown::OnTap(float XPx, float YPx) {
    auto In = [](const RectPx& R, float Px, float Py) {
        return Px >= R.X0 && Px < R.X1 && Py >= R.Y0 && Py < R.Y1;
    };

    if (Open) {
        for (std::size_t i = 0; i < RowRects.size(); ++i) {
            if (In(RowRects[i], XPx, YPx)) {
                SelectedIdx = RowItem[i];
                SelectionLatch = true;
                Open = false;
                return true;
            }
        }
        Open = false;      // tap outside any row (incl. the pill) closes the menu
        return true;       // consumed either way while open
    }

    if (In(PillRect, XPx, YPx)) { Open = true; return true; }
    return false;          // closed + missed → let the tap through to the game
}

} // namespace Lur::Hud
