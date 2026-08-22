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
#include "Lur/Hud/GuidLabel.h"    // ShortGuid — the linked row is labelled with the peer id
#include "Lur/Math/Mat4.h"
#include "Lur/Core/DevCommand.h"  // #116: commands rendered as buttons
#include "Lur/Render/ColorString.h"  // CVar<Color> ADL codec  // #117: RGBA picker popover
#include "Lur/Render/Sprite2D.h"
#include "Lur/Render/Mesh2D.h"
#include "Lur/Render/ColorMath.h"
#include "Lur/Render/Rg8Pack.h"
#include "Lur/Text/BuiltinFonts.h"
#include "Lur/Text/TextLayout.h"      // MeasureText — centring the coin+price group in a button
#include "Lur/Trace/Trace.h"          // #103: sub-scope render.view (world-record / gui-record / submit)
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

// Lur::Render::ColorMath::FromHsvDeg() was a second HSV implementation in a third convention; it is Lur::Render::ColorMath now
// (#201), and render_colormath_tests pins it against the formula that used to live here.

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
using Lur::Render::ColorMath::Srgb8;   // #201: was a local constexpr copy

// #117's first real CVar<Color>: the invalid-placement red. #142 reserves red for exactly this
// meaning, which makes it a palette DECISION rather than an arbitrary constant — the kind of
// thing worth being able to try three shades of without a rebuild. Retinted per frame through
// SetMaterialTint (never re-created), so the picker's slider moves it live on screen.
LUR_CVAR(CvGhostInvalidColor, "rps.view.ghost_invalid_color",
         (Lur::Render::Color{Srgb8(0xE1), Srgb8(0x4E), Srgb8(0x38), 1.0f}),
         ::Lur::Core::CVarFlagNone,
         "Colour of the blinking ghost shown when a building cannot be placed there");
// #103 A/B: skip the two FULL-SCREEN backdrop layers, so their cost can be measured rather than
// assumed. The issue blames them for `render.view` sitting at a whole 60 Hz refresh on the A14 —
// but that number is flat regardless of unit count, which is equally the signature of a vsync wait
// parked inside the scope, and that exact signature fooled the iOS investigation three times over
// (acquire -> submit -> begin-frame, #183). `gpu.wait` covers WaitForFrame here and reads ~0.16 ms,
// so the wait is not obviously parked — but "not obviously" is not a measurement.
//
// Turning the layers OFF and re-reading `rv.world` settles it in one run: a large drop means fill is
// genuinely the cost and the merge/bake lever is worth building; an unchanged ~16 ms means the frame
// is paced, not filled, and merging passes would have bought nothing. Dev-only, default ON.
LUR_CVAR(CvDrawField, "rps.view.draw_field", true, ::Lur::Core::CVarFlagNone,
         "DEV A/B (#103): draw the full-screen field gradient");
LUR_CVAR(CvDrawGrid, "rps.view.draw_grid", false, ::Lur::Core::CVarFlagNone,
         "DEV A/B (#103): draw the world grid lines");

using Lur::Render::GradStop;   // #201: promoted to Lur/Render/Mesh2D.h
// Field gradient — SCREENSPACE vertical: night-blue enemy horizon (top) through
// dark earth to the warm umber home ground (bottom). Both players see the same
// grade because both see the enemy at the top (per-player FlipY).
constexpr GradStop FieldStops[] = {
    {0.000f, {Srgb8(0x12), Srgb8(0x22), Srgb8(0x31), 1.0f}},
    {0.179f, {Srgb8(0x11), Srgb8(0x1B), Srgb8(0x15), 1.0f}},
    {0.550f, {Srgb8(0x10), Srgb8(0x17), Srgb8(0x07), 1.0f}},
    {0.795f, {Srgb8(0x16), Srgb8(0x1A), Srgb8(0x09), 1.0f}},
    {1.000f, {Srgb8(0x2E), Srgb8(0x27), Srgb8(0x0F), 1.0f}},
};
constexpr int NumFieldStops = 5;
// Grid colour gradient (screenspace) — lines are world-anchored, colour is not.
constexpr GradStop GridStops[] = {
    {0.0f, {Srgb8(0x26), Srgb8(0x30), Srgb8(0x3B), 1.0f}},
    {1.0f, {Srgb8(0x2E), Srgb8(0x36), Srgb8(0x27), 1.0f}},
};
constexpr float GridStepWu = 4.0f;   // line spacing, world units
constexpr float GridAlpha = 0.55f;   // keep the lines a subtle overlay


// MakeGradientStrip / MakeHGradientStrip / MakeDiscMesh are Lur::Render::Mesh2D now (#201),
// with the geometry split from CreateMesh so it is host-testable — including the triangle-LIST
// winding the disc needs for MoltenVK.

}  // namespace

namespace {
// ---- TOP-HUD BAND METRICS, in HS units, defined ONCE ----
// The opponent bar and the status panel stack with no gap, and the camera's scroll limit
// (TopHudWorldUnits) has to know how tall that stack is. These were three literals in two functions,
// which is how the camera limit ends up describing a layout that no longer exists — moving the panel
// silently left the enemy camp reachable behind the chrome.
constexpr float TopBarTopHs = 2.0f;    // gap between the OS inset and the opponent bar
constexpr float TopBarHs    = 36.0f;   // the opponent bar itself (3/4 of the 48 it first shipped at)
constexpr float TopPanelHs  = 30.0f;   // the gold | population | clock panel, flush under the bar
constexpr float TopHudHs    = TopBarTopHs + TopBarHs + TopPanelHs;   // inset -> panel's bottom edge

// The minimap strip's width. It owns the screen's right edge for the WHOLE height below the top
// panel, so the build plates are laid out in what is left — hence a constant both of them read.
constexpr float MiniStripHs = 12.0f;

// One build plate's width. Derived in one place because two callers need it: Render draws the row,
// and BottomHudWorldUnits turns it into the camera's bottom scroll limit. It was the same four-term
// expression copied into both, so narrowing the row for the minimap would have moved the plates
// without moving the limit that keeps your own camp clear of them.
float PlateWidthPx(float WidthPx, float HS) {
    const float Pad = 8.0f * HS, Gap = 6.0f * HS, GroupGap = 4.0f * Gap;
    const float RowW = WidthPx - MiniStripHs * HS;   // everything left of the minimap strip
    return (RowW - 2.0f * Pad - GroupGap - 2.0f * Gap) / 4.0f;
}
}  // namespace

float GameView::VisibleWorldHeight(float WidthPx, float HeightPx) {
    return HeightPx / Ppu(WidthPx);
}

float GameView::BottomHudWorldUnits(float WidthPx) const {
    const float HS = HudScale(WidthPx);
    const float Pad = 8.0f * HS;
    // nav-bar inset + plate block + group header + a margin so the camp sits WELL
    // above the plates
    return (BottomInsetPx + Pad + PlateWidthPx(WidthPx, HS) * 1.02f + 20.0f * HS + 3.0f * Pad) /
           Ppu(WidthPx);
}

