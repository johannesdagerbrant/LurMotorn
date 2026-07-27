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

#include "Lur/Core/Assert.h"          // LUR_ASSERT_MSG — trap a row/tier decode disagreement
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
// LUR_COOK rg8-shade-coverage src=Icons/miner.png,Icons/rock.png,Icons/paper.png,Icons/scissors.png,Icons/gold.png,Icons/mine.png,Icons/swords.png,Icons/camp.png,Icons/pointer.png,Icons/oreload.png,Icons/minecamp.png,Icons/hammer.png out=View/Private/IconMasks.h ns=RpsArt size=IconSize coverage=IconCoverage shade=IconShade
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

// #142 team palette: fully-bright/saturated hues (HSV, h in [0,360)) instead of lightness shades.
Color Hsv(float H, float S, float V) {
    const float C = V * S;
    const float X = C * (1.0f - std::fabs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
    const float M = V - C;
    float R = 0, G = 0, B = 0;
    if (H < 60)       { R = C; G = X; }
    else if (H < 120) { R = X; G = C; }
    else if (H < 180) { G = C; B = X; }
    else if (H < 240) { G = X; B = C; }
    else if (H < 300) { R = X; B = C; }
    else              { R = C; B = X; }
    return {R + M, G + M, B + M, 1.0f};
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

    // #142 team palette: CYAN (team 0) & MAGENTA (team 1) — yellow is the GOLD economy colour and
    // RED is the invalid signal, so neither can be a team; magenta is cyan's vivid opposite, clear
    // of both (and of the green health bars). Each unit type is FULLY bright + saturated (no
    // lightness shading / desaturation); type is read by HUE, interpolated across the four types
    // 1/3 of the way from the team base toward blue (cyan team) / purple (magenta team). Order = EUnit.
    constexpr float TeamBaseHue[2] = {180.0f, 320.0f};  // cyan, magenta
    constexpr float TeamEndHue[2]  = {200.0f, 290.0f};  // 1/3 toward blue / purple
    TeamTint[0] = Hsv(TeamBaseHue[0], 1.0f, 1.0f);
    TeamTint[1] = Hsv(TeamBaseHue[1], 1.0f, 1.0f);
    auto AtlasTinted = [&](Color C) {
        MaterialDesc D;
        D.BaseColor = IconAtlas;
        D.Tint = C;
        return Renderer->CreateMaterial(D);
    };
    CampMat[0] = AtlasTinted(TeamTint[0]);
    CampMat[1] = AtlasTinted(TeamTint[1]);
    // buildings are knocked BACK from their units — same hue, less saturation and value. A
    // building is static scenery you place once; the units are what you actually watch, so the
    // brightest, most saturated pixels should belong to them. Hue is deliberately unchanged, so a
    // building still reads as "this team, this type" at a glance; only its intensity yields.
    // Applied via HSV rather than an alpha fade: fading toward the background washed the colour out
    // and made two teams' buildings converge on the same murky grey, losing the ownership read.
    // darker + more desaturated again (was 0.55/0.70). With the +1/+5 labels sitting ON
    // the icon, the building is deliberately a BACKDROP: knocking the art back is what makes
    // the controls read on top of it without needing a plate behind them. This is the other way to
    // solve the occlusion — recede the art rather than move the UI off it.
    constexpr float BldgSat = 0.42f;   // vs 1.0 for units
    constexpr float BldgVal = 0.55f;   // vs 1.0 for units
    TeamTintBldg[0] = Hsv(TeamBaseHue[0], BldgSat, BldgVal);
    TeamTintBldg[1] = Hsv(TeamBaseHue[1], BldgSat, BldgVal);
    for (int Tm = 0; Tm < 2; ++Tm)
        for (int Ty = 0; Ty < UnitCount; ++Ty) {
            const float Frac = static_cast<float>(Ty) / static_cast<float>(UnitCount - 1);  // 0 .. 1
            const float H = TeamBaseHue[Tm] + Frac * (TeamEndHue[Tm] - TeamBaseHue[Tm]);
            TeamTypeTint[Tm][Ty] = Hsv(H, 1.0f, 1.0f);
            TypeTintMat[Tm][Ty] = AtlasTinted(TeamTypeTint[Tm][Ty]);
            Color Dim = TeamTypeTint[Tm][Ty]; Dim.A = 0.4f;
            TypeTintMatDim[Tm][Ty] = AtlasTinted(Dim);
            TeamTypeTintBldg[Tm][Ty] = Hsv(H, BldgSat, BldgVal);
            TypeTintMatBldg[Tm][Ty] = AtlasTinted(TeamTypeTintBldg[Tm][Ty]);
        }
    // #143 pulse LUTs: the plate keeps its base colour and only rises in OPACITY (transparent ->
    // opaque); the coin glyph glows from gold toward pure white. The throb walks both.
    for (int I = 0; I < PulseSteps; ++I) {
        const float F = static_cast<float>(I) / (PulseSteps - 1);
        PulsePlate[I] = FlatMat(Renderer, {Srgb(0x1A), Srgb(0x20), Srgb(0x26), 0.40f + 0.58f * F});
        const Color G{Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f};  // gold -> white
        CoinGlow[I] = AtlasTinted({G.R + (1.0f - G.R) * F, G.G + (1.0f - G.G) * F, G.B + (1.0f - G.B) * F, 1.0f});
        // #107 press LUT: a press must be unmistakable, so unlike the pulse (which only breathes in
        // opacity on the SAME dark plate) the pressed plate goes LIGHT — the panel-light grey at a
        // rising alpha. Its own LUT, because materials are immutable and the pulse's dark base could
        // never read as "I got your touch" against a dark button.
        PressPlate[I] = FlatMat(Renderer, {Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 0.30f + 0.65f * F});
    }
    // #139 placement ghost: a translucent team-tinted silhouette while the drop is valid, and a
    // blinking red one while invalid (two alpha steps the blink alternates — materials are immutable).
    for (int Tm = 0; Tm < 2; ++Tm) {
        Color G = TeamTint[Tm]; G.A = 0.5f;
        GhostMat[Tm] = AtlasTinted(G);
    }
    GhostBadMat[0] = AtlasTinted({Srgb(0xE1), Srgb(0x4E), Srgb(0x38), 0.85f});  // red, bright
    GhostBadMat[1] = AtlasTinted({Srgb(0xE1), Srgb(0x4E), Srgb(0x38), 0.30f});  // red, dim
    ProdBtnBg = FlatMat(Renderer, {Srgb(0x1A), Srgb(0x20), Srgb(0x26), 0.62f});  // #140 translucent button
    for (int Tm = 0; Tm < 2; ++Tm)  // #141 build-frontier line in each team's colour (semi-transparent)
        FrontierMat[Tm] = FlatMat(Renderer, {TeamTint[Tm].R, TeamTint[Tm].G, TeamTint[Tm].B, 0.6f});
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

void GameView::SelectLinkedOpponent() {
    if (!Linked) return;                   // no linked row exists yet — nothing to select
    SelPeer_ = true;                       // semantic: survives the row shift on any rebuild
    if (SelectorDirty) RefreshSelector();  // builds the row AND applies the selection
    else Selector.SetSelected(PeerRow());  // the linked row is the FIRST row
}

void GameView::RefreshSelector() {
    // #2: the opponent list is the three AI tiers, plus a LINKED-opponent row WHEN a peer is
    // connected — no "searching" placeholder (a peer is either linked or simply not listed) and no
    // hot-seat row: it's an AI or a linked opponent. Each row shows its session "W-L-D" score at
    // the right end. Picking any row (re)starts/switches to that match immediately (the main polls
    // TakeAiTier / TakePeerPick). Persistent peer enumeration + cross-launch scores ride #15-20.
    // Green / amber / red / WHITE. The traffic-light run stops at red because red is as far as that
    // metaphor goes — the fourth tier is off the scale, and white reads as "not on the ladder" at a
    // glance rather than "even redder". It is also the brightest dot in the list, which is the point.
    const Color Dots[AiTierCount] = {{Srgb(0x56), Srgb(0xC1), Srgb(0x5F), 1.0f},
                                     {Srgb(0xE0), Srgb(0xB0), Srgb(0x40), 1.0f},
                                     {Srgb(0xD9), Srgb(0x53), Srgb(0x4F), 1.0f},
                                     {Srgb(0xFF), Srgb(0xFF), Srgb(0xFF), 1.0f}};
    // ORDER (feedback 2026-07-25): the LINKED opponent sits at the TOP — a human peer is the main
    // event and the AI tiers are the fallback below it — with an "AI OPPONENTS" header between
    // them. The header is non-selectable and the widget draws a divider line above any header that
    // isn't the first row, so that one row IS the requested separating line.
    Lur::Hud::DropdownItem Items[2 + AiTierCount];   // linked row + header + one per tier
    char Buf[24];
    int N = 0;
    if (Linked) {
        Items[N].Label = "Linked opponent";
        Items[N].Lead = Lur::Hud::ELeadStyle::Dot;
        // BLUETOOTH BLUE — a linked human is a different KIND of opponent, not a fourth difficulty.
        // Green/amber/red stay reserved for the AI tiers, so the dot colour alone says "radio link".
        // #0082FC is the Bluetooth SIG blue.
        Items[N].LeadFill = Color{Srgb(0x00), Srgb(0x82), Srgb(0xFC), 1.0f};
        std::snprintf(Buf, sizeof(Buf), "%d-%d-%d", PeerScoreW_, PeerScoreL_, PeerScoreD_);
        Items[N].Trailing = Buf;
        ++N;
        Items[N].Label = "AI OPPONENTS";   // separator + section label in one row
        Items[N].Header = true;
        Items[N].Lead = Lur::Hud::ELeadStyle::None;
        ++N;
    }
    for (int T = 0; T < AiTierCount; ++T, ++N) {
        Items[N].Label = AiTierName(static_cast<EAiTier>(T));
        Items[N].Lead = Lur::Hud::ELeadStyle::Dot;
        Items[N].LeadFill = Dots[T];
        std::snprintf(Buf, sizeof(Buf), "%d-%d-%d", AiScoreW_[T], AiScoreL_[T], AiScoreD_[T]);
        Items[N].Trailing = Buf;
    }
    Selector.SetItems(Items, N);
    // Re-point at the SAME OPPONENT, not the same row number: the rows shift by two the moment the
    // linked row appears, so preserving the index would silently move the selection to a different
    // opponent (and could land it on the header).
    Selector.SetSelected(SelPeer_ && Linked ? PeerRow() : AiRow(SelAiTier_));
    SelectorDirty = false;
}

void GameView::SetAiScore(int Tier, int W, int L, int D) {
    if (Tier < 0 || Tier >= AiTierCount) return;
    if (AiScoreW_[Tier] == W && AiScoreL_[Tier] == L && AiScoreD_[Tier] == D) return;
    AiScoreW_[Tier] = W; AiScoreL_[Tier] = L; AiScoreD_[Tier] = D;
    SelectorDirty = true;
}
void GameView::SetPeerScore(int W, int L, int D) {
    if (PeerScoreW_ == W && PeerScoreL_ == L && PeerScoreD_ == D) return;
    PeerScoreW_ = W; PeerScoreL_ = L; PeerScoreD_ = D;
    SelectorDirty = true;
}
void GameView::NotifyPeerLinked() { PeerLinkBannerT_ = 4.0f; }  // #2: blink the bar for ~4 s

#if !LUR_SHIPPING
namespace {
// Every AffectsGameplay cvar as (parent-path, cvar) pairs, SORTED by name — the input to the
// hierarchical tree (#121). The DOTTED NAME is the hierarchy: the parent path is the name up to
// the last '.', so "rps.boid.sep_strength" nests under rps -> boid and the row shows only
// "sep_strength" (the leaf, after the last dot). There is no separate category. Name-sorted
// input means each node's leaves come out name-sorted (BuildCategoryTree preserves leaf order),
// so the layout is stable frame to frame.
// EVERY registered CVar, not just the AffectsGameplay ones (#156). The filter was invisible for as
// long as every CVar in the repo was a sim tunable; the first dev-only knob (rps.dev.flight_recorder)
// would simply not have appeared. Non-gameplay CVars nest into the same tree by name — they differ
// only in that a commit takes the local+persist path instead of the lockstep sync, which is decided
// per-CVar by the commit hook (AffectsGameplay), not by what the console chooses to list.
std::vector<std::pair<std::string, Lur::Core::ICVar*>> GatherAllCvars() {
    std::vector<std::pair<std::string, Lur::Core::ICVar*>> Items;
    Lur::Core::CVarRegistry::ForEach([&](Lur::Core::ICVar* C) {
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
        // A settled selection on an AI row starts a single-player match at that tier (#127); the
        // main polls TakeAiTier(). Peer/same-device rows have no target yet (#85 follow-up).
        // TookSelection() is the one-shot latch.
        if (Selector.TookSelection()) {
            // Decode the ROW back to an opponent. The AI rows sit below the linked row + header
            // when a peer is up, so the offset is not fixed (headers are never selectable, so the
            // separator row can't arrive here).
            const int Sel = Selector.Selected();
            if (Linked && Sel == PeerRow()) {
                SelPeer_ = true;
                PeerRowPicked_ = true;                                // #2: switch to the peer match
            } else {
                const int Tier = Sel - AiRow(0);
                // Bound by AiTierCount, NEVER a literal. This read "Tier <= 2" and it is exactly
                // how a fourth tier ships broken: the row was DRAWN and the widget highlighted it,
                // but the pick was dropped here, so the match kept running the previous tier and
                // the next list rebuild snapped the highlight back to SelAiTier_ — "I picked
                // Perhaps Impossible, it played like Easy, then the menu said Easy again".
                // Headers are not selectable and the peer row is handled above, so a settled
                // selection that is NOT a valid tier means the row table and this decode disagree.
                // Trap it instead of silently ignoring the tap — silence is what shipped a dropdown
                // row that did nothing.
                LUR_ASSERT_MSG(Tier >= 0 && Tier < AiTierCount,
                               "selector row %d decoded to tier %d (AiTierCount=%d)", Sel, Tier,
                               AiTierCount);
                if (Tier >= 0 && Tier < AiTierCount) {
                    SelPeer_ = false;
                    SelAiTier_ = Tier;
                    AiTierPicked_ = Tier;                             // #2: (re)start solo at this tier
                }
            }
        }
        return -2;
    }
    for (int Ty = 0; Ty < 4; ++Ty) {
        if (PlateLocked[Ty]) continue;  // §9: a locked plate is unselectable, not just un-droppable
        const float* Rc = PlateRect[Ty];
        if (XPx >= Rc[0] && XPx <= Rc[0] + Rc[2] && YPx >= Rc[1] && YPx <= Rc[1] + Rc[3])
            return Ty;
    }
    return -1;
}

int GameView::PlateAt(float XPx, float YPx) const {
    if (!Ready) return -1;
    for (int Ty = 0; Ty < 4; ++Ty) {
        // A LOCKED plate is not a drag source at all (§9 opening gate): report a miss so the press
        // falls through as a world tap instead of starting a drag that could never be dropped.
        if (PlateLocked[Ty]) continue;
        const float* Rc = PlateRect[Ty];
        if (XPx >= Rc[0] && XPx <= Rc[0] + Rc[2] && YPx >= Rc[1] && YPx <= Rc[1] + Rc[3]) return Ty;
    }
    return -1;
}

void GameView::BeginPlaceDrag(int Type, float XPx, float YPx) {
    GhostType_ = Type;
    GhostDragging_ = true;
    GhostValid_ = false;
    GhostXPx_ = XPx; GhostYPx_ = YPx;  // seed at the finger so frame 1 isn't at a stale spot
    GhostDesiredX_ = XPx; GhostDesiredY_ = YPx;
    GhostPushX_.Snap(0.0f);            // no obstacle push yet; start with zero offset, not last drag's
    GhostPushY_.Snap(0.0f);
    SlideT_ = -1.0f;  // cancel any in-flight slide-back
}

void GameView::SetPlacedPreview(int Type, float Wx, float Wy, bool Active) {
    PreviewType_ = Type; PreviewWx_ = Wx; PreviewWy_ = Wy; PreviewActive_ = Active;
}

int GameView::OnProductionButton(float XPx, float YPx, int32_t& OutSlot) const {
    for (int B = 0; B < ProdBtnCount_; ++B)
        for (int K = 0; K < ProdBtnPerBldg; ++K) {
            const float* R = ProdBtns_[B].R[K];
            if (R[2] > 0.0f && XPx >= R[0] && XPx <= R[0] + R[2] && YPx >= R[1] && YPx <= R[1] + R[3]) {
                OutSlot = ProdBtns_[B].Slot;
                return ProdMult[K];
            }
        }
    return 0;
}

bool GameView::PressProductionButton(float XPx, float YPx) {
    for (int B = 0; B < ProdBtnCount_; ++B)
        for (int K = 0; K < ProdBtnPerBldg; ++K) {
            const float* R = ProdBtns_[B].R[K];
            if (R[2] > 0.0f && XPx >= R[0] && XPx <= R[0] + R[2] && YPx >= R[1] && YPx <= R[1] + R[3]) {
                PressSlot_.store(ProdBtns_[B].Slot, std::memory_order_relaxed);
                PressBtn_.store(K, std::memory_order_relaxed);
                PressPending_.store(true, std::memory_order_release);  // publishes both stores
                return true;
            }
        }
    return false;
}

void GameView::UpdatePlaceDrag(float XPx, float YPx, bool Valid) {
    if (GhostType_ < 0) return;
    GhostXPx_ = XPx; GhostYPx_ = YPx; GhostValid_ = Valid;
    GhostDesiredX_ = XPx; GhostDesiredY_ = YPx;   // no separate desired point given: no push to spring
}

void GameView::UpdatePlaceDrag(float DesXPx, float DesYPx, float ResXPx, float ResYPx, bool Valid) {
    if (GhostType_ < 0) return;
    GhostDesiredX_ = DesXPx; GhostDesiredY_ = DesYPx;   // where the finger is
    GhostXPx_ = ResXPx; GhostYPx_ = ResYPx;             // where the sim would accept it
    GhostValid_ = Valid;
}

void GameView::EndPlaceDrag(bool Placed) {
    if (GhostType_ < 0) return;
    GhostDragging_ = false;
    if (Placed) { GhostType_ = -1; SlideT_ = -1.0f; }  // placed: the real building takes over
    else { SlideT_ = 0.0f; SlideFromX_ = GhostXPx_; SlideFromY_ = GhostYPx_; }  // invalid: slide home
}

void GameView::ScreenToWorld(float XPx, float YPx, float CameraY, float WidthPx, float HeightPx,
                             bool FlipY, float& OutWx, float& OutWy) const {
    const float P = Ppu(WidthPx);
    OutWx = XPx / P;
    const float Fy = CameraY + (HeightPx - YPx) / P;  // inverse of SY
    OutWy = FlipY ? FW(WorldHeight) - Fy : Fy;
}

void GameView::WorldToScreen(float Wx, float Wy, float CameraY, float WidthPx, float HeightPx,
                             bool FlipY, float& OutXPx, float& OutYPx) const {
    const float P = Ppu(WidthPx);
    OutXPx = Wx * P;
    const float Fy = FlipY ? FW(WorldHeight) - Wy : Wy;   // undo the per-player flip
    OutYPx = HeightPx - (Fy - CameraY) * P;               // inverse of ScreenToWorld's Fy step
}

bool GameView::ResolvePlacement(float DesXPx, float DesYPx, float CameraY, float WidthPx,
                                float HeightPx, bool FlipY, const Snapshot& Snap, uint8_t Team,
                                float& OutWx, float& OutWy, float& OutGhostXPx,
                                float& OutGhostYPx) const {
    float Dx = 0.0f, Dy = 0.0f;
    ScreenToWorld(DesXPx, DesYPx, CameraY, WidthPx, HeightPx, FlipY, Dx, Dy);
    // Snap radius ≈ the building's on-field icon size (footprint diameter, world units).
    const float Radius = FW(Snap.Cv.BuildingFootprint) * 2.0f;
    const bool Valid = Snap.SnapToValidPlace(Team, static_cast<uint8_t>(GhostType_), Dx, Dy, Radius,
                                             OutWx, OutWy);
    if (Valid) WorldToScreen(OutWx, OutWy, CameraY, WidthPx, HeightPx, FlipY, OutGhostXPx, OutGhostYPx);
    else { OutGhostXPx = DesXPx; OutGhostYPx = DesYPx; }  // no valid spot -> red blink at the finger-offset
    return Valid;
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
    HealthBars_.clear();   // refilled each frame by the building/unit passes below
    PulseBtnActive_ = false;   // set below if a +1 button is pulsing this frame

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

    // #135/#139: camps are no longer fixed locations — each team's mining camp is a PLACED
    // building (rendered in the entity loop below, tinted the owner's team colour). Nothing is
    // drawn here pre-placement; the pre-match camera sits at the player's baseline (§9.1).

    // Mines — finite (#84): a depleted mine is gone; live ones carry a gold reserve
    // bar above them (same visual language as unit health, gold fill).
    // #157: drawn mine diameter is a CVar. Read from the GLOBAL, not the sim snapshot, and that is
    // deliberate — it is render-only (not AffectsGameplay), so it is never latched into Cv, never
    // hashed, and never synced; putting it in the snapshot would imply the sim cares. It also means
    // an edit shows up on the very next frame with no match restart, unlike the map knobs.
    const float MinePx = FW(Rps::CvMineVisualSize.Get()) * P;
    for (int T = 0; T < NumMines; ++T) {
        if (Snap.MineGold[T] <= 0) continue;
        const float Mx = SX(FW(Snap.MineX[T])), My = SY(FW(Snap.MineY[T]));
        BlitGlyph(GlyphMine, MineMat, Mx, My, MinePx);
        const float Frac = static_cast<float>(Snap.MineGold[T]) / static_cast<float>(MineGoldCapacity);
        const float BarW = MinePx, BarH = 2.0f * HS, BarY = My - MinePx * 0.5f - 3.0f * HS;
        Blit(HealthBg, Mx, BarY, BarW, BarH);
        Blit(GoldBarFg, Mx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH);
    }

    // #141 build-frontier lines: a dotted horizontal line in EACH team's colour at its high-water
    // Y — your own build boundary AND the opponent's, as intel — with a "build up to here" marker
    // at the left end. View-only (reads Snap frontier, writes nothing, not hashed). SY handles the
    // per-player flip, so each side sees its own line toward the bottom. (Marker is a placeholder
    // for a hammer icon — pending a sourced glyph.)
    {
        const float Dash = 7.0f * HS, DashGap = 6.0f * HS, Thick = 2.0f * HS;
        const float Target[2] = {FW(Snap.FrontierT0), FW(Snap.FrontierT1)};
        // Latch the last TWO ticks' frontier per team. Forward motion is then interpolated across the
        // tick with the same Alpha the units use, instead of being re-snapped every frame: the sim
        // only moves this line at 10 Hz, so snapping made it visibly step (the reported forward
        // jitter) while the units that pushed it glided. Riding the same Alpha is still
        // gameplay-exact in the sense that matters — the line advances exactly with the units whose
        // positions define it, no spring, no lag of its own.
        if (!FrontierHaveTick_ || Snap.Tick != FrontierTick_) {
            for (int T = 0; T < 2; ++T) {
                FrontierPrev_[T] = FrontierHaveTick_ ? FrontierCur_[T] : Target[T];
                FrontierCur_[T] = Target[T];
            }
            FrontierTick_ = Snap.Tick;
            FrontierHaveTick_ = true;
        }
        auto DrawFrontier = [&](int Team) {
            // ADVANCE tracks the sim exactly (interpolated across the tick, like every unit);
            // RETRACTION is sprung. Ground GAINED must read as immediate and show precisely where you
            // may build. Ground LOST is the opposite problem: it happens when a far-forward unit of
            // yours dies, often off-screen, and an instant jump is simply missed. A double spring is
            // the right shape for that (theorangeduck.com's spring roll call): it eases IN, so the
            // line visibly gives way instead of leaving at full speed, and eases out onto the new
            // boundary without passing it.
            //
            // Visual only: the sim's frontier is the authority for every placement test and this
            // float never re-enters it, so two peers may draw the retraction at different moments and
            // stay bit-identical.
            const float Gained = Team == 0 ? FrontierCur_[Team] - FrontierPrev_[Team]
                                           : FrontierPrev_[Team] - FrontierCur_[Team];
            if (Gained > 0.0f) Retracting_[Team] = false;
            else if (Gained < 0.0f) Retracting_[Team] = true;
            // Gained == 0 holds the current mode, so a retraction already in flight is not cut short
            // by the next (unchanged) tick — which is what killed it when the test was "not forward".
            float Line;
            if (Retracting_[Team]) {
                FrontierSpring[Team].Update(FrontierCur_[Team], FrontierRetractHalflife, DtSec);
                Line = FrontierSpring[Team].X;
                const float Left = Line - FrontierCur_[Team];
                if (Left * Left < 0.01f) Retracting_[Team] = false;   // arrived: hand back to advance
            } else {
                Line = FrontierPrev_[Team] + (FrontierCur_[Team] - FrontierPrev_[Team]) * Alpha;
                FrontierSpring[Team].Snap(Line);   // keep the spring parked here for the next retract
            }
            const float Y = SY(Line);
            if (Y < -2.0f || Y > HeightPx + 2.0f) return;
            for (float X = 32.0f * HS; X < WidthPx - 2.0f * HS; X += Dash + DashGap)
                Blit(FrontierMat[Team], X + Dash * 0.5f, Y, Dash, Thick);
            BlitGlyph(GlyphHammer, CampMat[Team], 17.0f * HS, Y, 22.0f * HS);  // "build up to here" legend
        };
        DrawFrontier(0);
        DrawFrontier(1);
    }

    // Units — ONE instanced draw. Each instance carries prev+cur pixel centres; the
    // vertex shader lerps by Alpha, so there is no per-unit CPU interpolation (design §6).
    // We map prev/cur to pixels here (a cheap affine per unit, not a lerp).
    // Design lock: units are bare alpha-cutout SILHOUETTES tinted by team — type is
    // the shape, ownership is the colour. Per-instance UV rect picks the glyph.
    const float UnitPx = 1.7f * P;
    uint32_t N = 0;
    int32_t Workers = 0, Soldiers = 0;  // viewer-team split for the population counter
    const float BldgPx = FW(Snap.Cv.BuildingFootprint) * 2.3f * P;  // #139/#140: a bit bigger than the
                                                                 //   footprint so the slim buttons fit inside
    // TWO passes — every building first, then every unit — so units always render ON TOP of
    // buildings. This is a single instanced draw with no depth buffer, so the order instances sit in
    // the buffer IS the layer order; slots are allocated as things are placed/spawned, so a building
    // that happened to take a later slot drew over units standing on it, and a cart working a camp
    // could vanish behind it. Two passes cost one extra loop over Count and still emit ONE draw.
    for (int Pass = 0; Pass < 2; ++Pass)
    for (int32_t I = 0; I < Snap.Count && N < static_cast<uint32_t>(MaxUnits); ++I) {
        if (!Snap.IsAlive(I)) continue;
        const uint8_t Ty = Snap.Type[I], Tm = Snap.Team[I];
        const bool Bldg = Snap.IsBuilding(I);  // #139: buildings are static, footprint-sized entities
        if (Bldg != (Pass == 0)) continue;     // pass 0 = buildings (under), pass 1 = units (over)
        const bool Home = Snap.IsHomeBase(I);  // #146: the HQ (Type is UnitNone — colour/glyph by Kind)
        if (Tm == My && !Bldg) { if (Ty == UnitMiner) ++Workers; else ++Soldiers; }  // buildings aren't army
        // ABSOLUTE team colours (playtest: the players sit together and compare
        // screens — team 0 is blue and team 1 red on BOTH phones, so a unit looks
        // the same wherever you see it), now a UNIQUE per-type shade of that team hue
        // so composition reads by colour as well as glyph. HUD numbers stay viewer-relative.
        // #146: the home base wears the base team hue (its Type would index out of the per-type table).
        // the BUILDING variants are the knocked-back (lower sat/value) shades of the same hues.
        // (the HQ is itself a building — Kind != KindUnit — so it always takes the knocked-back tone)
        const Color C = Home ? TeamTintBldg[Tm]
                             : (Bldg ? TeamTypeTintBldg[Tm][Ty] : TeamTypeTint[Tm][Ty]);
        Lur::Render::InstanceData& D = Instances[N++];
        D.PrevX = SX(FW(Snap.PrevX[I])); D.PrevY = SY(FW(Snap.PrevY[I]));
        D.CurX = SX(FW(Snap.PosX[I]));   D.CurY = SY(FW(Snap.PosY[I]));
        D.R = C.R; D.G = C.G; D.B = C.B; D.A = C.A;
        // The HOME BASE wears the (distinct) camp/fortress glyph, bigger; a miner BUILDING wears the
        // mine-camp glyph; other buildings their (bigger) type glyph; units their type glyph.
        const int Glyph = Home ? static_cast<int>(GlyphCamp)
                               : (Bldg && Ty == UnitMiner ? static_cast<int>(GlyphMineCamp)
                                                          : static_cast<int>(Ty));
        D.Size = Home ? BldgPx * 1.6f
                      : (Bldg ? BldgPx : (Ty == UnitMiner ? UnitPx * 1.5f : UnitPx));  // carts read bigger (playtest)
        D.U0 = static_cast<float>(Glyph) / static_cast<float>(GlyphCount); D.V0 = 0.0f;
        D.U1 = static_cast<float>(Glyph + 1) / static_cast<float>(GlyphCount); D.V1 = 1.0f;
        // Facing (soldiers only; carts + buildings stay upright): the glyph's TOP points along the
        // MOVE direction. Below a low speed we DON'T update the stored heading — a nearly
        // stopped unit holds its last angle instead of spinning on sub-pixel noise.
        D.FaceX = 0.0f; D.FaceY = 0.0f;
        if (!Bldg && Ty != UnitMiner) {
            const float Vx = D.CurX - D.PrevX, Vy = D.CurY - D.PrevY;
            const float Sp = std::sqrt(Vx * Vx + Vy * Vy);
            if (Sp > 0.12f * P) { LastFaceX[I] = Vx / Sp; LastFaceY[I] = Vy / Sp; }  // fast: update
            D.FaceX = LastFaceX[I]; D.FaceY = LastFaceY[I];                          // else hold
        }
        // A LOADED cart shows its ore heap in COIN gold (playtest): one extra
        // instance, same endpoints, the heap glyph over the cart — still one draw.
        if (!Bldg && Ty == UnitMiner && Snap.Carry[I] > 0 && N < static_cast<uint32_t>(MaxUnits)) {
            Lur::Render::InstanceData& O = Instances[N++];
            O = D;  // same size: the enlarged heap is baked into the mask, seated on the rail
            O.R = Srgb(0xD9); O.G = Srgb(0xA9); O.B = Srgb(0x3C); O.A = 1.0f;
            O.U0 = static_cast<float>(GlyphOreLoad) / static_cast<float>(GlyphCount);
            O.U1 = static_cast<float>(GlyphOreLoad + 1) / static_cast<float>(GlyphCount);
        }
    }
    Renderer->DrawInstances(Quad, Instances, N, Alpha, AtlasMat);

    // #139/§9: a just-placed building shown view-only until the sim reflects it — the pre-match
    // camp appears the instant you drop it (not after the opponent readies), at the exact world
    // spot the real building will land, so there's no jump when the match starts.
    if (PreviewActive_ && PreviewType_ >= 0 && PreviewType_ < UnitCount) {
        const int PG = PreviewType_ == UnitMiner ? static_cast<int>(GlyphMineCamp) : PreviewType_;
        // building tone — this preview stands in for a real building, so it must not be
        // brighter than the thing it becomes (that read as a colour change on match start).
        BlitGlyph(PG, TypeTintMatBldg[My][PreviewType_], SX(PreviewWx_), SY(PreviewWy_), BldgPx);
    }

    // #140 per-building UI, present on EVERY local building all the time. Each shows: a HEALTH bar
    // ABOVE; the +1/+5 buttons on a diagonal INSIDE the icon (bottom-left / top-right) with the unit
    // price in the freed bottom-right corner; and — while producing — an "N/max" queue
    // count + next-unit PROGRESS bar BELOW. Buttons dim when unaffordable; their rects are captured
    // for OnProductionButton (tapped on the input thread). All world-anchored, so they scroll along.
    {
        using Lur::Text::EHAlign;
        using Lur::Text::EVAlign;
        const Color Ico{Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 1.0f};
        const Color GoldC{Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f};
        const Color DimC{Srgb(0x6A), Srgb(0x72), Srgb(0x78), 1.0f};
        const float Half = BldgPx * 0.5f;
        // with the translucent plates gone, every label sits directly on the building's art, so
        // each one gets a dark offset copy behind it. That is what the plates were actually for
        // (gold-on-cyan was unreadable) — the legibility problem outlives the panel, so it needs its
        // own answer. Two text draws for a handful of short labels; no new material or pipeline.
        const Color ShadowC{0.0f, 0.0f, 0.0f, 0.75f};
        auto TextShadowed = [&](const char* S, float X, float Y, float W, float H, float Px, Color C,
                                EHAlign HA) {
            const float O = 1.5f * HS;   // offset: enough to separate, small enough not to smear
            Text.Draw(Renderer, S, X + O, Y + O, W, H, Px, ShadowC, HA, EVAlign::Middle, false);
            Text.Draw(Renderer, S, X, Y, W, H, Px, C, HA, EVAlign::Middle, false);
        };
        // Playtest 2026-07-25: the x1/x5 pair is a horizontal row of big squares, not a slim stacked
        // column. ONE width (CtrlW) governs the whole control stack — cost plate, button row, queue
        // plate — so every outline down the building lines up instead of three ragged widths, and the
        // buttons are exactly as wide as the progress bar they sit above. Height stays generous:
        // ~120 px on this phone, so the tap target is comfortable even though the width is now
        // dictated by alignment rather than by the thumb.
        const float BGap = 6.0f * HS;
        const float CtrlW = BldgPx + 6.0f * HS;
        const float Bw = (CtrlW - BGap) / static_cast<float>(ProdBtnPerBldg);
        const float Bh = 48.0f * HS;   // a little bigger — bigger target AND a bigger label
        const float LabelPx = 31.0f * HS;   // the "+N" label, sized once
        // the price row moved from ABOVE the icon to BELOW it, immediately above the progress
        // bar. One constant for its height so the price and the queue/progress row below it are
        // positioned from the same number and cannot drift into each other.
        const float PriceRowH = 15.0f * HS;
        // The queue/progress row's height, hoisted out of its own block: the price is now placed
        // FROM this number (it tucks into the icon's bottom-right, just clear of the row), so both
        // must read it from the same place or they drift apart the moment one is retuned.
        const float QRowH = 20.0f * HS;
        ProdBtnCount_ = 0;
        PulseT_ += DtSec;              // #143 production-pulse throb clock
        // #107: latch a press stamped by the input thread, else age the flash out. Reading it here
        // (once per frame, before the buttons draw) means the acknowledgement lands on the SAME
        // frame as the touch — the queue itself still waits out the input delay, as it must.
        if (PressPending_.exchange(false, std::memory_order_acquire)) {
            FlashSlot_ = PressSlot_.load(std::memory_order_relaxed);
            FlashBtn_ = PressBtn_.load(std::memory_order_relaxed);
            FlashT_ = PressFlashSec;
        } else if (FlashT_ > 0.0f) {
            FlashT_ = std::max(0.0f, FlashT_ - DtSec);
        }

        // #139/feedback: your committed camp while the opponent hasn't placed theirs yet. It is NOT
        // in the sim (both camps land together as tick 0's input), so without this the field read as
        // empty and the drop looked lost. Same glyph + team tint as a real camp; deliberately NO
        // production buttons (those come from the sim's buildings, and nothing can be queued before
        // the match starts) and no health bar (it has no HP yet). Gently pulsed so it reads as
        // "pending", not "built".
        if (PendingCamp_) {
            const float Px = SX(PendingCampX_), Py = SY(PendingCampY_);
            const float Breathe = 0.90f + 0.10f * std::sin(PulseT_ * 3.0f);
            // the BUILDING tone, matching what it turns into — a brighter pending camp
            // would visibly dim the instant the match started and the real building took over.
            BlitGlyph(static_cast<int>(GlyphMineCamp), TypeTintMatBldg[My][UnitMiner], Px, Py,
                      BldgPx * Breathe);
        }
        bool FirstLocalSeen = false;   // the first (lowest-slot) local building = the camp
        for (int32_t I = 0; I < Snap.Count; ++I) {
            if (!Snap.IsAlive(I) || !Snap.IsBuilding(I)) continue;
            const bool Home = Snap.IsHomeBase(I);         // #146: the HQ — health bar only, no production
            const uint8_t Bty = Snap.Type[I];
            const float BSize = Home ? BldgPx * 1.6f : BldgPx;   // matches the instanced-draw size
            const float BHalf = BSize * 0.5f;
            const float Bx = SX(FW(Snap.PosX[I])), By = SY(FW(Snap.PosY[I]));
            if (By < -BHalf - 60.0f * HS || By > HeightPx + BHalf + 60.0f * HS) continue;  // off-screen

            // Health bar ABOVE the structure (all — read enemy siege progress too; the HOME BASE bar
            // is the win meter). Scale to the right max: the home base has its own HP, not a per-type one.
            const int32_t MaxHp = Home ? (Snap.HomeBaseMaxHp > 0 ? Snap.HomeBaseMaxHp : 1)
                                       : (Snap.BuildingMaxHp[Bty] > 0 ? Snap.BuildingMaxHp[Bty] : 1);
            const float HFrac = std::min(1.0f, std::max(0.0f, static_cast<float>(Snap.Hp[I]) / static_cast<float>(MaxHp)));
            const float HbW = BSize * 0.85f, HbH = 3.0f * HS, HbY = By - BHalf - 5.0f * HS;
            // collected, not drawn here — flushed in the GUI layer so the instanced units
            // (drawn after this pass) cannot cover a building's own bar.
            HealthBars_.push_back({HealthBg, Bx, HbY, HbW, HbH});
            HealthBars_.push_back({HealthFg, Bx - HbW * 0.5f + HbW * HFrac * 0.5f, HbY, HbW * HFrac, HbH});

            if (Home) continue;                // #146: the HQ produces nothing — no x1/x5 buttons/queue
            if (Snap.Team[I] != My) continue;  // production controls: your buildings only
            // #143: the FIRST building (the camp) teaches production — its buttons pulse until
            // anything is queued anywhere on it, then it's taught for the rest of the session.
            const bool IsFirstBldg = !FirstLocalSeen;
            FirstLocalSeen = true;
            if (IsFirstBldg && Snap.Queue[I] > 0) ProductionTaught_ = true;
            const bool Pulse = IsFirstBldg && Snap.Queue[I] == 0 && !ProductionTaught_;

            // "N/max" (left) + next-unit PROGRESS bar (right), a row centred UNDER the building.
            if (Snap.Queue[I] > 0) {
                const float QW = 34.0f * HS, RGap = 4.0f * HS;
                // JUST below the icon. The price moved up onto the icon's lower end, so this row
                // no longer has to clear it — it tucks straight under the bottom edge, and the whole
                // per-building stack gets shorter (less chance of reaching the building below).
                // up again, so the row sits FULLY over the icon's bottom rather than under it.
                // Row is 20*HS tall, so a centre one half-height above the bottom edge puts its whole
                // height inside the icon. Derived from the edge, not a magic offset, so it stays put
                // if the icon size changes.
                // Centre ON the icon's bottom edge: the row straddles it, half over the art and half
                // below. (This is the "fully inside" position moved back down by half a row height —
                // the two cancel, so it is simply the edge.)
                const float RowY = By + Half;
                const float GroupL = Bx - BldgPx * 0.5f;
                // Its own translucent plate (playtest): the count and the bar sit over open field or
                // over mine art, and on gold they were unreadable. Same plate as the buttons, so the
                // building's controls read as one family.
                Blit(ProdBtnBg, Bx, RowY, CtrlW, QRowH);   // one height, shared with RowY above
                char QB[16];
                std::snprintf(QB, sizeof(QB), "%d/%d", Snap.Queue[I], Snap.BuildingQueueMax);
                Text.Draw(Renderer, QB, GroupL, RowY - 7.0f * HS, QW, 14.0f * HS, 12.0f * HS, Ico,
                          EHAlign::Right, EVAlign::Middle, false);
                const int32_t Bt = Snap.Units[Bty].BuildTicks > 0 ? Snap.Units[Bty].BuildTicks : 1;
                const float PFrac = std::min(1.0f, static_cast<float>(Snap.BuildProgress[I]) / static_cast<float>(Bt));
                const float PbW = BldgPx - QW - RGap, PbH = 4.0f * HS;
                const float PbX = GroupL + QW + RGap + PbW * 0.5f;
                Blit(BarBg, PbX, RowY, PbW, PbH);
                if (PbW * PFrac > 0.5f)
                    Blit(GoldFlat, PbX - PbW * 0.5f + PbW * PFrac * 0.5f, RowY, PbW * PFrac, PbH);
            }
            // Playtest 2026-07-25: the cost is stated ONCE, centred above the icon, as the price of
            // ONE unit — it used to be repeated inside every button as that button's total, which
            // made the plates wordy AND small. The buttons below it then carry only the multiplier,
            // so they can be big: a pair side by side, sized for a thumb rather than a mouse.
            if (ProdBtnCount_ >= MaxProdButtons) continue;
            ProdButtons& PB = ProdBtns_[ProdBtnCount_++];
            PB.Slot = I;
            const int32_t UnitCost = Snap.Units[Bty].Cost;
            const bool AffordOne = Snap.Gold[My] >= UnitCost;
            // Layout ON the building: health bar above the icon, then the four quadrants of the icon
            // itself — +5 top-right, +1 bottom-left, the price of ONE unit bottom-right — and the
            // queue + progress row straddling the bottom edge. Everything is inside the building's own
            // footprint, so a cluster never lands on a neighbour's art — which is what happened when
            // the price floated above the icon.
            // The buttons no longer straddle the icon's edges — they sit FULLY INSIDE the footprint,
            // on a DIAGONAL: +1 in the bottom-left, +5 in the top-right. One column per button
            // (ColW = footprint / count), and the row alternates with K, which is what buys them their
            // height: Bh is ~2/3 of the icon, so two buttons could never share a column, but on a
            // diagonal each one's vertical overrun lands in the *other* button's column. Nothing
            // leaves the footprint, so a cluster can never reach a neighbour's art — the failure mode
            // the edge-straddling pair had — and the freed corner is where the price goes.
            const float ColW = 2.0f * Half / static_cast<float>(ProdBtnPerBldg);
            {
                // WAY smaller, and NO plate. The plates were the real occluders — the building's
                // art was legible through a glyph but not through three stacked translucent panels.
                // The price is reference information, not a control, so it yields the most space —
                // which is why it takes the ONE corner the diagonal button pair leaves free:
                // BOTTOM-RIGHT. +1 owns the bottom-left column and +5 the top-right one, so anything
                // on the centre line would be under a thumb target; the bottom-right band is the only
                // quiet spot left inside the footprint.
                // Vertically it is measured UP from the icon's bottom edge, clearing the queue/
                // progress row that straddles that edge (QRowH is shared with it, so they can't drift).
                const float CostY = (By + Half) - QRowH * 0.5f - PriceRowH * 0.5f;
                const float Cs = 11.0f * HS;
                char CBuf[12];
                std::snprintf(CBuf, sizeof(CBuf), "%d", UnitCost);
                // Right-anchored as a GROUP: the coin is a fixed step left of the digits and the digits
                // run left-to-right from there, so a 1-, 2- or 3-digit price keeps the coin still (the
                // eye finds the currency in the same place on every building) and the widest of them
                // still lands inside the icon's right half — 31*HS back from the right edge leaves
                // room for coin + ~3 digits, and Half is ~36.5*HS at every resolution.
                const float GlyphX = Bx + Half - 31.0f * HS;
                BlitGlyph(GlyphGold, AffordOne ? GoldIconMat : PlateIconDim, GlyphX, CostY, Cs);
                // A dark offset copy behind the text replaces the plate: the plate existed because
                // gold-on-cyan was unreadable, and that problem does not go away just because the
                // panel did. One extra text draw per building, and it works over any art.
                TextShadowed(CBuf, GlyphX + 8.0f * HS, CostY - 7.0f * HS, 40.0f * HS, 14.0f * HS,
                             12.0f * HS, AffordOne ? GoldC : DimC, EHAlign::Left);
            }
            // #143/#146 production pulse (the first camp only, until taught): ONLY the x1 button
            // throbs — right after the first camp x5 is unaffordable, so pulsing it would beg for a
            // buy the player can't make. The plate + "x1"/price brighten on the beat; the gold COIN
            // stays gold always (it reads as the currency, not a flashing element).
            for (int K = 0; K < ProdBtnPerBldg; ++K) {
                const bool BtnPulse = Pulse && K == 0;   // x1 only (x5 too dear at the first camp)
                const float Throb = BtnPulse ? 0.5f + 0.5f * std::sin(PulseT_ * 6.0f) : 0.0f;
                // #107 press flash: this button was just pressed -> DEPRESS it (scale in, against
                // the pulse's scale out) and flash the plate bright. 1 at the touch, 0 when spent.
                const float Press = (FlashT_ > 0.0f && FlashSlot_ == I && FlashBtn_ == K)
                                        ? FlashT_ / PressFlashSec : 0.0f;
                const float PulseK = (1.0f + 0.16f * Throb) * (1.0f - 0.18f * Press);
                const float Lift = Press > Throb ? Press : Throb;   // brightness: the stronger of the two
                const int PulseStep = static_cast<int>(Lift * (PulseSteps - 1) + 0.5f);
                // Column K of the footprint, Bw centred in it (they are equal today — CtrlW - BGap is
                // exactly BldgPx — but centring means a retuned BGap narrows the buttons instead of
                // pushing the last one out through the right edge).
                const float BX = Bx - Half + ColW * static_cast<float>(K) + (ColW - Bw) * 0.5f;
                // Even K hugs the icon's BOTTOM edge, odd K its TOP edge — the diagonal.
                const float BtnTop = (K % 2 == 0) ? (By + Half - Bh) : (By - Half);
                PB.R[K][0] = BX; PB.R[K][1] = BtnTop; PB.R[K][2] = Bw; PB.R[K][3] = Bh;  // hit rect: unscaled
                if (BtnPulse) {   // remember it for the GUI-layer pointing hand
                    PulseBtnActive_ = true;
                    for (int R4 = 0; R4 < 4; ++R4) PulseBtnRect_[R4] = PB.R[K][R4];
                }
                const int32_t Price = UnitCost * ProdMult[K];
                const bool Afford = Snap.Gold[My] >= Price;
                // Draw everything about the button's CENTRE, scaled by PulseK (1.0 unless pulsing).
                const float Cx = BX + Bw * 0.5f, Cy = BtnTop + Bh * 0.5f;
                const float bw = Bw * PulseK, bh = Bh * PulseK, bx = Cx - bw * 0.5f, by2 = Cy - bh * 0.5f;
                // NO plate. It was carrying three jobs — persistent background, #107 press
                // flash, #143 onboarding throb — so removing it means the feedback moves onto the
                // GLYPH. The hit rect above is untouched, so the button is exactly as easy to hit as
                // before; only the paint is gone. (void) the step LUT index: the plate LUTs it fed
                // are no longer drawn here.
                (void)PulseStep;
                // Press now BRIGHTENS the label to white instead of darkening it. Darkening was only
                // legible against the light press plate; with no plate a dark label on dark art just
                // disappears at the moment you most need confirmation. The "pushed in" read comes
                // from the scale-down already folded into PulseK.
                auto Glow = [&](Color C) -> Color {
                    const float Lift2 = Press > Throb ? Press : Throb;
                    return {C.R + (1.0f - C.R) * Lift2, C.G + (1.0f - C.G) * Lift2,
                            C.B + (1.0f - C.B) * Lift2, 1.0f};
                };
                // The label IS the button now, so it stays big and centred in the (unchanged) hit
                // rect: the visual shrank to a glyph but the target did not.
                // "+1"/"+5", not "x1"/"x5" (visual polish): the button ADDS that many to the queue, it does
                // not multiply anything. "x5" read as a rate or a multiplier on some other quantity.
                char L[8];
                std::snprintf(L, sizeof(L), "+%d", ProdMult[K]);
                TextShadowed(L, bx, by2, bw, bh, LabelPx * PulseK,
                             Afford ? Glow(Ico) : DimC, EHAlign::Center);
            }
        }
    }

    // Health bars on top of the units (sparse: only hurt units). Kept on the per-mesh
    // path — a second instanced draw is a later refinement.
    for (int32_t I = 0; I < Snap.Count; ++I) {
        if (!Snap.IsAlive(I) || Snap.IsBuilding(I)) continue;  // #139: building HP bar is #141
        const int32_t MaxHp = Snap.Units[Snap.Type[I]].MaxHp;
        if (Snap.Hp[I] <= 0 || Snap.Hp[I] >= MaxHp) continue;
        const float Sx = SX(FW(Snap.PrevX[I]) + (FW(Snap.PosX[I]) - FW(Snap.PrevX[I])) * Alpha);
        const float Sy = SY(FW(Snap.PrevY[I]) + (FW(Snap.PosY[I]) - FW(Snap.PrevY[I])) * Alpha);
        const float Frac = static_cast<float>(Snap.Hp[I]) / static_cast<float>(MaxHp);
        const float BarW = UnitPx, BarH = 2.0f * HS;
        const float BarY = Sy - UnitPx * 0.5f - 3.0f * HS;
        HealthBars_.push_back({HealthBg, Sx, BarY, BarW, BarH});   // GUI layer, see the flush
        HealthBars_.push_back({HealthFg, Sx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH});
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
    // EVERY health bar, drawn in the GUI layer so nothing in the world can occlude it.
    // Positions were already computed in screen pixels during the world pass and the GUI camera is
    // the same pixel-space ortho, so they land exactly where they were measured.
    for (const BarQuad& B : HealthBars_) Blit(B.Mat, B.X, B.Y, B.W, B.H);
    if (SelectorDirty) RefreshSelector();

    using Lur::Text::EHAlign;
    using Lur::Text::EVAlign;
    const Color Ico{Srgb(0xC9), Srgb(0xD3), Srgb(0xDA), 1.0f};
    const Color GoldC{Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f};
    const Color BadC{Srgb(0xE1), Srgb(0x4E), Srgb(0x38), 1.0f};
    const Color DimC{Srgb(0x6A), Srgb(0x72), Srgb(0x78), 1.0f};  // disabled/locked, not "unaffordable"
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

    // Build plates (#139): each is a DRAG SOURCE — press-drag its building icon onto the field
    // to place that building for its placement cost (units are then queued at the placed building
    // via the per-building x1/x5/x20 buttons, #140). Anchored above the OS bottom inset.
    // Grouping (#85 playtest): the miner (mining-camp) plate stands apart with a GOLD frame under a
    // gold-token header — "gathers gold" — while rock/paper/scissors share a backing strip under a
    // crossed-swords header — "produce warriors".
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
        // The plate is a DRAG SOURCE to PLACE a building (#139), so its price is the building
        // PLACEMENT cost and affordability reads against that (not the unit cost — units are
        // queued at the placed building via its x1/x5/x20 buttons, #140).
        // §9 opening gate: a LOCKED plate (soldier buildings before the first miner unit) is
        // DISABLED, not merely refused — greyed out here and un-armed in PlateAt, so it can't be
        // dragged at all. It used to look identical to an available one and silently reject every
        // drop, which reads as the game being broken (feedback 2026-07-25).
        const bool Unlocked = Snap.IsBuildingUnlocked(My, static_cast<uint8_t>(Ty));
        PlateLocked[Ty] = !Unlocked;
        const bool Afford = Unlocked && Snap.Gold[My] >= BuildingCostFor(Snap.Cv, Ty);
        // Locked: skip the type frame (gold for the camp / edge for the trio) so the plate reads
        // as inert chrome rather than an armed button.
        if (Unlocked)
            Blit(Ty == 0 ? GoldFlat : PanelEdge, X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f,
                 PlateW + 2.0f, PlateH2 + 2.0f);
        Blit(PlateBg, X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW, PlateH2);
        // Button glyph is the BUILDING icon (miner = camp) in the LOCAL team's per-type tint —
        // the exact icon that follows the finger and lands on the field (#139). While THIS plate
        // is being dragged (or its ghost is sliding home), the icon has "left" the button, so it
        // is hidden here; it reappears the instant a valid drop lands or the slide-back completes.
        const int PlateGlyph = Ty == UnitMiner ? static_cast<int>(GlyphMineCamp) : Ty;
        if (GhostType_ != Ty)
            BlitGlyph(PlateGlyph, Afford ? TypeTintMat[My][Ty] : TypeTintMatDim[My][Ty],
                      X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW * 0.52f);
        // Locked: the price is grey (not the red "you can't afford this" — you CAN afford it, the
        // building just isn't available yet), and the coin dims with it.
        BlitGlyph(GlyphGold, Unlocked ? GoldIconMat : PlateIconDim, X + 12.0f * HS,
                  PlateY + 12.0f * HS, 13.0f * HS);
        std::snprintf(Buf, sizeof(Buf), "%d", BuildingCostFor(Snap.Cv, Ty));
        Text.Draw(Renderer, Buf, X + 20.0f * HS, PlateY + 5.0f * HS, 40.0f * HS, 14.0f * HS,
                  13.0f * HS, !Unlocked ? DimC : (Afford ? Ico : BadC), EHAlign::Left, EVAlign::Top,
                  false);
    }

    // #139 placement ghost: the building icon that "left" its plate follows the pointer — team-
    // tinted when the drop is valid, blinking red when invalid (mirrors Sim::WouldAcceptPlace,
    // evaluated by the caller). A valid release clears it (the real building + the refilled plate
    // icon take over); an invalid release eases it back INTO the plate frame (shrinking to the
    // button-icon size) and vanishes. Drawn here in the HUD layer so it rides over the field and
    // its slide-back target (the plate) is known.
    GhostBlink_ += DtSec;
    if (GhostType_ >= 0) {
        const float DragPx = FW(Snap.Cv.BuildingFootprint) * 2.0f * P;  // footprint-sized while dragging
        const float ButtonPx = PlateW * 0.52f;
        // The ghost tracks the FINGER exactly; only the displacement obstacles push it by is sprung.
        // That distinction is the whole point: springing the position would make the icon lag the
        // thumb, which feels broken, whereas springing the OFFSET (a local-space vector from the
        // finger to the resolved spot) leaves the icon glued to the finger and lets the sidestep
        // around a mine or another building ease in and out instead of snapping.
        //
        // Nothing here reaches the sim: EndPlaceDrag commits the RESOLVED position, so a building
        // lands exactly where it would have without any spring — the solver is invisible to gameplay
        // and to the peer.
        if (GhostDragging_) {
            GhostPushX_.Update(GhostXPx_ - GhostDesiredX_, GhostPushHalflife, DtSec);
            GhostPushY_.Update(GhostYPx_ - GhostDesiredY_, GhostPushHalflife, DtSec);
        }
        float Gx = GhostDesiredX_ + GhostPushX_.X, Gy = GhostDesiredY_ + GhostPushY_.X, GPx = DragPx;
        if (!GhostDragging_ && SlideT_ >= 0.0f) {
            SlideT_ += DtSec;
            constexpr float Dur = 0.18f;
            float K = SlideT_ / Dur; if (K > 1.0f) K = 1.0f;
            K = 1.0f - (1.0f - K) * (1.0f - K);  // ease-out
            const float* Rc = PlateRect[GhostType_];
            Gx = SlideFromX_ + (Rc[0] + Rc[2] * 0.5f - SlideFromX_) * K;
            Gy = SlideFromY_ + (Rc[1] + Rc[3] * 0.5f - SlideFromY_) * K;
            GPx = DragPx + (ButtonPx - DragPx) * K;  // shrink into the button as it arrives
            if (SlideT_ >= Dur) { GhostType_ = -1; SlideT_ = -1.0f; }
        }
        if (GhostType_ >= 0) {
            const Lur::Render::MaterialHandle GM =
                (GhostDragging_ && !GhostValid_) ? GhostBadMat[std::sin(GhostBlink_ * 12.0f) > 0.0f ? 0 : 1]
                                                 : GhostMat[My];
            const int GG = GhostType_ == UnitMiner ? static_cast<int>(GlyphMineCamp) : GhostType_;
            BlitGlyph(GG, GM, Gx, Gy, GPx);
        }
    }

    // #143 placement hand: loop a pointing hand that CARRIES the mining-camp icon from the miner
    // plate toward the nearest gold MINE, demoing the first camp placement — until you place one (no
    // local building yet AND no pending preview). The camp icon + hand fade in together at the plate,
    // travel up, and fade out near the mine; the loop teaches "drag the camp onto gold to mine it",
    // not "drag it to the base". Reuses the alpha-stepped atlas material from the scroll hint.
    {
        bool HasLocalBuilding = false;
        for (int32_t I = 0; I < Snap.Count; ++I)
            // #146: the auto-placed HOME BASE doesn't count — the hand teaches placing the first
            // mining CAMP, which is still the player's first real building.
            if (Snap.IsAlive(I) && Snap.IsBuilding(I) && !Snap.IsHomeBase(I) && Snap.Team[I] == My) {
                HasLocalBuilding = true; break;
            }
        if (!HasLocalBuilding && !PreviewActive_) {
            OnbHandT_ += DtSec;
            const float Period = 1.7f;
            const float t = std::fmod(OnbHandT_, Period) / Period;   // 0..1 loop
            const float* MpR = PlateRect[UnitMiner];
            const float Sx = MpR[0] + MpR[2] * 0.5f, Sy = MpR[1] + MpR[3] * 0.5f;  // miner plate centre
            // end PAST BOTH STARTER MINE ROWS, not at the nearest deposit. Aiming at the nearest
            // mine stopped the hand on the FIRST row — which is not a legal camp spot (it is inside
            // mine_clearance) and taught the player to drop short, exactly where the drop gets refused.
            // The target is instead the spot a camp actually belongs: just beyond the second row, on
            // the X gap between the mine columns at 14 and 20, which is the nearest legal placement
            // that serves both rows. Derived from the live CVars, so it follows the rows when they
            // are re-tuned.
            const float RowSafeW = FW(Snap.Cv.MineRowSafe);
            const float ClearW = FW(Snap.Cv.MineClearance);
            const float WorldH = FW(WorldHeight);
            const float PastRowsY = My == 0 ? RowSafeW + ClearW + 1.0f
                                            : WorldH - RowSafeW - ClearW - 1.0f;
            float Ex = SX(17.0f), Ey = SY(PastRowsY);
            // Keep it on screen: if the computed spot is off the top (a tall map, a scrolled camera),
            // fall back to the old fraction-of-screen target rather than dragging into nowhere.
            if (Ey < 40.0f * HS || Ey > Sy - 40.0f * HS) { Ex = WidthPx * 0.5f; Ey = HeightPx * 0.42f; }
            const float E = t * t * (3.0f - 2.0f * t);                            // smoothstep ease
            const float Hx = Sx + (Ex - Sx) * E, Hy = Sy + (Ey - Sy) * E;
            const float A = t < 0.15f ? t / 0.15f : (t > 0.8f ? (1.0f - t) / 0.2f : 1.0f);  // fade ends
            int Step = static_cast<int>(A * HintAlphaSteps) - 1;
            if (Step < 0) Step = 0;
            if (Step >= HintAlphaSteps) Step = HintAlphaSteps - 1;
            // Carried mining-camp icon (fades with the hand), with the pointer just off its corner.
            BlitGlyph(GlyphMineCamp, HintPointer[Step], Hx, Hy, 40.0f * HS);
            BlitGlyph(GlyphPointer, HintPointer[Step], Hx + 13.0f * HS, Hy + 16.0f * HS, 40.0f * HS);
        } else {
            OnbHandT_ = 0.0f;
        }
    }

    // the SECOND onboarding beat — once the camp is down, teach the +1 button. A pointing hand
    // comes in from the LOWER LEFT and repeatedly approaches and retreats along that diagonal, which
    // reads as tapping without needing a separate tap animation. Runs only while that button is
    // actually pulsing (first camp, nothing queued yet, production not taught), so it stops the moment
    // the player queues anything.
    //
    // Straight BELOW the button, not off to one side: the hand glyph points up, so approaching along
    // the vertical means it already aims at the target and needs no rotation. Below also keeps it off
    // the building's art and off the price/progress rows, which sit inside the icon.
    if (PulseBtnActive_) {
        OnbFingerT_ += DtSec;
        const float Period = 1.1f;
        const float Ph = std::fmod(OnbFingerT_, Period) / Period;
        // cos gives a smooth 0 -> 1 -> 0, so the hand eases IN and back OUT with no snap at the loop
        // boundary (a sawtooth would visibly jump back to "far").
        const float Ease = 0.5f - 0.5f * std::cos(Ph * 6.2831853f);
        const float Far = 40.0f * HS, Near = 10.0f * HS;
        const float Dist = Far - (Far - Near) * Ease;
        const float Bcx = PulseBtnRect_[0] + PulseBtnRect_[2] * 0.5f;
        const float Bcy = PulseBtnRect_[1] + PulseBtnRect_[3] * 0.5f;
        const float Fy = Bcy + PulseBtnRect_[3] * 0.5f + Dist;   // below the button's bottom edge
        BlitGlyph(GlyphPointer, HintPointer[HintAlphaSteps - 1], Bcx, Fy, 44.0f * HS);
    } else {
        OnbFingerT_ = 0.0f;   // restart the approach from far out next time it appears
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
        // #104 z-order: TWO layered passes, armies first and live gold ON TOP. Gold is the
        // decision layer — where the remaining income is tells you where to send miners, and
        // late-game it is what the match turns on — so an army marching over a mine must not
        // erase it. The gold pip stays bigger than a unit dot (2.6 vs 2.0) and fully opaque, so
        // it reads as a crisp pip inside a unit blob rather than hiding under it.
        //
        // Separate draws rather than one append-ordered batch on purpose: the scratch holds
        // MaxUnits instances, so a full field (4096 alive) would otherwise starve whichever
        // layer appends LAST — i.e. exactly the mines this fixes. Two instanced draws of the
        // same material is a rounding error next to that.
        //
        // Units + buildings (#139: camps are placed entities now, no fixed camp dots). A building
        // reads as a bigger dot in the owner's base team colour so the two bases stand out.
        for (int32_t I = 0; I < Snap.Count; ++I) {
            if (!Snap.IsAlive(I)) continue;
            const bool Bldg = Snap.IsBuilding(I);
            Dot(MapX(FW(Snap.PosX[I])), MapFy(FlipW(FW(Snap.PosY[I]))), Bldg ? 3.4f * HS : 2.0f * HS,
                Bldg ? TeamTint[Snap.Team[I]] : TeamTypeTint[Snap.Team[I]][Snap.Type[I]]);
        }
        Renderer->DrawInstances(Quad, Instances, M, 0.0f, WhiteMat);
        // Live gold, on top.
        const Color MiniGold{Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f};
        M = 0;
        for (int T = 0; T < NumMines; ++T)
            if (Snap.MineGold[T] > 0)
                Dot(MapX(FW(Snap.MineX[T])), MapFy(FlipW(FW(Snap.MineY[T]))), 2.6f * HS, MiniGold);
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

    // #2: "opponent link established" — a peer linked while an AI match was running. Blink a green
    // line just under the opponent bar for a few seconds (the player can pick the "Linked opponent"
    // row to switch). Time it out here; NotifyPeerLinked() (main, on the link edge) re-arms it.
    if (PeerLinkBannerT_ > 0.0f) {
        PeerLinkBannerT_ -= DtSec;
        const float Blink = 0.5f + 0.5f * std::sin(PeerLinkBannerT_ * 8.0f);  // ~1.3 Hz throb
        const Color LinkC{Srgb(0x56), Srgb(0xC1), Srgb(0x5F), Blink};
        Text.Draw(Renderer, "opponent link established", Pad, TopInsetPx + 30.0f * HS,
                  WidthPx - 2.0f * Pad, 16.0f * HS, 12.0f * HS, LinkC, EHAlign::Center,
                  EVAlign::Middle, false);
    }

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
        auto Root = Lur::DevGui::BuildCategoryTree(GatherAllCvars(), '.');
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

            // ---- a cvar row: [ i | name ......... | AG | value | R ] ----
            Lur::Core::ICVar* C = It.Cv;
            const bool Overridden = C->Overridden();
            const bool HasTip = C->Tooltip()[0] != '\0';
            const float InfoS = LineH - 6.0f * HS;
            const float InfoX = IndentX + RowPad;
            const float NameX = InfoX + InfoS + 5.0f * HS;
            // "AG" tag immediately left of the value for an AffectsGameplay CVar (#156). Now that the
            // tree lists dev knobs alongside sim tunables, the difference is not cosmetic: an AG edit
            // is latched, hashed and synced to the peer, a dev one is local and persisted only. The
            // name column yields the width, so unmarked rows are unchanged.
            const bool Ag = C->AffectsGameplay();
            const float AgW = Ag ? 18.0f * HS : 0.0f;
            const float NameW = ValX - NameX - 6.0f * HS - AgW;

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
                       TapY >= Sy && TapY <= Sy + LineH) {
                if (C->IsBool()) {
                    // A bool TOGGLES in place (#156) — a numpad for a two-state knob is a keypad to
                    // type "1" into. One tap flips it and commits down the same hook every other
                    // edit uses, so it persists (and, were a bool ever AffectsGameplay, syncs).
                    C->SetFromString(C->RawValue() != 0 ? "false" : "true");
                    if (CvCommitFn_) CvCommitFn_(CvCommitCtx_, *C);
                    // Never leave the numpad bound to a row that no longer uses it.
                    if (C == SelectedCvar_) { Numpad_.Clear(); NumpadOpen_ = false; SelectedCvar_ = nullptr; }
                } else {
                    SelectedCvar_ = C; Numpad_.Clear(); NumpadOpen_ = true;
                }
                TapUsed = true;
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
            if (Ag)
                Text.Draw(Renderer, "AG", ValX - AgW, Sy, AgW - 3.0f * HS, LineH, 9.5f * HS, DimInk,
                          Lur::Text::EHAlign::Right, Lur::Text::EVAlign::Middle);
            Blit(DevKeyMat, ValX + ValW * 0.5f, Sy + LineH * 0.5f, ValW, LineH - 4.0f * HS);
            char VS[64];
            // A bool reads as a CHECKBOX, centred, so it is obvious at a glance that the row is a
            // toggle and not a number to type into. ASCII on purpose — the MSDF atlas is cooked from
            // the glyphs we ship, so a ballot-box codepoint is not guaranteed to be in it.
            if (C->IsBool())
                std::snprintf(VS, sizeof(VS), "%s", C->RawValue() != 0 ? "[x]" : "[ ]");
            else if (NumpadOpen_ && Selected) std::snprintf(VS, sizeof(VS), "%s_", Numpad_.Buffer().c_str());
            else                              std::snprintf(VS, sizeof(VS), "%s", C->ValueString().c_str());
            Text.Draw(Renderer, VS, ValX + 5.0f * HS, Sy, ValW - 10.0f * HS, LineH, 12.5f * HS,
                      Overridden ? Accent : Ink,
                      C->IsBool() ? Lur::Text::EHAlign::Center : Lur::Text::EHAlign::Right,
                      Lur::Text::EVAlign::Middle);
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
