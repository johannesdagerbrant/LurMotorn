#include "Rps/GameView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Lur/DevGui/CategoryTree.h"  // #121: hierarchical (|-nested) category tree
#include "Lur/DevGui/Popover.h"       // #121/#129: below-or-above anchored placement
#include "Lur/Math/Mat4.h"
#include "Lur/Render/DevGuiLayer.h"  // #113: BeginDevGuiLayer (shipping-guarded dev pass)
#include "Lur/Render/Sprite2D.h"
#include "Lur/Text/BuiltinFonts.h"
#include "Rps/Tunables.h"

// The design-lock glyph set (#85, Docs/Journal/2026-07-19/rps-hud-prototype.html): indices
// 0..3 are EUnit order (miner, rock, paper, scissors), then gold / mine / swords /
// camp. Sources: game-icons.net (CC BY 3.0) + Font Awesome Free (CC BY 4.0) + the
// custom bold pick (ours) — attribution required in-app before shipping (#85).
// LUR_COOK rg8-shade-coverage src=Icons/miner.png,Icons/rock.png,Icons/paper.png,Icons/scissors.png,Icons/gold.png,Icons/mine.png,Icons/swords.png,Icons/camp.png,Icons/pointer.png,Icons/oreload.png out=View/Private/IconMasks.h ns=RpsArt size=IconSize coverage=IconCoverage shade=IconShade
#include "IconMasks.h"

namespace Rps {
namespace {

using Lur::Render::Color;
using Lur::Render::IRenderer;
using Lur::Render::MaterialDesc;
using Lur::Math::Mat4;

// Fixed -> float, VIEW SIDE ONLY. The sim never sees a float; the renderer is where
// they're allowed (positions become pixels, interpolation is a lerp).
float FW(Fixed F) { return static_cast<float>(F.Raw) / static_cast<float>(Fixed::One); }
float Ppu(float WidthPx) { return WidthPx / FW(WorldWidth); }  // pixels per world unit (fill width)

// HUD metrics scale with the framebuffer width (baseline = the 360 px desktop window),
// like the prototype's viewport-relative units — a 1080-wide phone gets 3x text/icons.
float HudScale(float WidthPx) {
    const float S = WidthPx / 360.0f;
    return S < 1.0f ? 1.0f : S;
}

Lur::Render::MaterialHandle FlatMat(IRenderer* R, Color C) {
    MaterialDesc D;
    D.BaseColor = 0;  // flat white
    D.Tint = C;
    return R->CreateMaterial(D);
}

const char* ResultStr(uint8_t R, int MyTeam) {
    if (R == ResultDraw) return "DRAW";
    if (R == ResultTeam0Wins) return MyTeam == 0 ? "YOU WIN" : "YOU LOSE";
    if (R == ResultTeam1Wins) return MyTeam == 1 ? "YOU WIN" : "YOU LOSE";
    return "";
}

// ---- Locked palette (#85, Docs/Journal/2026-07-19/rps-hud-prototype.html) ----
constexpr float Srgb(int V) { return static_cast<float>(V) / 255.0f; }
struct GradStop { float P; Color C; };
// Field gradient — SCREENSPACE vertical: night-blue enemy horizon (top) through
// dark earth to the warm umber home ground (bottom). Both players see the same
// grade because both see the enemy at the top (per-player FlipY).
constexpr GradStop FieldStops[] = {
    {0.000f, {Srgb(0x12), Srgb(0x22), Srgb(0x31), 1.0f}},
    {0.179f, {Srgb(0x11), Srgb(0x1B), Srgb(0x15), 1.0f}},
    {0.550f, {Srgb(0x10), Srgb(0x17), Srgb(0x07), 1.0f}},
    {0.795f, {Srgb(0x16), Srgb(0x1A), Srgb(0x09), 1.0f}},
    {1.000f, {Srgb(0x2E), Srgb(0x27), Srgb(0x0F), 1.0f}},
};
constexpr int NumFieldStops = 5;
// Grid colour gradient (screenspace) — lines are world-anchored, colour is not.
constexpr GradStop GridStops[] = {
    {0.0f, {Srgb(0x26), Srgb(0x30), Srgb(0x3B), 1.0f}},
    {1.0f, {Srgb(0x2E), Srgb(0x36), Srgb(0x27), 1.0f}},
};
constexpr float GridStepWu = 4.0f;   // line spacing, world units
constexpr float GridAlpha = 0.55f;   // keep the lines a subtle overlay

Color GradSample(const GradStop* S, int N, float T) {
    if (T <= S[0].P) return S[0].C;
    for (int I = 1; I < N; ++I) {
        if (T <= S[I].P) {
            const float Span = S[I].P - S[I - 1].P;
            const float K = Span > 0.0f ? (T - S[I - 1].P) / Span : 1.0f;
            const Color& A = S[I - 1].C;
            const Color& B = S[I].C;
            return {A.R + (B.R - A.R) * K, A.G + (B.G - A.G) * K,
                    A.B + (B.B - A.B) * K, A.A + (B.A - A.A) * K};
        }
    }
    return S[N - 1].C;
}

// A unit-rect (0,0)-(1,1) vertical strip: one vertex row per stop, the stop colour
// baked per vertex — the GPU interpolates between stops. Same triangle-list winding
// as MakeQuad (no fans: MoltenVK).
Lur::Render::MeshHandle MakeGradientStrip(IRenderer* R, const GradStop* Stops, int N, float Alpha) {
    Lur::Render::Vertex V[2 * 8];
    uint32_t Idx[6 * 7];
    const Lur::Math::Vec3 Nrm{0.0f, 0.0f, 1.0f};
    for (int I = 0; I < N; ++I) {
        const Color& C = Stops[I].C;
        const Lur::Math::Vec4 VC{C.R, C.G, C.B, C.A * Alpha};
        V[2 * I + 0] = {{0.0f, Stops[I].P, 0.0f}, Nrm, {0.0f, Stops[I].P}, VC};
        V[2 * I + 1] = {{1.0f, Stops[I].P, 0.0f}, Nrm, {1.0f, Stops[I].P}, VC};
    }
    uint32_t K = 0;
    for (int I = 0; I < N - 1; ++I) {
        const uint32_t A = 2 * I;
        Idx[K++] = A; Idx[K++] = A + 1; Idx[K++] = A + 3;
        Idx[K++] = A; Idx[K++] = A + 3; Idx[K++] = A + 2;
    }
    return R->CreateMesh(V, static_cast<uint32_t>(2 * N), Idx, K);
}

}  // namespace

float GameView::VisibleWorldHeight(float WidthPx, float HeightPx) {
    return HeightPx / Ppu(WidthPx);
}

float GameView::BottomHudWorldUnits(float WidthPx) const {
    const float HS = HudScale(WidthPx);
    const float Pad = 8.0f * HS, Gap = 6.0f * HS;
    const float GroupGap = 4.0f * Gap;
    const float PlateW = (WidthPx - 2.0f * Pad - GroupGap - 2.0f * Gap) / 4.0f;
    // nav-bar inset + plate block + group header + a margin so the camp sits WELL
    // above the plates
    return (BottomInsetPx + Pad + PlateW * 1.02f + 20.0f * HS + 3.0f * Pad) / Ppu(WidthPx);
}

float GameView::TopHudWorldUnits(float WidthPx) const {
    const float HS = HudScale(WidthPx);
    // status-bar inset + dropdown block + status panel + a margin, mirroring the
    // bottom: the ENEMY camp must clear the top chrome at max scroll-up.
    return (TopInsetPx + 82.0f * HS + 24.0f * HS) / Ppu(WidthPx);
}

void GameView::CreateResources(IRenderer* Renderer) {
    const Lur::Render::Quad Q = Lur::Render::MakeQuad();  // white; the material tints it
    Quad = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);

    // Field backdrop + grid (#85): gradient meshes drawn under everything else.
    WhiteMat = FlatMat(Renderer, {1.0f, 1.0f, 1.0f, 1.0f});
    FieldGradMesh = MakeGradientStrip(Renderer, FieldStops, NumFieldStops, 1.0f);
    VLineMesh = MakeGradientStrip(Renderer, GridStops, 2, GridAlpha);
    for (int I = 0; I < GridShades; ++I) {
        Color C = GradSample(GridStops, 2, static_cast<float>(I) / (GridShades - 1));
        C.A *= GridAlpha;
        GridLut[I] = FlatMat(Renderer, C);
    }