float GameView::TopHudWorldUnits(float WidthPx) const {
    const float HS = HudScale(WidthPx);
    // status-bar inset + the opponent bar + status panel + a margin, mirroring the
    // bottom: the ENEMY camp must clear the top chrome at max scroll-up.
    return (TopInsetPx + (TopHudHs + 24.0f) * HS) / Ppu(WidthPx);
}

void GameView::CreateResources(IRenderer* Renderer) {
    const Lur::Render::Quad Q = Lur::Render::MakeQuad();  // white; the material tints it
    Quad = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    Disc = Lur::Render::MakeDiscMesh(Renderer, 28);   // 28 segments: no straight edge visible at button size

    // Field backdrop + grid (#85): gradient meshes drawn under everything else.
    WhiteMat = FlatMat(Renderer, {1.0f, 1.0f, 1.0f, 1.0f});
    FieldGradMesh = Lur::Render::MakeGradientStripV(Renderer, FieldStops, NumFieldStops, 1.0f);
    VLineMesh = Lur::Render::MakeGradientStripV(Renderer, GridStops, 2, GridAlpha);
    for (int I = 0; I < GridShades; ++I) {
        Color C = Lur::Render::GradSample(GridStops, 2, static_cast<float>(I) / (GridShades - 1));
        C.A *= GridAlpha;
        GridLut[I] = FlatMat(Renderer, C);
    }

    // Upload the cooked glyph atlas (#85): GlyphCount masks side by side, RG8
    // interleaved (R = shade, G = coverage). White sources -> shade 255, so the
    // tint IS the fill and coverage is the cutout.
    {
        constexpr int S = RpsArt::IconSize;
        static uint8_t Rg[GlyphCount * S * S * 2];  // ~256 KB scratch — static, off the stack
        // One tile per glyph, placed at column G*S of the wide atlas. The destination index used to
        // be written out here as 2 * (Y * (GlyphCount * S) + G * S + X); it is Lur::Render::PackRg8's
        // stride parameter now, shared with chess and tested (#201).
        for (int G = 0; G < GlyphCount; ++G)
            Lur::Render::PackRg8(Rg, GlyphCount * S, G * S, 0, RpsArt::IconShade[G],
                                 RpsArt::IconCoverage[G], S, S);
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
    TeamTint[0] = Lur::Render::ColorMath::FromHsvDeg(TeamBaseHue[0], 1.0f, 1.0f);
    TeamTint[1] = Lur::Render::ColorMath::FromHsvDeg(TeamBaseHue[1], 1.0f, 1.0f);
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
    TeamTintBldg[0] = Lur::Render::ColorMath::FromHsvDeg(TeamBaseHue[0], BldgSat, BldgVal);
    TeamTintBldg[1] = Lur::Render::ColorMath::FromHsvDeg(TeamBaseHue[1], BldgSat, BldgVal);
    for (int Tm = 0; Tm < 2; ++Tm)
        for (int Ty = 0; Ty < UnitCount; ++Ty) {
            const float Frac = static_cast<float>(Ty) / static_cast<float>(UnitCount - 1);  // 0 .. 1
            const float H = TeamBaseHue[Tm] + Frac * (TeamEndHue[Tm] - TeamBaseHue[Tm]);
            TeamTypeTint[Tm][Ty] = Lur::Render::ColorMath::FromHsvDeg(H, 1.0f, 1.0f);
            TypeTintMat[Tm][Ty] = AtlasTinted(TeamTypeTint[Tm][Ty]);
            Color Dim = TeamTypeTint[Tm][Ty]; Dim.A = 0.4f;
            TypeTintMatDim[Tm][Ty] = AtlasTinted(Dim);
            TeamTypeTintBldg[Tm][Ty] = Lur::Render::ColorMath::FromHsvDeg(H, BldgSat, BldgVal);
            TypeTintMatBldg[Tm][Ty] = AtlasTinted(TeamTypeTintBldg[Tm][Ty]);
            Color BDim = TeamTypeTintBldg[Tm][Ty]; BDim.A = 0.4f;   // unaffordable drag slot (same hue, faded)
            TypeTintMatBldgDim[Tm][Ty] = AtlasTinted(BDim);
            // The production bar's fill: the SAME HUE as the building and the unit, knocked back one
            // more step — darker and less saturated than the BUILDING (feedback 2026-07-30), the way
            // the building is darker and less saturated than its units. So the ramp reads unit >
            // building > progress bar, brightest to dimmest, and the bar identifies what it is
            // producing without competing with either.
            //
            // It first shipped as the MIDPOINT of building and unit, which put the bar BRIGHTER than
            // the building it sits on — a big block of near-unit colour in the middle of the icon,
            // pulling the eye to the least urgent thing on screen. One knock-back factor, applied to
            // both channels, so the relationship survives a palette retune.
            // Still HSV, not RGB: these endpoints differ only in saturation and value, so scaling
            // THOSE stays on the hue, while scaling RGB drifts off it.
            constexpr float BarKnock = 0.7f;
            ProgressMat[Tm][Ty] =
                FlatMat(Renderer, Lur::Render::ColorMath::FromHsvDeg(H, BldgSat * BarKnock, BldgVal * BarKnock));
        }
    // #143 pulse LUTs: the plate keeps its base colour and only rises in OPACITY (transparent ->
    // opaque); the coin glyph glows from gold toward pure white. The throb walks both.
    for (int I = 0; I < PulseSteps; ++I) {
        const float F = static_cast<float>(I) / (PulseSteps - 1);
        PulsePlate[I] = FlatMat(Renderer, {Srgb8(0x1A), Srgb8(0x20), Srgb8(0x26), 0.40f + 0.58f * F});
        const Color G{Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f};  // gold -> white
        CoinGlow[I] = AtlasTinted({G.R + (1.0f - G.R) * F, G.G + (1.0f - G.G) * F, G.B + (1.0f - G.B) * F, 1.0f});
        // #107 press LUT: a press must be unmistakable, so unlike the pulse (which only breathes in
        // opacity on the SAME dark plate) the pressed plate goes LIGHT — the panel-light grey at a
        // rising alpha. Its own LUT, because materials are immutable and the pulse's dark base could
        // never read as "I got your touch" against a dark button.
        PressPlate[I] = FlatMat(Renderer, {Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), 0.30f + 0.65f * F});
    }
    // #139 placement ghost: a translucent team-tinted silhouette while the drop is valid, and a
    // blinking red one while invalid (two alpha steps the blink alternates — materials are immutable).
    for (int Tm = 0; Tm < 2; ++Tm) {
        Color G = TeamTint[Tm]; G.A = 0.5f;
        GhostMat[Tm] = AtlasTinted(G);
    }
    GhostBadMat[0] = AtlasTinted({Srgb8(0xE1), Srgb8(0x4E), Srgb8(0x38), 0.85f});  // red, bright
    GhostBadMat[1] = AtlasTinted({Srgb8(0xE1), Srgb8(0x4E), Srgb8(0x38), 0.30f});  // red, dim
    ProdBtnBg = FlatMat(Renderer, {Srgb8(0x1A), Srgb8(0x20), Srgb8(0x26), 0.62f});  // #140 translucent button
    for (int Tm = 0; Tm < 2; ++Tm)  // #141 build-frontier line in each team's colour (semi-transparent)
        FrontierMat[Tm] = FlatMat(Renderer, {TeamTint[Tm].R, TeamTint[Tm].G, TeamTint[Tm].B, 0.6f});
    MineMat = AtlasTinted({Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f});  // mine stone = gold tone
    HealthBg = FlatMat(Renderer, {0.05f, 0.05f, 0.05f, 0.9f});
    HealthFg = FlatMat(Renderer, {0.35f, 0.95f, 0.40f, 1.0f});
    GoldBarFg = FlatMat(Renderer, {0.85f, 0.66f, 0.24f, 1.0f});
#if !LUR_SHIPPING
    // The console builds its own materials, gradient meshes and font. This was ~40 lines of dev
    // palette and picker-mesh construction here, re-typed in chess the moment chess wanted a
    // console.
    Console_.CreateResources(Renderer);
#endif

    Font.Init(Lur::Text::InterFont());
    Font.UploadAtlas(*Renderer);
    Text.CreateResources(Renderer, &Font);

    // ---- HUD (#85): locked panel palette + the engine dropdown + DSEG7 clock ----
    PanelMat = FlatMat(Renderer, {Srgb8(0x1A), Srgb8(0x1F), Srgb8(0x24), 0.97f});
    PanelEdge = FlatMat(Renderer, {Srgb8(0x39), Srgb8(0x42), Srgb8(0x4B), 1.0f});
    PlateBg = FlatMat(Renderer, {Srgb8(0x23), Srgb8(0x29), Srgb8(0x30), 0.97f});
    BarBg = FlatMat(Renderer, {0.0f, 0.0f, 0.0f, 0.45f});
    GoldFlat = FlatMat(Renderer, {Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f});
    PlateIconMat = AtlasTinted({Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), 1.0f});
    PlateIconDim = AtlasTinted({Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), 0.4f});
    GoldIconMat = AtlasTinted({Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f});
    MiniWinMat = FlatMat(Renderer, {1.0f, 1.0f, 1.0f, 0.12f});
    MiniWinEdge = FlatMat(Renderer, {Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), 0.6f});
    // First-scroll hint (#85 playtest): alpha-stepped materials (materials are
    // immutable, so the fade walks a LUT) + up/down arrow triangle meshes.
    for (int I = 0; I < HintAlphaSteps; ++I) {
        const float A = static_cast<float>(I + 1) / HintAlphaSteps;
        MaterialDesc DP;
        DP.BaseColor = IconAtlas;
        DP.Tint = {Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), A};
        HintPointer[I] = Renderer->CreateMaterial(DP);
        HintArrow[I] = FlatMat(Renderer, {Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), A});  // white, like the finger
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