    // Upload the cooked glyph atlas (#85): GlyphCount masks side by side, RG8
    // interleaved (R = shade, G = coverage). White sources -> shade 255, so the
    // tint IS the fill and coverage is the cutout.
    {
        constexpr int S = RpsArt::IconSize;
        static uint8_t Rg[GlyphCount * S * S * 2];  // ~256 KB scratch — static, off the stack
        for (int G = 0; G < GlyphCount; ++G)
            for (int Y = 0; Y < S; ++Y)
                for (int X = 0; X < S; ++X) {
                    const size_t Dst = 2 * (static_cast<size_t>(Y) * (GlyphCount * S) + static_cast<size_t>(G) * S + X);
                    const size_t Src = static_cast<size_t>(Y) * S + X;
                    Rg[Dst + 0] = RpsArt::IconShade[G][Src];
                    Rg[Dst + 1] = RpsArt::IconCoverage[G][Src];
                }
        IconAtlas = Renderer->LoadTexture(Rg, GlyphCount * S, S, Lur::Render::ETextureFormat::Rg8);
    }
    {
        MaterialDesc D;
        D.BaseColor = IconAtlas;
        AtlasMat = Renderer->CreateMaterial(D);  // white tint; per-instance colour fills
    }
    constexpr float GC = static_cast<float>(GlyphCount);
    for (int G = 0; G < GlyphCount; ++G) {
        Lur::Render::Quad Q = Lur::Render::MakeQuad();
        const float U0 = static_cast<float>(G) / GC, U1 = static_cast<float>(G + 1) / GC;
        Q.Vertices[0].Uv = {U0, 0.0f}; Q.Vertices[1].Uv = {U1, 0.0f};
        Q.Vertices[2].Uv = {U1, 1.0f}; Q.Vertices[3].Uv = {U0, 1.0f};
        GlyphMesh[G] = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    }

    // Locked team + accent tints (rps-hud-prototype.html): silhouette fill colours.
    TeamTint[0] = {Srgb(0x3F), Srgb(0xA8), Srgb(0xDC), 1.0f};  // you (bottom)
    TeamTint[1] = {Srgb(0xE0), Srgb(0x4A), Srgb(0x31), 1.0f};  // foe (top)
    auto AtlasTinted = [&](Color C) {
        MaterialDesc D;
        D.BaseColor = IconAtlas;
        D.Tint = C;
        return Renderer->CreateMaterial(D);
    };
    CampMat[0] = AtlasTinted(TeamTint[0]);
    CampMat[1] = AtlasTinted(TeamTint[1]);
    // Per-type shade of each team's hue — same colour family, four readable variants
    // (negative = toward black, positive = toward white: keeps the hue, shifts lightness).
    // Order matches EUnit: Miner, Rock, Paper, Scissor. Playtest 2026-07-20: cart is the
    // LIGHTEST, then progressively DARKER left→right, with HIGH contrast between steps.
    constexpr float TypeLight[UnitCount] = {0.62f, 0.22f, -0.22f, -0.62f};
    auto Shade = [](Color B, float F) -> Color {
        if (F < 0.0f) { const float K = 1.0f + F; return {B.R * K, B.G * K, B.B * K, B.A}; }
        return {B.R + (1.0f - B.R) * F, B.G + (1.0f - B.G) * F, B.B + (1.0f - B.B) * F, B.A};
    };
    for (int Tm = 0; Tm < 2; ++Tm)
        for (int Ty = 0; Ty < UnitCount; ++Ty) {
            TeamTypeTint[Tm][Ty] = Shade(TeamTint[Tm], TypeLight[Ty]);
            TypeTintMat[Tm][Ty] = AtlasTinted(TeamTypeTint[Tm][Ty]);
            Color Dim = TeamTypeTint[Tm][Ty]; Dim.A = 0.4f;
            TypeTintMatDim[Tm][Ty] = AtlasTinted(Dim);
        }
    MineMat = AtlasTinted({Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f});  // mine stone = gold tone
    HealthBg = FlatMat(Renderer, {0.05f, 0.05f, 0.05f, 0.9f});
    HealthFg = FlatMat(Renderer, {0.35f, 0.95f, 0.40f, 1.0f});
    GoldBarFg = FlatMat(Renderer, {0.85f, 0.66f, 0.24f, 1.0f});
#if !LUR_SHIPPING
    DevPanelMat = FlatMat(Renderer, {0.08f, 0.08f, 0.08f, 0.88f});  // DevTheme charcoal
    DevAccentMat = FlatMat(Renderer, {0.25f, 0.95f, 0.85f, 1.0f});  // DevTheme cyan accent
    DevKeyMat = FlatMat(Renderer, {0.20f, 0.22f, 0.24f, 0.98f});    // numpad key face
#endif

    Font.Init(Lur::Text::InterFont());
    Font.UploadAtlas(*Renderer);
    Text.CreateResources(Renderer, &Font);

    // ---- HUD (#85): locked panel palette + the engine dropdown + DSEG7 clock ----
    PanelMat = FlatMat(Renderer, {Srgb(0x1A), Srgb(0x1F), Srgb(0x24), 0.97f});
    PanelEdge = FlatMat(Renderer, {Srgb(0x39), Srgb(0x42), Srgb(0x4B), 1.0f});
    PlateBg = FlatMat(Renderer, {Srgb(0x23), Srgb(0x29), Srgb(0x30), 0.97f});
    BarBg = FlatMat(Renderer, {0.0f, 0.0f, 0.0f, 0.45f});
    GoldFlat = FlatMat(Renderer, {Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f});
    PlateIconMat = AtlasTinted({Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 1.0f});
    PlateIconDim = AtlasTinted({Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 0.4f});
    GoldIconMat = AtlasTinted({Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f});
    MiniWinMat = FlatMat(Renderer, {1.0f, 1.0f, 1.0f, 0.12f});
    MiniWinEdge = FlatMat(Renderer, {Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 0.6f});
    // First-scroll hint (#85 playtest): alpha-stepped materials (materials are
    // immutable, so the fade walks a LUT) + up/down arrow triangle meshes.
    for (int I = 0; I < HintAlphaSteps; ++I) {
        const float A = static_cast<float>(I + 1) / HintAlphaSteps;
        MaterialDesc DP;
        DP.BaseColor = IconAtlas;
        DP.Tint = {Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), A};
        HintPointer[I] = Renderer->CreateMaterial(DP);
        HintArrow[I] = FlatMat(Renderer, {Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), A});  // white, like the finger
    }
    {
        const Lur::Math::Vec3 Nrm{0.0f, 0.0f, 1.0f};
        const Lur::Math::Vec4 Wc{1.0f, 1.0f, 1.0f, 1.0f};
        const uint32_t Idx[3] = {0, 1, 2};
        Lur::Render::Vertex Up[3] = {{{0.5f, 0.0f, 0.0f}, Nrm, {0.5f, 0.0f}, Wc},
                                     {{1.0f, 1.0f, 0.0f}, Nrm, {1.0f, 1.0f}, Wc},
                                     {{0.0f, 1.0f, 0.0f}, Nrm, {0.0f, 1.0f}, Wc}};
        ArrowUp = Renderer->CreateMesh(Up, 3, Idx, 3);
        Lur::Render::Vertex Dn[3] = {{{0.0f, 0.0f, 0.0f}, Nrm, {0.0f, 0.0f}, Wc},
                                     {{1.0f, 0.0f, 0.0f}, Nrm, {1.0f, 0.0f}, Wc},
                                     {{0.5f, 1.0f, 0.0f}, Nrm, {0.5f, 1.0f}, Wc}};
        ArrowDown = Renderer->CreateMesh(Dn, 3, Idx, 3);
    }
    ClockFont.Init(Lur::Text::Dseg7Font());
    ClockFont.UploadAtlas(*Renderer);
    ClockText.CreateResources(Renderer, &ClockFont);
    Selector.CreateResources(Renderer, &Font);

    Ready = true;
}