void GameView::SetLinked(bool InLinked, const std::string& PeerGuid) {
    if (Linked == InLinked && PeerGuid_ == PeerGuid) return;
    Linked = InLinked;
    PeerGuid_ = PeerGuid;
    SelectorDirty = true;
}

void GameView::SetBuildMismatch(bool Mismatch) {
    if (BuildMismatch_ == Mismatch) return;
    BuildMismatch_ = Mismatch;
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
    // Green / amber / red — a plain traffic light, one colour per rung. The off-the-scale WHITE dot
    // went with the fourth tier when the ladder was re-cut to three (2026-07-30): red is the top of
    // the metaphor again, and it now marks a tier that IS the old "Perhaps Impossible" build.
    const Color Dots[AiTierCount] = {{Srgb8(0x56), Srgb8(0xC1), Srgb8(0x5F), 1.0f},
                                     {Srgb8(0xE0), Srgb8(0xB0), Srgb8(0x40), 1.0f},
                                     {Srgb8(0xD9), Srgb8(0x53), Srgb8(0x4F), 1.0f}};
    // ORDER (feedback 2026-07-25): the LINKED opponent sits at the TOP — a human peer is the main
    // event and the AI tiers are the fallback below it — with an "AI OPPONENTS" header between
    // them. The header is non-selectable and the widget draws a divider line above any header that
    // isn't the first row, so that one row IS the requested separating line.
    Lur::Hud::DropdownItem Items[2 + AiTierCount];   // linked row + header + one per tier
    char Buf[24];
    int N = 0;
    if (Linked) {
        // The peer's DEVICE ID, not a generic word — the same thing chess's opponent list shows,
        // and for the same reason: with two phones on a table you need to tell which is which, and
        // a label that reads identically on both tells you nothing. Falls back to the generic text
        // when no identity is known (desktop loopback, or before the handshake names the peer).
        Items[N].Label = PeerGuid_.empty() ? std::string("Linked opponent")
                                           : Lur::Hud::ShortGuid(PeerGuid_);
        Items[N].Lead = Lur::Hud::ELeadStyle::Dot;
        if (BuildMismatch_) {
            // RED: the link is up but the match will be REFUSED, because the two builds cannot
            // agree on the CVar id list, the tick order or the hash (#112). Saying so here is the
            // whole point — the refusal is otherwise invisible and reads as a freeze (#178).
            //
            // NO RING, deliberately. The ring is chess's "your turn" marker, and RPS is a realtime
            // sim with no turns — reusing it here would import turn-based vocabulary into a game
            // that has none, and teach two different meanings for one mark. The red fill plus the
            // sublabel already separate this from the red AI tier: that row carries a tier name
            // and no explanation, this one carries a device id and a reason.
            Items[N].LeadFill = Color{Srgb8(0xD9), Srgb8(0x53), Srgb8(0x4F), 1.0f};
            Items[N].Sublabel = "different build - rebuild both";
        } else {
            // BLUETOOTH BLUE — a linked human is a different KIND of opponent, not a fourth
            // difficulty, and blue says "radio link, healthy". Green/amber/red stay reserved for
            // the AI tiers (and now for this row's fault state). #0082FC is the Bluetooth SIG blue.
            Items[N].LeadFill = Color{Srgb8(0x00), Srgb8(0x82), Srgb8(0xFC), 1.0f};
        }
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
    GhostSprX_.Snap(0.0f); GhostSprY_.Snap(0.0f);  // zero snap offset; validity starts false
    GhostWasValid_ = false;
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
    GhostDesiredX_ = XPx; GhostDesiredY_ = YPx;   // no separate resolve given: offset is zero
}

void GameView::UpdatePlaceDrag(float DesXPx, float DesYPx, float ResXPx, float ResYPx, bool Valid) {
    if (GhostType_ < 0) return;
    GhostDesiredX_ = DesXPx; GhostDesiredY_ = DesYPx;   // where the finger is (default, un-snapped)
    GhostXPx_ = ResXPx; GhostYPx_ = ResYPx;             // where the sim would accept it (resolved)
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
    // The same, for the disc mesh: a circle of diameter Dpx centred at (Cx, Cy).
    auto BlitDisc = [&](Lur::Render::MaterialHandle Mat, float Cx, float Cy, float Dpx) {
        const Mat4 M = Mat4::Translation({Cx - Dpx * 0.5f, Cy - Dpx * 0.5f, 0.0f}) *
                       Mat4::Scale({Dpx, Dpx, 1.0f});
        Renderer->DrawMesh(Disc, Mat, M);
    };
    // Text with a dark offset copy behind it. Every label that sits on the FIELD (building art, mine
    // art, open ground) needs this: it is what the translucent plates used to do, and the legibility
    // problem outlived the plates. Function-scope rather than local to the building pass because the
    // interactables sub-layer re-draws its labels during the GUI flush (see Interactables_).
    const Color ShadowC{0.0f, 0.0f, 0.0f, 0.75f};
    auto TextShadowed = [&](const char* S, float X, float Y, float W, float H, float Px, Color C,
                            Lur::Text::EHAlign HA) {
        const float O = 1.5f * HS;   // offset: enough to separate, small enough not to smear
        Text.Draw(Renderer, S, X + O, Y + O, W, H, Px, ShadowC, HA, Lur::Text::EVAlign::Middle, false);
        Text.Draw(Renderer, S, X, Y, W, H, Px, C, HA, Lur::Text::EVAlign::Middle, false);
    };

#if LUR_TRACE_ENABLED
    // #103: split render.view (the ~18 ms MoltenVK encoding hog on iOS) at the render-pass
    // boundaries — world draws / GUI draws / EndFrame(submit+present) — to see where the commands
    // pile up. Sampled by timestamp rather than RAII-wrapped: this function is ~1700 lines with no
    // top-level return between BeginFrame and EndFrame, so wrapping it in a block is too invasive.
    const uint64_t RvT0 = Lur::Trace::NowNs();
#endif
    Renderer->BeginFrame(Lur::Render::MakeOrthoCamera(WidthPx, HeightPx));
    HealthBars_.clear();     // refilled each frame by the building/unit passes below
    Interactables_.clear();  // ditto — the press targets, flushed on top of everything world-drawn
    PulseBtnActive_ = false;   // set below if a +1 button is pulsing this frame

    // Field backdrop: the locked SCREENSPACE gradient — spans the viewport, never scrolls.
    if (CvDrawField.Get())
        Renderer->DrawMesh(FieldGradMesh, WhiteMat, Mat4::Scale({WidthPx, HeightPx, 1.0f}));

    // World grid. The LINES are world-anchored (they scroll and X never scrolls, so
    // vertical lines are screen-static); the COLOUR is sampled from the grid gradient
    // in screen space (prototype rule), so the palette holds still under the scroll.
    if (CvDrawGrid.Get()) {
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
        // ONLY WHILE BEING DUG (feedback 2026-07-30): an untouched mine is full by definition, so a
        // full bar over every deposit on the map was decoration — dozens of identical bars saying
        // nothing, and the few that mattered (a reserve running out) were lost among them. Same rule
        // the unit bars already follow.
        if (Snap.MineGold[T] < MineGoldCapacity) {
            const float BarW = MinePx, BarH = 2.0f * HS, BarY = My - MinePx * 0.5f - 3.0f * HS;
            // GUI layer, like the unit/building bars — every bar flushes in BeginGui so the instanced
            // entities (drawn after this pass) can't paint over one.
            HealthBars_.push_back({HealthBg, Mx, BarY, BarW, BarH});
            HealthBars_.push_back({GoldBarFg, Mx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH});
        }
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

    // ---- Rollback correction smoothing (Phase 3) ----
    // Once per PUBLISHED snapshot, absorb any per-slot chain discontinuity into the visual-error
    // offset; every frame, decay that offset toward zero. In normal play snapshot N's Pos equals
    // snapshot N+1's Prev (the sim sets Prev=Pos each step), so the discontinuity is exactly zero and
    // this is a no-op — the offset only becomes non-zero when a rollback rewrote the timeline under a
    // unit. Applied to BOTH interpolation endpoints below, so the whole [Prev,Pos] segment slides.
    const bool NewSnap = Snap.PublishNs != LastSmoothPublishNs;
    if (NewSnap) {
        for (int32_t I = 0; I < Snap.Count; ++I) {
            // Observe() returns "this slot's occupant changed", which is the edge on which EVERY piece
            // of per-slot view state has to be dropped — not just the smoothing offset. LastFace is why
            // a fresh soldier used to point wherever its predecessor last ran; LastCarry is why a new
            // cart could bank gold it never mined (the deposit "+N" fires on the carry >0 -> 0 edge).
            const bool NewOccupant =
                Smoother.Observe(I, Snap.IsAlive(I), Snap.Serial[I], FW(Snap.PrevX[I]),
                                 FW(Snap.PrevY[I]), FW(Snap.PosX[I]), FW(Snap.PosY[I]));
            if (NewOccupant) { LastFaceX[I] = 0.0f; LastFaceY[I] = 0.0f; LastCarry[I] = 0; }
        }
    }
    Smoother.Decay(DtSec, Snap.Count);
    if (NewSnap) LastSmoothPublishNs = Snap.PublishNs;

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
        // Plain interpolation of the sim's Verlet-integrated positions (the shader does
        // mix(Prev,Pos,alpha)) — smooth by construction, NO render-side motion prediction. The only
        // shift is the rollback correction offset, which is exactly 0 in normal play (the snapshot
        // chain Pos_N == Prev_{N+1} holds) and non-zero only for the few render frames after a real
        // rollback, easing that jump instead of popping it.
        const float Ex = Smoother.ErrX(I), Ey = Smoother.ErrY(I);
        D.PrevX = SX(FW(Snap.PrevX[I]) + Ex); D.PrevY = SY(FW(Snap.PrevY[I]) + Ey);
        D.CurX = SX(FW(Snap.PosX[I]) + Ex);   D.CurY = SY(FW(Snap.PosY[I]) + Ey);
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
            O.R = Srgb8(0xD9); O.G = Srgb8(0xA9); O.B = Srgb8(0x3C); O.A = 1.0f;
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
        const Color Ico{Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), 1.0f};
        const Color GoldC{Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f};
        const Color DimC{Srgb8(0x6A), Srgb8(0x72), Srgb8(0x78), 1.0f};
        const float Half = BldgPx * 0.5f;
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
        // The two lines inside a button, sized once. The disc is 0.75 of the narrow side of the hit
        // rect (Bw ~36*HS) so it is ~27*HS across, and it now carries the quantity AND its price —
        // two lines in a circle, which is what sets these sizes. Each line is measured against the
        // CHORD at its own offset from the centre, not the diameter: the chord at +/-0.19 D is still
        // ~0.92 of it, about 25*HS. "+5" at 11*HS is ~12*HS of glyphs; the price line is a coin plus
        // digits, so at 8*HS it is ~8 (coin) + 1.2 (gap) + 12 (three digits) = 21*HS, and a four-digit
        // total still lands inside 25. That last sum is why the price is 8 and not 9: the coin costs
        // the line a digit's worth of width.
        // (31*HS back when the bare label WAS the button and owned the whole rect; 24*HS when the
        // disc arrived with one line.) Legibility survives the drop because a label on a solid disc
        // reads at a smaller size than the same label floating on building art.
        const float LabelPx = 11.0f * HS;
        const float PricePx = 8.0f * HS;
        // The queue/progress row's height. (PriceRowH went with the standalone corner price label —
        // the price lives inside the buttons now, so there is no separate row to keep clear of.)
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
            // ONLY WHEN HURT (feedback 2026-07-30), which is the rule the unit bars have always
            // followed. A full bar over every structure — including both HQs and every camp you own —
            // is a row of identical green lines that carries no information, and it buried the one
            // that does: something of yours is under attack. Damage is now the ONLY thing that puts a
            // bar on screen, so a bar appearing IS the alarm.
            //
            // NOTE this hides the home-base "win meter" until the siege starts, which is the intent:
            // an untouched HQ at 100% is not a meter, it is a constant.
            //
            // collected, not drawn here — flushed in the GUI layer so the instanced units
            // (drawn after this pass) cannot cover a building's own bar.
            if (Snap.Hp[I] > 0 && Snap.Hp[I] < MaxHp) {
                const float HbW = BSize * 0.85f, HbH = 3.0f * HS, HbY = By - BHalf - 5.0f * HS;
                HealthBars_.push_back({HealthBg, Bx, HbY, HbW, HbH});
                HealthBars_.push_back({HealthFg, Bx - HbW * 0.5f + HbW * HFrac * 0.5f, HbY, HbW * HFrac, HbH});
            }

            if (Home) continue;                // #146: the HQ produces nothing — no x1/x5 buttons/queue
            if (Snap.Team[I] != My) continue;  // production controls: your buildings only
            // #143: the FIRST building (the camp) teaches production — its buttons pulse until
            // anything is queued anywhere on it, then it's taught for the rest of the session.
            const bool IsFirstBldg = !FirstLocalSeen;
            FirstLocalSeen = true;
            if (IsFirstBldg && Snap.Queue[I] > 0) ProductionTaught_ = true;
            const bool Pulse = IsFirstBldg && Snap.Queue[I] == 0 && !ProductionTaught_;

            // The next-unit PROGRESS BAR, a row straddling the building's bottom edge, with the
            // "N/max" queue count centred ON it.
            //
            // The bar now FILLS the row (feedback 2026-07-30): it used to be a 4*HS sliver in the
            // right-hand two thirds, with the count squeezed into a 34*HS box beside it — so the
            // element carrying the information you actually watch was the smallest thing in the
            // stack, and the row's own plate read as the background of nothing. Track and fill are
            // the whole row; the count sits on top of them, shadowed so it survives both the dark
            // track and the gold fill sliding under it.
            if (Snap.Queue[I] > 0) {
                // CENTRED ON THE ICON (feedback 2026-08-03): the row sits across the building's own
                // centre rather than straddling its bottom edge. It reads as "this building is working
                // on something" — a meter ON the thing it belongs to — instead of a caption hanging
                // underneath it, and it can never reach down toward the building below. Anchored to By
                // (the icon centre), not an offset, so it holds if the icon size changes.
                const float RowY = By;
                // ONE rect for the whole row: the translucent plate (the count and the bar sit over
                // open field or mine art, and on gold they were unreadable) IS the bar's track, so
                // there is no second background to keep in sync with the first.
                Blit(ProdBtnBg, Bx, RowY, CtrlW, QRowH);   // one height, shared with RowY above
                Blit(BarBg, Bx, RowY, CtrlW, QRowH);       // the track: darkens the UNFILLED part
                const int32_t Bt = Snap.Units[Bty].BuildTicks > 0 ? Snap.Units[Bty].BuildTicks : 1;
                const float PFrac = std::min(1.0f, static_cast<float>(Snap.BuildProgress[I]) / static_cast<float>(Bt));
                if (CtrlW * PFrac > 0.5f)   // sub-pixel fills are not worth a draw call
                    Blit(ProgressMat[My][Bty], Bx - CtrlW * 0.5f + CtrlW * PFrac * 0.5f, RowY,
                         CtrlW * PFrac, QRowH);
                char QB[16];
                std::snprintf(QB, sizeof(QB), "%d/%d", Snap.Queue[I], Snap.BuildingQueueMax);
                TextShadowed(QB, Bx - CtrlW * 0.5f, RowY - QRowH * 0.5f, CtrlW, QRowH, 14.0f * HS,
                             Ico, EHAlign::Center);
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
            // THE PRICE MOVED INSIDE THE BUTTONS (feedback 2026-07-30) and the standalone corner
            // label is gone. It used to state the cost of ONE unit once, in the bottom-right corner
            // the diagonal pair leaves free — but each button now shows the cost of ITS OWN quantity
            // (unit cost x1 under +1, x5 under +5), which makes the old label an exact duplicate of
            // the +1 button's second line. Two numbers saying the same thing, one of them crowded
            // against a disc, is worse than one number in the place you are about to press.
            (void)AffordOne;   // affordability is now per button (Price = UnitCost * ProdMult[K])
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
                const int32_t Price = UnitCost * ProdMult[K];
                const bool Afford = Snap.Gold[My] >= Price;
                // Draw everything about the button's CENTRE, scaled by PulseK (1.0 unless pulsing).
                const float Cx = BX + Bw * 0.5f, Cy = BtnTop + Bh * 0.5f;
                // A CIRCULAR BACKGROUND, in the build plates' own material (feedback 2026-07-30):
                // bare labels on the building art did not read as controls at all. It is the plate
                // material HANDLE, not a copy of its colour — a drag ORIGIN and the button you press
                // afterwards are the same family of thing, and sharing the handle means they cannot
                // drift apart when the palette is retuned.
                //
                // This walks back the earlier "NO plate" decision, whose reason was occlusion — three
                // stacked translucent panels per building hid the art. A disc at THIS size covers a
                // fraction of what those panels did, and the art is knocked back by the building HSV
                // tint anyway (see BldgSat/BldgVal), which was the other half of that fix.
                //
                // 0.75 OF THE INSCRIBED CIRCLE, shrunk toward the icon's OWN CORNER (feedback
                // 2026-07-30): +1 pivots on the bottom-left corner, +5 on the top-right — the corners
                // they already sat nearest. Scaling about the pivot rather than about the button's own
                // centre is what keeps them pinned in their corners as they shrink, instead of both
                // drifting toward the middle of the icon (where they would meet). It also opens the
                // gap between them from "nearly touching" to ~2x their own diameter, which is what
                // makes room for a second line of text inside each one.
                constexpr float BtnShrink = 0.75f;
                const float PivX = (K % 2 == 0) ? (Bx - Half) : (Bx + Half);
                const float PivY = (K % 2 == 0) ? (By + Half) : (By - Half);
                const float Dia = (Bw < Bh ? Bw : Bh) * BtnShrink * PulseK;
                const float DCx = PivX + (Cx - PivX) * BtnShrink;
                const float DCy = PivY + (Cy - PivY) * BtnShrink;
                if (BtnPulse) {   // remember it for the GUI-layer pointing hand
                    PulseBtnActive_ = true;
                    // The DISC's box, not the hit rect: the hand points at what the player can see,
                    // and those are no longer the same rectangle (see the note on the hit rect below).
                    PulseBtnRect_[0] = DCx - Dia * 0.5f; PulseBtnRect_[1] = DCy - Dia * 0.5f;
                    PulseBtnRect_[2] = Dia;              PulseBtnRect_[3] = Dia;
                }
                // NOT DRAWN HERE — queued for the interactables sub-layer, so a neighbouring
                // building's progress bar (drawn later in this same loop) can never cover a button.
                // The press/pulse flash rides along on the BACKGROUND, where it belongs: with a plate
                // to light up, a press reads as the button lighting up rather than as its text
                // brightening. PressPlate is the light LUT #107 added for exactly this.
                // Press now BRIGHTENS the label to white instead of darkening it. Darkening was only
                // legible against the light press plate; with no plate a dark label on dark art just
                // disappears at the moment you most need confirmation. The "pushed in" read comes
                // from the scale-down already folded into PulseK.
                auto Glow = [&](Color C) -> Color {
                    const float Lift2 = Press > Throb ? Press : Throb;
                    return {C.R + (1.0f - C.R) * Lift2, C.G + (1.0f - C.G) * Lift2,
                            C.B + (1.0f - C.B) * Lift2, 1.0f};
                };
                // TWO LINES INSIDE THE DISC: the quantity, and under it what that quantity COSTS.
                // "+1"/"+5", not "x1"/"x5" (visual polish): the button ADDS that many to the queue, it
                // does not multiply anything. "x5" read as a rate or a multiplier on some other quantity.
                //
                // The price is this button's OWN total (UnitCost * ProdMult[K]), which is the number
                // you are actually spending when you press it — a single "cost of one" elsewhere on
                // the icon made the +5 press an arithmetic exercise. Both lines are offset +/-0.19 of
                // the diameter from the centre: far enough apart that the two never touch, close
                // enough that the widest line (a 4-digit +5 total) still clears the circle, since the
                // chord that far off centre is still ~0.92 of the diameter.
                //
                // THE HIT RECT IS DELIBERATELY BIGGER THAN THE DISC and stays the full column-half
                // (set above, unscaled). Shrinking the target with the paint would undo the playtest
                // lesson that made these thumb-sized in the first place; the two rects cannot overlap
                // each other because the columns are disjoint, so the only cost is that a tap on bare
                // art near the corner still counts — which is a feature on a phone.
                InteractBtn Btn;
                Btn.Cx = DCx; Btn.Cy = DCy; Btn.Dia = Dia;
                Btn.Flash = PulseStep > 0 ? PressPlate[PulseStep] : 0;
                Btn.CoinMat = Afford ? GoldIconMat : PlateIconDim;
                Btn.LabelPx = LabelPx * PulseK;
                Btn.PricePx = PricePx * PulseK;
                Btn.LabelCol = Afford ? Glow(Ico) : DimC;
                Btn.PriceCol = Afford ? Glow(GoldC) : DimC;
                std::snprintf(Btn.Label, sizeof(Btn.Label), "+%d", ProdMult[K]);
                std::snprintf(Btn.Price, sizeof(Btn.Price), "%d", Price);
                Interactables_.push_back(Btn);
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
                  {Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), A}, Lur::Text::EHAlign::Center,
                  Lur::Text::EVAlign::Top, false);
    }

    // ---- HUD (GUI layer, pixel space) — the locked layout (#85): opponent
    // dropdown on top, status panel (gold | population | clock) under it, four
    // production plates along the bottom edge. ----
#if LUR_TRACE_ENABLED
    const uint64_t RvT1 = Lur::Trace::NowNs();  // #103: end of the world pass, start of GUI
#endif
    Renderer->BeginGui();
    // ---- THE GUI LAYER'S SUB-LAYER ORDER, and it is a rule rather than an accident ----
    //   1. promoted world overlays  — health/reserve bars: above the field, below anything pressable
    //   2. INTERACTABLES            — every world-anchored press target (the production buttons)
    //   3. screen-space HUD chrome  — status panel + the build plates along the bottom
    //   4. the opponent dropdown    — last, because an OPEN menu outranks everything, itself included
    //
    // (2) is above (1) because a control you can hit must never be hidden by decoration, and below
    // (3) because the chrome is screen-space furniture that world content scrolls UNDER — a building
    // near the bottom edge must not paint its buttons over the plates. Within (2) nothing overlaps:
    // the two buttons per building sit in opposite corners of its footprint.
    //
    // Positions were computed in screen pixels during the world pass and the GUI camera is the same
    // pixel-space ortho, so everything lands exactly where it was measured.
    for (const BarQuad& B : HealthBars_) Blit(B.Mat, B.X, B.Y, B.W, B.H);
    for (const InteractBtn& B : Interactables_) {
        BlitDisc(PlateBg, B.Cx, B.Cy, B.Dia);
        if (B.Flash != 0) BlitDisc(B.Flash, B.Cx, B.Cy, B.Dia);
        const float LineH = B.Dia * 0.38f;
        TextShadowed(B.Label, B.Cx - B.Dia * 0.5f, B.Cy - LineH, B.Dia, LineH, B.LabelPx,
                     B.LabelCol, Lur::Text::EHAlign::Center);
        // The price line is a COIN + DIGITS centred as one group (feedback 2026-07-30): the number is
        // gold, but only the coin says the number is MONEY, and this is the one place in the HUD where
        // a bare figure could be read as a count. Measured rather than guessed — the group's width
        // depends on how many digits the total has, and a hardcoded offset would drift the pair off
        // centre the moment a cost gains one.
        const int PriceLen = static_cast<int>(std::strlen(B.Price));
        float PriceW = 0.0f, PriceH = 0.0f;
        int PriceLines = 0;
        Lur::Text::MeasureText(Font, B.Price, PriceLen, B.Dia, B.PricePx, /*Wrap*/ false, PriceW,
                               PriceH, PriceLines);
        const float CoinPx = B.PricePx;              // coin reads as one more "digit" of the group
        const float CoinGap = B.PricePx * 0.15f;
        const float GroupL = B.Cx - (CoinPx + CoinGap + PriceW) * 0.5f;
        BlitGlyph(GlyphGold, B.CoinMat, GroupL + CoinPx * 0.5f, B.Cy + LineH * 0.5f, CoinPx);
        TextShadowed(B.Price, GroupL + CoinPx + CoinGap, B.Cy, PriceW, LineH, B.PricePx, B.PriceCol,
                     Lur::Text::EHAlign::Left);
    }
    if (SelectorDirty) RefreshSelector();

    using Lur::Text::EHAlign;
    using Lur::Text::EVAlign;
    const Color Ico{Srgb8(0xC9), Srgb8(0xD3), Srgb8(0xDA), 1.0f};
    const Color GoldC{Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f};
    const Color BadC{Srgb8(0xE1), Srgb8(0x4E), Srgb8(0x38), 1.0f};
    const Color DimC{Srgb8(0x6A), Srgb8(0x72), Srgb8(0x78), 1.0f};  // disabled/locked, not "unaffordable"
    const float Pad = 8.0f * HS;
    char Buf[64];

    // ---- The top HUD stack: opponent bar, then the status panel FLUSH under it ----
    // No gap between the two (feedback 2026-07-30): they are one block of chrome, and the sliver of
    // field showing between them read as a seam rather than as breathing room. The bar's own height
    // is the pivot for that — it shrank to 3/4 and the panel followed it up, so the whole stack got
    // shorter rather than the gap moving somewhere else.
    const float SelTop = TopInsetPx + TopBarTopHs * HS;
    const float SelH   = TopBarHs * HS;
    const float PanelY = SelTop + SelH, PanelH = TopPanelHs * HS;
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
    // THE ROW ENDS WHERE THE MINIMAP BEGINS (feedback 2026-07-30). The strip now runs to the bottom
    // of the screen, so the plates yield the width instead of being overdrawn: they are laid out in
    // WidthPx - MiniStripHs and every plate is that much narrower. Shifting them left without
    // narrowing them would have been the same collision one plate further along.
    const float Gap = 6.0f * HS;
    const float GroupGap = 4.0f * Gap;
    const float PlateW = PlateWidthPx(WidthPx, HS);
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
        // Button glyph is the BUILDING icon (miner = camp) in the LOCAL team's per-type BUILDING tint
        // — the exact colour it lands on the field as (feedback 2026-08-03), so the slot previews the
        // placed building rather than the brighter unit shade. While THIS plate is being dragged (or its
        // ghost is sliding home) the icon has "left" the button, so it is hidden here; it reappears the
        // instant a valid drop lands or the slide-back completes.
        const int PlateGlyph = Ty == UnitMiner ? static_cast<int>(GlyphMineCamp) : Ty;
        if (GhostType_ != Ty)
            BlitGlyph(PlateGlyph, Afford ? TypeTintMatBldg[My][Ty] : TypeTintMatBldgDim[My][Ty],
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
        const float DragPx = BldgPx * 1.1f;   // 1.1x the placed size — reads as "dropped into the ground"
        const float ButtonPx = PlateW * 0.52f;
        // The spring acts on the OFFSET the snap imposes — (resolved - desired) — not the world
        // position. The ghost is drawn at desired + spring(offset), so it stays GLUED to the finger
        // (the desired point is applied fresh each frame, un-sprung) and only the sidestep around a
        // mine/building eases. Springing the position instead lagged the thumb even in open ground,
        // where the offset is zero and the ghost should track the finger exactly.
        //
        // Active ONLY while valid, and it HARD-SNAPS across the valid<->invalid edge: a red (invalid)
        // ghost has offset 0 (resolved == desired), so it sits exactly at the finger; the instant a
        // valid spot is (re)acquired the offset snaps to its full value (ghost jumps ONTO the spot) and
        // the spring eases from there as you keep moving in legal territory — not as it pops back in.
        // Twice as fast as the old spring (0.045 vs 0.09): this has to read as reactive.
        //
        // Purely cosmetic: EndPlaceDrag commits GhostXPx_/GhostYPx_ (the resolved spot), so the actual
        // drop is where the building lands regardless of where the spring draws the ghost.
        const float OffX = GhostXPx_ - GhostDesiredX_, OffY = GhostYPx_ - GhostDesiredY_;
        if (GhostDragging_) {
            if (GhostValid_ && GhostWasValid_) {          // staying valid: ease the snap offset
                GhostSprX_.Update(OffX, GhostSpringHalflife, DtSec);
                GhostSprY_.Update(OffY, GhostSpringHalflife, DtSec);
            } else {                                       // (re)acquired validity OR invalid: snap, no ease
                GhostSprX_.Snap(OffX);                     //   valid -> full offset (onto the spot); invalid -> 0
                GhostSprY_.Snap(OffY);
            }
            GhostWasValid_ = GhostValid_;
        }
        float Gx = GhostDesiredX_ + GhostSprX_.X, Gy = GhostDesiredY_ + GhostSprY_.X, GPx = DragPx;
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
            // The two alpha steps the blink alternates between are baked in; only the HUE is
            // tunable, so a colour that reads badly can be fixed without touching the blink.
            const Lur::Render::Color Bad = CvGhostInvalidColor.Get();
            Renderer->SetMaterialTint(GhostBadMat[0], {Bad.R, Bad.G, Bad.B, 0.85f});
            Renderer->SetMaterialTint(GhostBadMat[1], {Bad.R, Bad.G, Bad.B, 0.30f});
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
        // FULL HEIGHT (feedback 2026-07-30): from the status panel's bottom edge to the last pixel of
        // the screen. It used to stop short of the build plates, which cost it a third of its length
        // and — because the map is drawn to scale along it — a third of the resolution with which it
        // could show where anything was. The plates were narrowed to clear it (PlateWidthPx), so the
        // two no longer share any pixels and the draw order between them stops mattering.
        const float StripW = MiniStripHs * HS;
        const float StripX = WidthPx - StripW;
        const float StripY = PanelY + PanelH;
        const float StripB = HeightPx;
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
        const Color MiniGold{Srgb8(0xD9), Srgb8(0xA9), Srgb8(0x3C), 1.0f};
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
    //
    // NO CAPTION (feedback 2026-07-30). The word "Opponent" above a row that already names the
    // opponent was a label for something self-evident, and it cost a third of the band to say it.
    //
    // EDGE TO EDGE, and 3/4 of the height it first shipped at — the two go together: a bar pinned to
    // both screen edges reads as a title bar rather than as a widget, and a title bar does not need
    // 48*HS of height to be found. Height shrank from the BOTTOM (the top gap is fixed at
    // TopBarTopHs), which is what let the status panel move up flush against it.
    //
    // Full width means no side padding at all: X = 0, W = WidthPx. Every other HUD element insets by
    // Pad; this one deliberately does not.
    //
    // The OPEN LIST keeps compact rows: the pill is a header, the list is a list. Scaling five rows
    // to a header-sized pill would push the bottom of the menu into the build plates.
    Selector.Draw(Renderer, nullptr, 0.0f, SelTop, WidthPx, SelH, /*MenuRowH*/ 26.0f * HS);

    // #178: a build-fingerprint MISMATCH refuses the match before tick 0, and that refusal was
    // invisible in the game view — the selector row goes red (RefreshSelector) but the dropdown is
    // shut while you wait, so all the player sees is a placed camp and a screen that never starts.
    // "A correct refusal reads as a freeze" is the whole bug. Say it on the field, unmissably and
    // PERSISTENTLY (no timer — the condition only clears on a rebuild, unlike the transient link/
    // resync banners): a centered headline so it cannot be mistaken for a hang, plus the one action
    // that fixes it. Driven by BuildMismatch_ per frame, so it vanishes the instant a matching peer
    // reconnects. Drawn BEFORE and mutually exclusive with the transient banners below — a refused
    // match is not being linked or repaired, and this message outranks either.
    //
    // Gated on SelPeer_ (the linked peer is our CURRENT opponent), not on BuildMismatch_ alone: a
    // mismatched peer can link while you are mid-AI-match (that is exactly what the #2 link blink is
    // for), and shouting "DIFFERENT BUILD" over a game the mismatch is not blocking would be wrong —
    // the selector's red row already carries the standing warning for that case. The mid-screen
    // banner is for when the refusal is actually stopping the match YOU are trying to play.
    const bool ShowBuildMismatch = BuildMismatch_ && SelPeer_;
    if (ShowBuildMismatch) {
        const float Throb = 0.6f + 0.4f * std::sin(PulseT_ * 5.0f);   // reuse the #143 animation clock
        const Color BadC{Srgb8(0xD9), Srgb8(0x53), Srgb8(0x4F), 1.0f};   // the selector's mismatch red
        const Color BadThrob{Srgb8(0xD9), Srgb8(0x53), Srgb8(0x4F), Throb};
        Text.Draw(Renderer, "DIFFERENT BUILD", 0.0f, HeightPx * 0.40f, WidthPx, 34.0f * HS,
                  26.0f * HS, BadThrob, EHAlign::Center, EVAlign::Middle, false);
        Text.Draw(Renderer, "opponent is on a different build - the match cannot start",
                  Pad, HeightPx * 0.40f + 34.0f * HS, WidthPx - 2.0f * Pad, 16.0f * HS, 13.0f * HS,
                  BadC, EHAlign::Center, EVAlign::Middle, false);
        Text.Draw(Renderer, "rebuild BOTH devices from the same commit",
                  Pad, HeightPx * 0.40f + 54.0f * HS, WidthPx - 2.0f * Pad, 16.0f * HS, 13.0f * HS,
                  BadC, EHAlign::Center, EVAlign::Middle, false);
    }

    // #163: the link is HALF-OPEN — we are connected, but the opponent's phone has gone silent (its
    // BLE notify path is wedged), so the match cannot start or advance. Until now that read to the
    // player as a pure freeze — the whole #163 complaint ("placed a camp on both phones and nothing
    // happened"). The Session detects it (Session::IsLinkHalfOpen) and the main feeds it here. Amber,
    // NOT the build-mismatch red: this can clear itself (a reconnect that brings traffic retires it),
    // and when it doesn't the fix is a human one that the sub-line names. Gated on SelPeer_ like the
    // build banner so it only shows for the linked match you actually picked; persistent (state-
    // driven, clears the instant traffic resumes), and it outranks the transient link/resync lines.
    const bool ShowHalfOpen = LinkHalfOpen_ && SelPeer_ && !ShowBuildMismatch;
    if (ShowHalfOpen) {
        const float Throb = 0.55f + 0.45f * std::sin(PulseT_ * 6.0f);
        const Color AmbC{Srgb8(0xE8), Srgb8(0xA5), Srgb8(0x3A), 1.0f};
        const Color AmbThrob{Srgb8(0xE8), Srgb8(0xA5), Srgb8(0x3A), Throb};
        Text.Draw(Renderer, "LINK STALLED", 0.0f, HeightPx * 0.40f, WidthPx, 30.0f * HS,
                  24.0f * HS, AmbThrob, EHAlign::Center, EVAlign::Middle, false);
        Text.Draw(Renderer, "the opponent's phone went quiet - reconnecting",
                  Pad, HeightPx * 0.40f + 32.0f * HS, WidthPx - 2.0f * Pad, 16.0f * HS, 13.0f * HS,
                  AmbC, EHAlign::Center, EVAlign::Middle, false);
        Text.Draw(Renderer, "if it persists, toggle Bluetooth on the other phone",
                  Pad, HeightPx * 0.40f + 52.0f * HS, WidthPx - 2.0f * Pad, 16.0f * HS, 13.0f * HS,
                  AmbC, EHAlign::Center, EVAlign::Middle, false);
    }

    // #2: "opponent link established" — a peer linked while an AI match was running. Blink a green
    // line for a few seconds (the player can pick the "Linked opponent" row to switch). Time it out
    // here; NotifyPeerLinked() (main, on the link edge) re-arms it.
    // BELOW the status panel now: the bar it used to sit inside grew to fill its whole band, so at
    // the old Y it drew on top of the opponent's name.
    if (!ShowBuildMismatch && !ShowHalfOpen && PeerLinkBannerT_ > 0.0f) {
        PeerLinkBannerT_ -= DtSec;
        const float Blink = 0.5f + 0.5f * std::sin(PeerLinkBannerT_ * 8.0f);  // ~1.3 Hz throb
        const Color LinkC{Srgb8(0x56), Srgb8(0xC1), Srgb8(0x5F), Blink};
        Text.Draw(Renderer, "opponent link established", Pad, PanelY + PanelH + 6.0f * HS,
                  WidthPx - 2.0f * Pad, 16.0f * HS, 12.0f * HS, LinkC, EHAlign::Center,
                  EVAlign::Middle, false);
    }

    // #161: a desync repair is in flight. The match holds for a moment and can rewind a second or two
    // of play when it resumes — silently, that reads as a glitch or as the opponent cheating, which is
    // a worse experience than the freeze it replaced. Amber, and in the same slot as the link banner
    // (the two cannot coexist: one is a fresh link, the other a live match being repaired). Driven by
    // the actual state per frame, so it disappears the instant the repair lands rather than on a timer.
    if (!ShowBuildMismatch && !ShowHalfOpen && Recovering_) {
        const float Throb = 0.55f + 0.45f * std::sin(PulseT_ * 7.0f);  // reuse the #143 animation clock
        const Color WarnC{Srgb8(0xE8), Srgb8(0xA5), Srgb8(0x3A), Throb};
        Text.Draw(Renderer, "resyncing with opponent...", Pad, PanelY + PanelH + 6.0f * HS,
                  WidthPx - 2.0f * Pad, 16.0f * HS, 12.0f * HS, WarnC, EHAlign::Center,
                  EVAlign::Middle, false);
    }

#if !LUR_SHIPPING
    // ---- The dev console ----
    // Composited by the BeginDevGui THIRD pass, over the game and its GUI, which is why it is
    // drawn LAST and inside the same BeginFrame..EndFrame. Closed, it paints nothing and enters
    // no pass. Lur::DevGui::Console owns everything else.
    Console_.Draw(Renderer, WidthPx, HeightPx);
#endif

#if LUR_TRACE_ENABLED
    const uint64_t RvT2 = Lur::Trace::NowNs();  // #103: end of GUI+dev draws, start of submit+present
#endif
    Renderer->EndFrame();
#if LUR_TRACE_ENABLED
    const uint64_t RvT3 = Lur::Trace::NowNs();
    static const Lur::Trace::ScopeId RvWorld  = Lur::Trace::Register("rv.world");   // BeginFrame + field/grid/units/text
    static const Lur::Trace::ScopeId RvGui    = Lur::Trace::Register("rv.gui");     // BeginGui + HUD + dev overlay
    static const Lur::Trace::ScopeId RvSubmit = Lur::Trace::Register("rv.submit");  // EndFrame: vkQueueSubmit + present
    Lur::Trace::AddSample(RvWorld,  RvT1 - RvT0);
    Lur::Trace::AddSample(RvGui,    RvT2 - RvT1);
    Lur::Trace::AddSample(RvSubmit, RvT3 - RvT2);
#endif
}

}  // namespace Rps