void GameView::SetLinked(bool InLinked) {
    if (Linked == InLinked) return;
    Linked = InLinked;
    SelectorDirty = true;
}

void GameView::RefreshSelector() {
    // Two rows for now: the live BLE/loopback peer and the local hot-seat. Real
    // opponent enumeration (chess's OpponentRegistry pattern) rides the RPS
    // persistence work — #85's follow-up; the widget and layout are final.
    Lur::Hud::DropdownItem Items[2];
    Items[0].Label = Linked ? "Linked peer" : "Searching...";
    Items[0].Lead = Lur::Hud::ELeadStyle::Dot;
    Items[0].LeadFill = Linked ? Color{Srgb(0x56), Srgb(0xC1), Srgb(0x5F), 1.0f}
                               : Color{Srgb(0x5B), Srgb(0x67), Srgb(0x70), 1.0f};
    Items[1].Label = "Same device";
    Items[1].Lead = Lur::Hud::ELeadStyle::Split;
    Selector.SetItems(Items, 2);
    Selector.SetSelected(0);
    SelectorDirty = false;
}

#if !LUR_SHIPPING
namespace {
// Every AffectsGameplay cvar as (parent-path, cvar) pairs, SORTED by name — the input to the
// hierarchical tree (#121). The DOTTED NAME is the hierarchy: the parent path is the name up to
// the last '.', so "rps.boid.sep_strength" nests under rps -> boid and the row shows only
// "sep_strength" (the leaf, after the last dot). There is no separate category. Name-sorted
// input means each node's leaves come out name-sorted (BuildCategoryTree preserves leaf order),
// so the layout is stable frame to frame.
std::vector<std::pair<std::string, Lur::Core::ICVar*>> GatherGameplayCvars() {
    std::vector<std::pair<std::string, Lur::Core::ICVar*>> Items;
    Lur::Core::CVarRegistry::ForEach([&](Lur::Core::ICVar* C) {
        if (!C->AffectsGameplay()) return;
        const std::string Name = C->Name();
        const auto Dot = Name.rfind('.');
        Items.emplace_back(Dot == std::string::npos ? std::string{} : Name.substr(0, Dot), C);
    });
    std::sort(Items.begin(), Items.end(), [](const auto& A, const auto& B) {
        return std::strcmp(A.second->Name(), B.second->Name()) < 0;
    });
    return Items;
}
}  // namespace

void GameView::DevTap(float XPx, float YPx) {
    DevTapX_.store(XPx, std::memory_order_relaxed);
    DevTapY_.store(YPx, std::memory_order_relaxed);
    DevTapPending_.store(true, std::memory_order_release);  // consumed on the render thread
}
#endif

int GameView::OnTap(float XPx, float YPx) {
    if (!Ready) return -1;
    if (Selector.OnTap(XPx, YPx)) {
        Selector.TookSelection();  // selection has no target yet (#85 follow-up)
        return -2;
    }
    for (int Ty = 0; Ty < 4; ++Ty) {
        const float* Rc = PlateRect[Ty];
        if (XPx >= Rc[0] && XPx <= Rc[0] + Rc[2] && YPx >= Rc[1] && YPx <= Rc[1] + Rc[3])
            return Ty;
    }
    return -1;
}

void GameView::Render(IRenderer* Renderer, const Snapshot& Snap, float Alpha, float CameraY,
                      float WidthPx, float HeightPx, bool FlipY, float DtSec) {
    if (!Ready) return;
    const float P = Ppu(WidthPx);
    const float WHf = FW(WorldHeight);
    // Everything user-facing is VIEWER-RELATIVE: "you" is whichever team this device
    // plays (FlipY is set exactly for the top/team-1 player). Blue = you, red = foe,
    // and the HUD reads your team's gold/queues — on both phones.
    const int My = FlipY ? 1 : 0;
    const int Foe = 1 - My;
    const float HS = HudScale(WidthPx);  // HUD metrics scale with resolution (#85 feedback)
    if (DtSec < 0.0f) DtSec = 0.0f;
    if (DtSec > 0.25f) DtSec = 0.25f;    // view-side animation clock (hitch-proof)

    // World -> screen. Pixel space is Y-DOWN (MakeOrthoCamera); world Y grows UP (your
    // camp at small Y sits at the bottom), so flip: Wy == CameraY lands at the bottom.
    // FlipY mirrors the field vertically for the TOP player (team 1) so both players see
    // their own camp at the bottom (§9). The flip is baked here; the camera (which is in
    // this flipped space) and the content-drag are unchanged.
    auto SX = [&](float Wx) { return Wx * P; };
    auto SY = [&](float Wy) { const float Fy = FlipY ? WHf - Wy : Wy; return HeightPx - (Fy - CameraY) * P; };

    // Centre a Wpx x Hpx quad at screen (Cx, Cy).
    auto Blit = [&](Lur::Render::MaterialHandle Mat, float Cx, float Cy, float Wpx, float Hpx) {
        const Mat4 M = Mat4::Translation({Cx - Wpx * 0.5f, Cy - Hpx * 0.5f, 0.0f}) *
                       Mat4::Scale({Wpx, Hpx, 1.0f});
        Renderer->DrawMesh(Quad, Mat, M);
    };

    Renderer->BeginFrame(Lur::Render::MakeOrthoCamera(WidthPx, HeightPx));

    // Field backdrop: the locked SCREENSPACE gradient — spans the viewport, never scrolls.
    Renderer->DrawMesh(FieldGradMesh, WhiteMat, Mat4::Scale({WidthPx, HeightPx, 1.0f}));

    // World grid. The LINES are world-anchored (they scroll and X never scrolls, so
    // vertical lines are screen-static); the COLOUR is sampled from the grid gradient
    // in screen space (prototype rule), so the palette holds still under the scroll.
    for (float Wx = 0.0f; Wx <= FW(WorldWidth) + 0.01f; Wx += GridStepWu) {
        const Mat4 M = Mat4::Translation({SX(Wx) - 0.5f, 0.0f, 0.0f}) *
                       Mat4::Scale({1.0f, HeightPx, 1.0f});
        Renderer->DrawMesh(VLineMesh, WhiteMat, M);
    }
    for (float Wy = 0.0f; Wy <= WHf + 0.01f; Wy += GridStepWu) {
        const float Y = SY(Wy);
        if (Y < -1.0f || Y > HeightPx + 1.0f) continue;
        int Li = static_cast<int>(Y / HeightPx * (GridShades - 1) + 0.5f);
        if (Li < 0) Li = 0;
        if (Li >= GridShades) Li = GridShades - 1;
        Blit(GridLut[Li], WidthPx * 0.5f, Y, WidthPx, 1.0f);
    }

    // Draw a glyph-atlas quad (tinted silhouette) centred at (Cx, Cy).
    auto BlitGlyph = [&](int Glyph, Lur::Render::MaterialHandle Mat, float Cx, float Cy, float Px) {
        const Mat4 M = Mat4::Translation({Cx - Px * 0.5f, Cy - Px * 0.5f, 0.0f}) *
                       Mat4::Scale({Px, Px, 1.0f});
        Renderer->DrawMesh(GlyphMesh[Glyph], Mat, M);
    };

    // Camps render as ICONS like everything else (design lock: same tech, no bespoke
    // geometry) — tinted the owner's ABSOLUTE team colour (see the unit-tint note).
    const float CampPx = 4.5f * P;
    BlitGlyph(GlyphCamp, CampMat[0], SX(FW(CampX)), SY(FW(Camp0Y)), CampPx);
    BlitGlyph(GlyphCamp, CampMat[1], SX(FW(CampX)), SY(FW(Camp1Y)), CampPx);

    // Mines — finite (#84): a depleted mine is gone; live ones carry a gold reserve
    // bar above them (same visual language as unit health, gold fill).
    const float MinePx = 2.2f * P;
    for (int T = 0; T < NumMines; ++T) {
        if (Snap.MineGold[T] <= 0) continue;
        const float Mx = SX(FW(Snap.MineX[T])), My = SY(FW(Snap.MineY[T]));
        BlitGlyph(GlyphMine, MineMat, Mx, My, MinePx);
        const float Frac = static_cast<float>(Snap.MineGold[T]) / static_cast<float>(MineGoldCapacity);
        const float BarW = MinePx, BarH = 2.0f * HS, BarY = My - MinePx * 0.5f - 3.0f * HS;
        Blit(HealthBg, Mx, BarY, BarW, BarH);
        Blit(GoldBarFg, Mx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH);
    }

    // Units — ONE instanced draw. Each instance carries prev+cur pixel centres; the
    // vertex shader lerps by Alpha, so there is no per-unit CPU interpolation (design §6).
    // We map prev/cur to pixels here (a cheap affine per unit, not a lerp).
    // Design lock: units are bare alpha-cutout SILHOUETTES tinted by team — type is
    // the shape, ownership is the colour. Per-instance UV rect picks the glyph.
    const float UnitPx = 1.7f * P;
    uint32_t N = 0;
    int32_t Workers = 0, Soldiers = 0;  // viewer-team split for the population counter
    for (int32_t I = 0; I < Snap.Count && N < static_cast<uint32_t>(MaxUnits); ++I) {
        if (!Snap.IsAlive(I)) continue;
        const uint8_t Ty = Snap.Type[I], Tm = Snap.Team[I];
        if (Tm == My) { if (Ty == UnitMiner) ++Workers; else ++Soldiers; }
        // ABSOLUTE team colours (playtest: the players sit together and compare
        // screens — team 0 is blue and team 1 red on BOTH phones, so a unit looks
        // the same wherever you see it), now a UNIQUE per-type shade of that team hue
        // so composition reads by colour as well as glyph. HUD numbers stay viewer-relative.
        const Color C = TeamTypeTint[Tm][Ty];
        Lur::Render::InstanceData& D = Instances[N++];
        D.PrevX = SX(FW(Snap.PrevX[I])); D.PrevY = SY(FW(Snap.PrevY[I]));
        D.CurX = SX(FW(Snap.PosX[I]));   D.CurY = SY(FW(Snap.PosY[I]));
        D.R = C.R; D.G = C.G; D.B = C.B; D.A = C.A;
        D.Size = Ty == UnitMiner ? UnitPx * 1.5f : UnitPx;  // carts read bigger (playtest)
        D.U0 = static_cast<float>(Ty) / static_cast<float>(GlyphCount); D.V0 = 0.0f;
        D.U1 = static_cast<float>(Ty + 1) / static_cast<float>(GlyphCount); D.V1 = 1.0f;
        // Facing (soldiers only; carts stay upright): the glyph's TOP points along the
        // MOVE direction. Below a low speed we DON'T update the stored heading — a nearly
        // stopped unit holds its last angle instead of spinning on sub-pixel noise.
        D.FaceX = 0.0f; D.FaceY = 0.0f;
        if (Ty != UnitMiner) {
            const float Vx = D.CurX - D.PrevX, Vy = D.CurY - D.PrevY;
            const float Sp = std::sqrt(Vx * Vx + Vy * Vy);
            if (Sp > 0.12f * P) { LastFaceX[I] = Vx / Sp; LastFaceY[I] = Vy / Sp; }  // fast: update
            D.FaceX = LastFaceX[I]; D.FaceY = LastFaceY[I];                          // else hold
        }
        // A LOADED cart shows its ore heap in COIN gold (playtest): one extra
        // instance, same endpoints, the heap glyph over the cart — still one draw.
        if (Ty == UnitMiner && Snap.Carry[I] > 0 && N < static_cast<uint32_t>(MaxUnits)) {
            Lur::Render::InstanceData& O = Instances[N++];
            O = D;  // same size: the enlarged heap is baked into the mask, seated on the rail
            O.R = Srgb(0xD9); O.G = Srgb(0xA9); O.B = Srgb(0x3C); O.A = 1.0f;
            O.U0 = static_cast<float>(GlyphOreLoad) / static_cast<float>(GlyphCount);
            O.U1 = static_cast<float>(GlyphOreLoad + 1) / static_cast<float>(GlyphCount);
        }
    }
    Renderer->DrawInstances(Quad, Instances, N, Alpha, AtlasMat);

    // Health bars on top of the units (sparse: only hurt units). Kept on the per-mesh
    // path — a second instanced draw is a later refinement.
    for (int32_t I = 0; I < Snap.Count; ++I) {
        if (!Snap.IsAlive(I)) continue;
        const int32_t MaxHp = Snap.Units[Snap.Type[I]].MaxHp;
        if (Snap.Hp[I] <= 0 || Snap.Hp[I] >= MaxHp) continue;
        const float Sx = SX(FW(Snap.PrevX[I]) + (FW(Snap.PosX[I]) - FW(Snap.PrevX[I])) * Alpha);
        const float Sy = SY(FW(Snap.PrevY[I]) + (FW(Snap.PosY[I]) - FW(Snap.PrevY[I])) * Alpha);
        const float Frac = static_cast<float>(Snap.Hp[I]) / static_cast<float>(MaxHp);
        const float BarW = UnitPx, BarH = 2.0f * HS;
        const float BarY = Sy - UnitPx * 0.5f - 3.0f * HS;
        Blit(HealthBg, Sx, BarY, BarW, BarH);
        Blit(HealthFg, Sx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH);  // left-aligned
    }

    // Deposit juice (#85 playtest): "+N" floats where a miner banked its carry —
    // world-anchored (they ride the scroll), rising and fading over a second. The
    // carry >0 -> 0 edge only ever happens at the deposit, so it IS the event.
    for (int32_t I = 0; I < Snap.Count; ++I) {
        if (!Snap.IsAlive(I)) { LastCarry[I] = 0; continue; }
        const int32_t C = Snap.Type[I] == UnitMiner ? Snap.Carry[I] : 0;
        if (LastCarry[I] > 0 && C == 0) {
            for (int K = 0; K < MaxFloats; ++K)
                if (!Floats[K].Active) {
                    Floats[K] = {FW(Snap.PosX[I]), FW(Snap.PosY[I]), 0.0f, LastCarry[I], true};
                    break;
                }
        }
        LastCarry[I] = C;
    }
    for (int K = 0; K < MaxFloats; ++K) {
        GoldFloat& F = Floats[K];
        if (!F.Active) continue;
        F.Age += DtSec;
        if (F.Age > 1.0f) { F.Active = false; continue; }
        const float A = F.Age < 0.55f ? 1.0f : 1.0f - (F.Age - 0.55f) / 0.45f;
        char FB[16];
        std::snprintf(FB, sizeof(FB), "+%d", F.Value);
        const float Fy = SY(F.Wy) - (14.0f + 30.0f * F.Age) * HS;
        Text.Draw(Renderer, FB, SX(F.Wx) - 60.0f, Fy, 120.0f, 20.0f * HS, 13.0f * HS,
                  {Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), A}, Lur::Text::EHAlign::Center,
                  Lur::Text::EVAlign::Top, false);
    }

    // ---- HUD (GUI layer, pixel space) — the locked layout (#85): opponent
    // dropdown on top, status panel (gold | population | clock) under it, four
    // production plates along the bottom edge. ----
    Renderer->BeginGui();
    if (SelectorDirty) RefreshSelector();

    using Lur::Text::EHAlign;
    using Lur::Text::EVAlign;
    const Color Ico{Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 1.0f};
    const Color GoldC{Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f};
    const Color BadC{Srgb(0xE1), Srgb(0x4E), Srgb(0x38), 1.0f};
    const float Pad = 8.0f * HS;
    char Buf[64];

    // Status panel — below the OS status bar (TopInsetPx) and the dropdown pill.
    const float PanelY = TopInsetPx + 52.0f * HS, PanelH = 30.0f * HS;
    Blit(PanelMat, WidthPx * 0.5f, PanelY + PanelH * 0.5f, WidthPx, PanelH);
    Blit(PanelEdge, WidthPx * 0.5f, PanelY + PanelH, WidthPx, 1.0f * HS);
    const float Mid = PanelY + PanelH * 0.5f;
    BlitGlyph(GlyphGold, GoldIconMat, Pad + 9.0f * HS, Mid, 18.0f * HS);
    // Animated counter (#85 playtest): the shown value ROLLS toward the real one,
    // and the type pops/brightens on a gain (pairs with the +N deposit floats).
    const float GoldNow = static_cast<float>(Snap.Gold[My]);
    if (DisplayedGold < 0.0f) DisplayedGold = GoldNow;
    if (GoldNow > DisplayedGold + 0.5f) GoldPulse = 1.0f;
    DisplayedGold += (GoldNow - DisplayedGold) * (1.0f - std::exp(-8.0f * DtSec));
    if (std::fabs(GoldNow - DisplayedGold) < 0.6f) DisplayedGold = GoldNow;
    GoldPulse -= DtSec * 2.5f;
    if (GoldPulse < 0.0f) GoldPulse = 0.0f;
    const float GoldK = 0.5f * GoldPulse;
    const Color GoldTxt{GoldC.R + (1.0f - GoldC.R) * GoldK, GoldC.G + (1.0f - GoldC.G) * GoldK,
                        GoldC.B + (1.0f - GoldC.B) * GoldK, 1.0f};
    std::snprintf(Buf, sizeof(Buf), "%d", static_cast<int>(DisplayedGold + 0.5f));
    Text.Draw(Renderer, Buf, Pad + 22.0f * HS, PanelY, 120.0f * HS, PanelH,
              15.0f * HS * (1.0f + 0.3f * GoldPulse), GoldTxt,
              EHAlign::Left, EVAlign::Middle, false);
    BlitGlyph(GlyphMiner, PlateIconMat, WidthPx * 0.5f - 40.0f * HS, Mid, 16.0f * HS);
    std::snprintf(Buf, sizeof(Buf), "%d / %d", Workers, Soldiers);
    Text.Draw(Renderer, Buf, WidthPx * 0.5f - 28.0f * HS, PanelY, 56.0f * HS, PanelH,
              14.0f * HS, Ico, EHAlign::Center, EVAlign::Middle, false);
    BlitGlyph(GlyphSwords, PlateIconMat, WidthPx * 0.5f + 40.0f * HS, Mid, 16.0f * HS);
    const uint32_t Secs = Snap.Tick / TickRateHz;  // tick-derived: identical on both peers
    std::snprintf(Buf, sizeof(Buf), "%02u:%02u", Secs / 60u, Secs % 60u);
    ClockText.Draw(Renderer, Buf, WidthPx - Pad - 74.0f * HS, PanelY, 74.0f * HS, PanelH,
                   13.0f * HS, Ico, EHAlign::Right, EVAlign::Middle, false);

    // Production plates: the icon IS the unit glyph; stack tag, cost, progress bar
    // (which visibly accelerates with the stack — #84's pacing thesis on screen).
    // Anchored above the OS bottom inset (Android nav bar / iOS home indicator).
    // Grouping (#85 playtest): the miner plate stands apart with a GOLD frame under a
    // gold-token header — "spawns gold gatherers" — while rock/paper/scissors share a
    // backing strip under a crossed-swords header — "spawns warriors".
    const float Gap = 6.0f * HS;
    const float GroupGap = 4.0f * Gap;
    const float PlateW = (WidthPx - 2.0f * Pad - GroupGap - 2.0f * Gap) / 4.0f;
    const float PlateH2 = PlateW * 1.02f;
    const float PlateY = HeightPx - BottomInsetPx - Pad - PlateH2;
    const float HeadH = 16.0f * HS;
    const float TrioX = Pad + PlateW + GroupGap;
    const float TrioW = 3.0f * PlateW + 2.0f * Gap;
    const float BackTop = PlateY - HeadH - 4.0f * HS;
    const float BackH = PlateH2 + HeadH + 8.0f * HS;
    Blit(PanelMat, Pad + PlateW * 0.5f, BackTop + BackH * 0.5f, PlateW + 6.0f * HS, BackH);
    Blit(PanelMat, TrioX + TrioW * 0.5f, BackTop + BackH * 0.5f, TrioW + 6.0f * HS, BackH);
    BlitGlyph(GlyphGold, GoldIconMat, Pad + PlateW * 0.5f, PlateY - HeadH * 0.5f - 2.0f * HS,
              13.0f * HS);
    BlitGlyph(GlyphSwords, PlateIconMat, TrioX + TrioW * 0.5f, PlateY - HeadH * 0.5f - 2.0f * HS,
              13.0f * HS);
    for (int Ty = 0; Ty < 4; ++Ty) {
        const float X = Ty == 0 ? Pad : TrioX + static_cast<float>(Ty - 1) * (PlateW + Gap);
        PlateRect[Ty][0] = X; PlateRect[Ty][1] = PlateY;
        PlateRect[Ty][2] = PlateW; PlateRect[Ty][3] = PlateH2;
        const bool Afford = Snap.Gold[My] >= Snap.Units[Ty].Cost;
        Blit(Ty == 0 ? GoldFlat : PanelEdge, X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f,
             PlateW + 2.0f, PlateH2 + 2.0f);
        Blit(PlateBg, X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW, PlateH2);
        // Button glyph in the LOCAL team's per-type tint (playtest 2026-07-20) — the
        // plate previews the exact colour the spawned unit will wear, not a flat white.
        BlitGlyph(Ty, Afford ? TypeTintMat[My][Ty] : TypeTintMatDim[My][Ty],
                  X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW * 0.52f);
        BlitGlyph(GlyphGold, GoldIconMat, X + 12.0f * HS, PlateY + 12.0f * HS, 13.0f * HS);
        std::snprintf(Buf, sizeof(Buf), "%d", Snap.Units[Ty].Cost);
        Text.Draw(Renderer, Buf, X + 20.0f * HS, PlateY + 5.0f * HS, 40.0f * HS, 14.0f * HS,
                  13.0f * HS, Afford ? Ico : BadC, EHAlign::Left, EVAlign::Top, false);
        const int32_t QN = Snap.QueueCount[My][Ty];
        if (QN > 0) {
            Blit(GoldFlat, X + PlateW - 14.0f * HS, PlateY + 9.0f * HS, 26.0f * HS, 16.0f * HS);
            std::snprintf(Buf, sizeof(Buf), "%dx", QN);
            Text.Draw(Renderer, Buf, X + PlateW - 27.0f * HS, PlateY + 1.0f * HS, 26.0f * HS,
                      16.0f * HS, 12.0f * HS, {Srgb(0x14), Srgb(0x16), Srgb(0x1A), 1.0f},
                      EHAlign::Center, EVAlign::Middle, false);
        }
        const float BarW = PlateW - 12.0f * HS;
        Blit(BarBg, X + PlateW * 0.5f, PlateY + PlateH2 - 7.0f * HS, BarW, 5.0f * HS);
        if (QN > 0) {
            float Frac = static_cast<float>(Snap.BuildProgress[My][Ty]) /
                         static_cast<float>(Snap.Units[Ty].BuildTicks);
            if (Frac > 1.0f) Frac = 1.0f;
            const float Bw = BarW * Frac;
            if (Bw > 0.5f)
                Blit(GoldFlat, X + 6.0f * HS + Bw * 0.5f, PlateY + PlateH2 - 7.0f * HS, Bw,
                     5.0f * HS);
        }
    }

    // Match-result banner. (The tick/FOE debug line is gone - playtest feedback;
    // the LOCKSTEP log line carries the same numbers for diagnosis.)
    (void)Foe;
    if (Snap.Result != ResultOngoing) {
        Text.Draw(Renderer, ResultStr(Snap.Result, My), 0.0f, HeightPx * 0.42f, WidthPx,
                  40.0f * HS, 30.0f * HS, GoldC, EHAlign::Center, EVAlign::Middle, false);
    }

    // Minimap strip (playtest): the WHOLE field on the right edge, VS Code
    // scrollbar-style, in the same GUI layer as the plates/panel/dropdown. Dots are
    // units + camps (absolute team colours) and live deposits (gold); the bright
    // window is exactly what the camera shows. One extra instanced draw.
    {
        const float StripW = 12.0f * HS;
        const float StripX = WidthPx - StripW;
        const float StripY = PanelY + PanelH + 4.0f * HS;
        const float StripB = PlateY - HeadH - 8.0f * HS;
        const float StripH = StripB - StripY;
        const float WH = FW(WorldHeight);
        Blit(PanelMat, StripX + StripW * 0.5f, StripY + StripH * 0.5f, StripW, StripH);
        Blit(PanelEdge, StripX - 0.5f, StripY + StripH * 0.5f, 1.0f, StripH);
        // World -> strip. CameraY (and the camera window) already live in the FLIPPED
        // space the field renders in; world positions flip the same way, so the strip
        // is oriented exactly like the screen: home at the bottom, enemy at the top.
        auto MapFy = [&](float Fy) { return StripB - (Fy / WH) * StripH; };
        auto FlipW = [&](float Wy) { return FlipY ? WH - Wy : Wy; };
        auto MapX = [&](float Wx) {
            return StripX + 1.5f + (Wx / FW(WorldWidth)) * (StripW - 3.0f);
        };
        const float VisH = HeightPx / P;
        float WinTop = MapFy(CameraY + VisH), WinBot = MapFy(CameraY);
        if (WinTop < StripY) WinTop = StripY;
        if (WinBot > StripB) WinBot = StripB;
        if (WinBot > WinTop) {
            Blit(MiniWinMat, StripX + StripW * 0.5f, (WinTop + WinBot) * 0.5f, StripW,
                 WinBot - WinTop);
            Blit(MiniWinEdge, StripX + StripW * 0.5f, WinTop, StripW, 1.0f);
            Blit(MiniWinEdge, StripX + StripW * 0.5f, WinBot, StripW, 1.0f);
        }
        // Dots — reuse the per-frame instance scratch (the unit batch was already
        // uploaded by its draw call). Prev == Cur: no interpolation on the map.
        uint32_t M = 0;
        auto Dot = [&](float Px, float Py, float Sz, Color C) {
            if (M >= static_cast<uint32_t>(MaxUnits)) return;
            Lur::Render::InstanceData& D = Instances[M++];
            D.PrevX = D.CurX = Px;
            D.PrevY = D.CurY = Py;
            D.R = C.R; D.G = C.G; D.B = C.B; D.A = C.A;
            D.Size = Sz;
            D.U0 = 0.0f; D.V0 = 0.0f; D.U1 = 0.0f; D.V1 = 0.0f;  // flat material: no atlas
            D.FaceX = 0.0f; D.FaceY = 0.0f;                      // dots never rotate (reused scratch)
        };
        const Color MiniGold{Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 0.9f};
        for (int T = 0; T < NumMines; ++T)
            if (Snap.MineGold[T] > 0)
                Dot(MapX(FW(Snap.MineX[T])), MapFy(FlipW(FW(Snap.MineY[T]))), 2.6f * HS, MiniGold);
        Dot(MapX(FW(CampX)), MapFy(FlipW(FW(Camp0Y))), 3.4f * HS, TeamTint[0]);
        Dot(MapX(FW(CampX)), MapFy(FlipW(FW(Camp1Y))), 3.4f * HS, TeamTint[1]);
        for (int32_t I = 0; I < Snap.Count; ++I) {
            if (!Snap.IsAlive(I)) continue;
            Dot(MapX(FW(Snap.PosX[I])), MapFy(FlipW(FW(Snap.PosY[I]))), 2.0f * HS,
                TeamTypeTint[Snap.Team[I]][Snap.Type[I]]);  // same per-type tint as the units
        }
        Renderer->DrawInstances(Quad, Instances, M, 0.0f, WhiteMat);
    }

    // First-scroll hint (#85 playtest): from the moment one of YOUR units walks off
    // the screen, bob a pointing finger + up/down arrows mid-screen; the first camera
    // pan fades it out for good (per-session, view-only).
    if (Hint == EHint::Idle) {
        for (int32_t I = 0; I < Snap.Count; ++I) {
            if (!Snap.IsAlive(I) || Snap.Team[I] != My) continue;
            const float Sy = SY(FW(Snap.PosY[I]));
            if (Sy < -2.0f * UnitPx || Sy > HeightPx + 2.0f * UnitPx) {
                Hint = EHint::Active;
                HintCamY = CameraY;
                break;
            }
        }
    } else if (Hint == EHint::Active) {
        HintAge += DtSec;
        if (std::fabs(CameraY - HintCamY) > 1.0f) { Hint = EHint::Fading; HintFade = 1.0f; }
    } else if (Hint == EHint::Fading) {
        HintAge += DtSec;
        HintFade -= DtSec * 2.0f;
        if (HintFade <= 0.0f) Hint = EHint::Done;
    }
    if (Hint == EHint::Active || Hint == EHint::Fading) {
        const float A = Hint == EHint::Active ? 1.0f : HintFade;
        int Step = static_cast<int>(A * HintAlphaSteps) - 1;
        if (Step < 0) Step = 0;
        if (Step >= HintAlphaSteps) Step = HintAlphaSteps - 1;
        const float Bob = std::sin(HintAge * 4.0f) * 14.0f * HS;
        // Just below the top panel with clear margin (playtest): the whole cluster —
        // up arrow at its highest bob — stays under the panel edge, near where the
        // unit walked out of view.
        const float Cx = WidthPx * 0.5f, Cy = TopInsetPx + 168.0f * HS + Bob;
        const float Pp = 56.0f * HS;
        BlitGlyph(GlyphPointer, HintPointer[Step], Cx, Cy, Pp);
        const float Aw = 22.0f * HS, Ah = 14.0f * HS;
        const Mat4 Up = Mat4::Translation({Cx - Aw * 0.5f, Cy - Pp * 0.75f - Ah, 0.0f}) *
                        Mat4::Scale({Aw, Ah, 1.0f});
        Renderer->DrawMesh(ArrowUp, HintArrow[Step], Up);
        const Mat4 Dn = Mat4::Translation({Cx - Aw * 0.5f, Cy + Pp * 0.75f, 0.0f}) *
                        Mat4::Scale({Aw, Ah, 1.0f});
        Renderer->DrawMesh(ArrowDown, HintArrow[Step], Dn);
    }

    // The opponent dropdown draws LAST so its open list overlays the panel.
    Selector.Draw(Renderer, "Opponent", Pad, TopInsetPx + 4.0f * HS, WidthPx - 2.0f * Pad,
                  24.0f * HS);

#if !LUR_SHIPPING
    // ---- The CONSOLE (#114) — one tool, ONE UI on both platforms ----
    // Composited by the BeginDevGui THIRD pass (over the game + its GUI). The shipping build
    // compiles none of it (BeginDevGuiLayer is a no-op, this block is #if'd out). Opened by
    // the phone's two-finger triple-tap or the desktop § key; identical either way.
    if (DevOverlayOpen_) {
        Lur::Render::BeginDevGuiLayer(Renderer);
        const float OW = WidthPx;  // full-window overlay
        // Every AffectsGameplay CVar, arranged into a HIERARCHICAL (|-nested) category tree
        // (#121) drawn as recursively-collapsible sections inside a SCROLLABLE viewport, each
        // row carrying an "i" tooltip button (#129). Values are read straight from the registry
        // (ValueString is guard-free; the global value is only mutated by the console, never the
        // sim thread — so this render-thread read is race-free). Selection is a CVar POINTER,
        // stable across collapse/expand/scroll; the numpad + the tooltip toaster anchor BELOW
        // the selected row (flipping above when they'd run off-screen, PlaceBelowOrAbove).
        const Lur::Render::Color Accent{0.55f, 0.98f, 0.90f, 1.0f};
        const Lur::Render::Color Ink{0.86f, 0.90f, 0.92f, 1.0f};
        const Lur::Render::Color CatInk{0.62f, 0.72f, 0.78f, 1.0f};
        const Lur::Render::Color DimInk{0.40f, 0.45f, 0.48f, 1.0f};
        const float LineH = 20.0f * HS, CatH = 22.0f * HS, TitleH = 26.0f * HS;
        const float IndentW = 12.0f * HS;  // per depth level

        // Split on '.' — the dotted cvar name IS the category hierarchy (#121).
        auto Root = Lur::DevGui::BuildCategoryTree(GatherGameplayCvars(), '.');
        const int Count = Root.TotalLeaves;

        // Fixed panel viewport (the content scrolls inside it, #121). Height is the screen, not
        // the content — so an arbitrary number of cvars fits.
        const float PW = OW - 4.0f * Pad;
        const float X0 = 2.0f * Pad, Y0 = HeightPx * 0.06f;
        const float PH = HeightPx * 0.86f;
        const float ViewTop = Y0 + TitleH + 4.0f * HS;   // content clip band
        const float ViewBot = Y0 + PH - 4.0f * HS;
        const float ViewH = ViewBot - ViewTop;

        // Flatten the (honouring-collapse) tree into a linear list of rows with content-space Y,
        // so scroll, culling, hit-testing + the anchored popovers all read one array. Kind 0 =
        // category header (Node), 1 = a cvar row (Cv).
        struct VItem { int Kind; const Lur::DevGui::CatNode<Lur::Core::ICVar*>* Node;
                       Lur::Core::ICVar* Cv; int Depth; float Cy; };
        std::vector<VItem> Vis;
        float Cy = 0.0f;
        for (Lur::Core::ICVar* C : Root.Leaves) { Vis.push_back({1, nullptr, C, 0, Cy}); Cy += LineH; }
        using Node = Lur::DevGui::CatNode<Lur::Core::ICVar*>;
        std::function<void(const Node&, int)> Flatten = [&](const Node& N, int Depth) {
            Vis.push_back({0, &N, nullptr, Depth, Cy});
            Cy += CatH;
            if (CollapsedCats_.find(N.Path) != CollapsedCats_.end()) return;  // folded: skip body
            for (Lur::Core::ICVar* C : N.Leaves) { Vis.push_back({1, nullptr, C, Depth + 1, Cy}); Cy += LineH; }
            for (const Node& Ch : N.Children) Flatten(Ch, Depth + 1);
        };
        for (const Node& Ch : Root.Children) Flatten(Ch, 0);
        const float ContentH = Cy;

        // Fold in this frame's accumulated scroll (drag on phone / wheel on desktop), clamp.
        ScrollY_ += DevScrollAccum_.exchange(0.0f, std::memory_order_relaxed);
        const float MaxScroll = ContentH > ViewH ? ContentH - ViewH : 0.0f;
        if (ScrollY_ < 0.0f) ScrollY_ = 0.0f;
        if (ScrollY_ > MaxScroll) ScrollY_ = MaxScroll;

        // A cvar's row screen-Y this frame (content-Y - scroll + viewport top), for anchoring
        // the numpad/toaster. Defaults to mid-viewport if the row is folded/scrolled away.
        auto RowScreenY = [&](Lur::Core::ICVar* Which, float Fallback) {
            for (const VItem& It : Vis)
                if (It.Kind == 1 && It.Cv == Which) return ViewTop - ScrollY_ + It.Cy;
            return Fallback;
        };

        // Panel + title + close-X.
        Blit(DevPanelMat, X0 + PW * 0.5f, Y0 + PH * 0.5f, PW, PH);
        const float XbtnS = TitleH;
        const float XbtnX = X0 + PW - XbtnS;
        Blit(DevKeyMat, XbtnX + XbtnS * 0.5f, Y0 + XbtnS * 0.5f, XbtnS, XbtnS);
        Text.Draw(Renderer, "X", XbtnX, Y0, XbtnS, XbtnS, 16.0f * HS, Accent,
                  Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
        char T[96];
        std::snprintf(T, sizeof(T), "Console  %d cvars", Count);
        Text.Draw(Renderer, T, X0 + 10.0f * HS, Y0 + 3.0f * HS, PW - XbtnS - 18.0f * HS,
                  TitleH, 14.0f * HS, Accent);

        // Tap handling — consumed on THIS thread, where every rect is laid out, so hit-test +
        // edits don't race the ValueString reads.
        const bool TapPending = DevTapPending_.load(std::memory_order_acquire);
        const float TapX = DevTapX_.load(std::memory_order_relaxed);
        const float TapY = DevTapY_.load(std::memory_order_relaxed);
        bool TapUsed = false;

        // A visible toaster is modal-lite: the next tap ANYWHERE dismisses it (and is consumed),
        // so it can't also trigger a row underneath. Auto-expires after a few seconds.
        if (!ToastText_.empty()) {
            ToastAge_ += DtSec;
            if (ToastAge_ > 6.0f) { ToastText_.clear(); ToastCvar_ = nullptr; }
        }
        if (TapPending && !TapUsed && !ToastText_.empty()) {
            ToastText_.clear(); ToastCvar_ = nullptr; TapUsed = true;
        }

        // Top-right X closes the view.
        if (TapPending && !TapUsed && TapX >= XbtnX && TapX <= XbtnX + XbtnS && TapY >= Y0 &&
            TapY <= Y0 + TitleH) {
            DevOverlayOpen_ = false; NumpadOpen_ = false; TapUsed = true;
        }

        // Numpad geometry — anchored BELOW the selected row, flipped above when it wouldn't fit.
        const float NumW = OW * 0.62f;
        const float NumX = (OW - NumW) * 0.5f;
        float NumH = NumW;
        if (NumH > HeightPx * 0.42f) NumH = HeightPx * 0.42f;
        const float NumGap = 8.0f * HS;
        const float NumAnchorY = RowScreenY(SelectedCvar_, HeightPx * 0.45f);
        const float NumY = Lur::DevGui::PlaceBelowOrAbove(NumAnchorY, LineH, NumH + 10.0f * HS,
                                                          NumGap, HeightPx) + 5.0f * HS;
        // Cancel: a small 4th button just right of the top row (1 2 3 -> x) that closes the
        // numpad WITHOUT committing a value. Hit-tested before the grid.
        const float NumKeyH = (NumH - NumGap * 3.0f) / 4.0f;
        const float CancelS = NumKeyH * 0.62f;
        const float CancelX = NumX + NumW + 6.0f * HS;
        const float CancelY = NumY + (NumKeyH - CancelS) * 0.5f;
        if (TapPending && !TapUsed && NumpadOpen_ && TapX >= CancelX && TapX <= CancelX + CancelS &&
            TapY >= CancelY && TapY <= CancelY + CancelS) {
            Numpad_.Clear(); NumpadOpen_ = false; TapUsed = true;  // discard, no write
        }
        if (TapPending && !TapUsed && NumpadOpen_ &&
            Numpad_.Tap(NumX, NumY, NumW, NumH, NumGap, TapX, TapY)) {
            TapUsed = true;
            if (Numpad_.TakeEnter()) {  // commit the typed value to the selected CVar
                if (SelectedCvar_)
                    if (!Numpad_.Buffer().empty() &&
                        SelectedCvar_->SetFromString(Numpad_.Buffer().c_str()))
                        if (CvCommitFn_) CvCommitFn_(CvCommitCtx_, *SelectedCvar_);  // persist + sync
                Numpad_.Clear();
                NumpadOpen_ = false;
            }
        }

        // Row columns (constant right edge, so values align down a true column).
        const float RowPad = 4.0f * HS;
        const float ResetS = LineH - 4.0f * HS;
        const float ResetX = X0 + PW - RowPad - ResetS;
        const float ValW = 90.0f * HS;
        const float ValX = ResetX - 6.0f * HS - ValW;

        // Draw + hit-test every visible row, culled to the viewport (rows scrolled fully out of
        // the band are neither drawn nor tappable).
        for (const VItem& It : Vis) {
            const float RowH = It.Kind == 0 ? CatH : LineH;
            const float Sy = ViewTop - ScrollY_ + It.Cy;
            if (Sy + RowH <= ViewTop || Sy >= ViewBot) continue;  // fully outside the band
            const float IndentX = X0 + static_cast<float>(It.Depth) * IndentW;
            const bool InBand = TapY >= ViewTop && TapY <= ViewBot;

            if (It.Kind == 0) {  // ---- category header: tap toggles fold ----
                const bool Collapsed = CollapsedCats_.find(It.Node->Path) != CollapsedCats_.end();
                if (TapPending && !TapUsed && InBand && TapX >= IndentX && TapX <= X0 + PW &&
                    TapY >= Sy && TapY <= Sy + CatH) {
                    if (Collapsed) CollapsedCats_.erase(It.Node->Path);
                    else CollapsedCats_.insert(It.Node->Path);
                    TapUsed = true;
                }
                Blit(DevKeyMat, (IndentX + X0 + PW) * 0.5f, Sy + CatH * 0.5f,
                     (X0 + PW - IndentX) - 6.0f * HS, CatH - 3.0f * HS);
                char H[96];
                std::snprintf(H, sizeof(H), "[%c] %s  (%d)", Collapsed ? '+' : '-',
                              It.Node->Segment.c_str(), It.Node->TotalLeaves);
                Text.Draw(Renderer, H, IndentX + 10.0f * HS, Sy, X0 + PW - IndentX - 14.0f * HS,
                          CatH, 13.0f * HS, CatInk);
                continue;
            }

            // ---- a cvar row: [ i | name .......... | value | R ] ----
            Lur::Core::ICVar* C = It.Cv;
            const bool Overridden = C->Overridden();
            const bool HasTip = C->Tooltip()[0] != '\0';
            const float InfoS = LineH - 6.0f * HS;
            const float InfoX = IndentX + RowPad;
            const float NameX = InfoX + InfoS + 5.0f * HS;
            const float NameW = ValX - NameX - 6.0f * HS;

            if (TapPending && !TapUsed && InBand && TapX >= InfoX && TapX <= InfoX + InfoS &&
                TapY >= Sy && TapY <= Sy + LineH && HasTip) {  // "i": open the tooltip toaster
                ToastText_ = C->Tooltip(); ToastCvar_ = C; ToastAge_ = 0.0f; TapUsed = true;
            } else if (TapPending && !TapUsed && InBand && TapX >= ResetX && TapX <= ResetX + ResetS &&
                       TapY >= Sy && TapY <= Sy + LineH) {  // reset -> default + commit
                C->Reset();
                if (CvCommitFn_) CvCommitFn_(CvCommitCtx_, *C);
                if (C == SelectedCvar_) { Numpad_.Clear(); NumpadOpen_ = false; }
                TapUsed = true;
            } else if (TapPending && !TapUsed && InBand && TapX >= IndentX && TapX <= X0 + PW &&
                       TapY >= Sy && TapY <= Sy + LineH) {  // select -> open the numpad
                SelectedCvar_ = C; Numpad_.Clear(); NumpadOpen_ = true; TapUsed = true;
            }

            const bool Selected = (C == SelectedCvar_);
            // "i" button — active (accent) when a tooltip exists, greyed + inert otherwise.
            Blit(DevKeyMat, InfoX + InfoS * 0.5f, Sy + LineH * 0.5f, InfoS, InfoS);
            Text.Draw(Renderer, "i", InfoX, Sy + (LineH - InfoS) * 0.5f, InfoS, InfoS, 12.0f * HS,
                      HasTip ? Accent : DimInk, Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
            if (Selected)
                Blit(DevAccentMat, IndentX + 2.0f * HS, Sy + LineH * 0.5f, 3.0f * HS, LineH - 5.0f * HS);
            // Row label = the leaf name only (after the last '.'); the parents live in the tree.
            const char* Dot = std::strrchr(C->Name(), '.');
            const char* Label = Dot ? Dot + 1 : C->Name();
            Text.Draw(Renderer, Label, NameX, Sy, NameW, LineH, 12.5f * HS,
                      Selected ? Accent : Ink, Lur::Text::EHAlign::Left, Lur::Text::EVAlign::Middle);
            Blit(DevKeyMat, ValX + ValW * 0.5f, Sy + LineH * 0.5f, ValW, LineH - 4.0f * HS);
            char VS[64];
            if (NumpadOpen_ && Selected) std::snprintf(VS, sizeof(VS), "%s_", Numpad_.Buffer().c_str());
            else                         std::snprintf(VS, sizeof(VS), "%s", C->ValueString().c_str());
            Text.Draw(Renderer, VS, ValX + 5.0f * HS, Sy, ValW - 10.0f * HS, LineH, 12.5f * HS,
                      Overridden ? Accent : Ink, Lur::Text::EHAlign::Right, Lur::Text::EVAlign::Middle);
            Blit(DevKeyMat, ResetX + ResetS * 0.5f, Sy + LineH * 0.5f, ResetS, ResetS);
            Text.Draw(Renderer, "R", ResetX, Sy + (LineH - ResetS) * 0.5f, ResetS, ResetS,
                      12.0f * HS, Overridden ? Accent : DimInk, Lur::Text::EHAlign::Center,
                      Lur::Text::EVAlign::Middle);
        }

        // Scrollbar indicator (right edge) when the content overflows the viewport.
        if (MaxScroll > 0.0f) {
            const float TrackW = 3.0f * HS, TrackX = X0 + PW - TrackW - 1.0f * HS;
            const float ThumbH = ViewH * (ViewH / ContentH);
            const float ThumbY = ViewTop + (ViewH - ThumbH) * (ScrollY_ / MaxScroll);
            Blit(DevAccentMat, TrackX + TrackW * 0.5f, ThumbY + ThumbH * 0.5f, TrackW, ThumbH);
        }

        // The numpad: a backing panel + the 4x3 key grid (shared KeyRect geometry).
        if (NumpadOpen_) {
            Blit(DevPanelMat, NumX + NumW * 0.5f, NumY + NumH * 0.5f, NumW + 10.0f * HS,
                 NumH + 10.0f * HS);
            for (int R = 0; R < Lur::DevGui::Numpad::Rows; ++R)
                for (int C = 0; C < Lur::DevGui::Numpad::Cols; ++C) {
                    float Kx, Ky, Kw, Kh;
                    Lur::DevGui::Numpad::KeyRect(NumX, NumY, NumW, NumH, NumGap, R, C, Kx, Ky, Kw, Kh);
                    const bool Ent = Lur::DevGui::Numpad::IsEnter(R, C);
                    Blit(Ent ? DevAccentMat : DevKeyMat, Kx + Kw * 0.5f, Ky + Kh * 0.5f, Kw, Kh);
                    Text.Draw(Renderer, Lur::DevGui::Numpad::Label(R, C), Kx, Ky, Kw, Kh,
                              Ent ? 15.0f * HS : 22.0f * HS,
                              Ent ? Lur::Render::Color{0.04f, 0.07f, 0.07f, 1.0f} : Ink,
                              Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
                }
            // Cancel "x" — small, right of the top row; dismisses without writing.
            const Lur::Render::Color Warn{0.94f, 0.52f, 0.46f, 1.0f};
            Blit(DevKeyMat, CancelX + CancelS * 0.5f, CancelY + CancelS * 0.5f, CancelS, CancelS);
            Text.Draw(Renderer, "x", CancelX, CancelY, CancelS, CancelS, 12.0f * HS, Warn,
                      Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
        }

        // The tooltip toaster — a panel + the cvar's help text, anchored below/above its row.
        if (!ToastText_.empty()) {
            const float TW = PW - 20.0f * HS, TX = X0 + (PW - TW) * 0.5f;
            const float TH = 40.0f * HS;
            const float TAnchorY = RowScreenY(ToastCvar_, HeightPx * 0.4f);
            const float TY = Lur::DevGui::PlaceBelowOrAbove(TAnchorY, LineH, TH, NumGap, HeightPx);
            Blit(DevPanelMat, TX + TW * 0.5f, TY + TH * 0.5f, TW, TH);
            Blit(DevAccentMat, TX + TW * 0.5f, TY + 2.0f * HS, TW, 2.0f * HS);  // accent top edge
            Text.Draw(Renderer, ToastText_.c_str(), TX + 8.0f * HS, TY, TW - 16.0f * HS, TH,
                      12.0f * HS, Ink, Lur::Text::EHAlign::Left, Lur::Text::EVAlign::Middle);
        }

        if (TapPending) DevTapPending_.store(false, std::memory_order_release);  // one-shot
    } else {
        DevTapPending_.store(false, std::memory_order_relaxed);  // discard taps while hidden
        DevScrollAccum_.store(0.0f, std::memory_order_relaxed);
    }
#endif

    Renderer->EndFrame();
}

}  // namespace Rps
