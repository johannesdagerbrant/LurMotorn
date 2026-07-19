#include "Rps/GameView.h"

#include <cstdio>

#include "Lur/Math/Mat4.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Text/BuiltinFonts.h"
#include "Rps/Tunables.h"

// The design-lock glyph set (#85, Docs/Planning/rps-hud-prototype.html): indices
// 0..3 are EUnit order (miner, rock, paper, scissors), then gold / mine / swords /
// camp. Sources: game-icons.net (CC BY 3.0) + Font Awesome Free (CC BY 4.0) + the
// custom bold pick (ours) — attribution required in-app before shipping (#85).
// LUR_COOK rg8-shade-coverage src=Icons/miner.png,Icons/rock.png,Icons/paper.png,Icons/scissors.png,Icons/gold.png,Icons/mine.png,Icons/swords.png,Icons/camp.png out=View/Private/IconMasks.h ns=RpsArt size=IconSize coverage=IconCoverage shade=IconShade
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

// ---- Locked palette (#85, Docs/Planning/rps-hud-prototype.html) ----
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
    const float PlateW = (WidthPx - 2.0f * Pad - 3.0f * Gap) / 4.0f;
    // nav-bar inset + plate block + a margin so the camp sits WELL above the plates
    return (BottomInsetPx + Pad + PlateW * 1.02f + 3.0f * Pad) / Ppu(WidthPx);
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
    MineMat = AtlasTinted({Srgb(0xD9), Srgb(0xA9), Srgb(0x3C), 1.0f});  // mine stone = gold tone
    HealthBg = FlatMat(Renderer, {0.05f, 0.05f, 0.05f, 0.9f});
    HealthFg = FlatMat(Renderer, {0.35f, 0.95f, 0.40f, 1.0f});
    GoldBarFg = FlatMat(Renderer, {0.85f, 0.66f, 0.24f, 1.0f});

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
                      float WidthPx, float HeightPx, bool FlipY) {
    if (!Ready) return;
    const float P = Ppu(WidthPx);
    const float WHf = FW(WorldHeight);
    // Everything user-facing is VIEWER-RELATIVE: "you" is whichever team this device
    // plays (FlipY is set exactly for the top/team-1 player). Blue = you, red = foe,
    // and the HUD reads your team's gold/queues — on both phones.
    const int My = FlipY ? 1 : 0;
    const int Foe = 1 - My;
    const float HS = HudScale(WidthPx);  // HUD metrics scale with resolution (#85 feedback)

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
    // geometry) — the barracks tent in the owner's viewer-relative colour.
    const float CampPx = 4.5f * P;
    BlitGlyph(GlyphCamp, CampMat[0 == My ? 0 : 1], SX(FW(CampX)), SY(FW(Camp0Y)), CampPx);
    BlitGlyph(GlyphCamp, CampMat[1 == My ? 0 : 1], SX(FW(CampX)), SY(FW(Camp1Y)), CampPx);

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
        const Color C = TeamTint[Tm == My ? 0 : 1];
        Lur::Render::InstanceData& D = Instances[N++];
        D.PrevX = SX(FW(Snap.PrevX[I])); D.PrevY = SY(FW(Snap.PrevY[I]));
        D.CurX = SX(FW(Snap.PosX[I]));   D.CurY = SY(FW(Snap.PosY[I]));
        D.R = C.R; D.G = C.G; D.B = C.B; D.A = C.A;
        D.Size = UnitPx;
        D.U0 = static_cast<float>(Ty) / static_cast<float>(GlyphCount); D.V0 = 0.0f;
        D.U1 = static_cast<float>(Ty + 1) / static_cast<float>(GlyphCount); D.V1 = 1.0f;
    }
    Renderer->DrawInstances(Quad, Instances, N, Alpha, AtlasMat);

    // Health bars on top of the units (sparse: only hurt units). Kept on the per-mesh
    // path — a second instanced draw is a later refinement.
    for (int32_t I = 0; I < Snap.Count; ++I) {
        if (!Snap.IsAlive(I)) continue;
        const int32_t MaxHp = UnitTable[Snap.Type[I]].MaxHp;
        if (Snap.Hp[I] <= 0 || Snap.Hp[I] >= MaxHp) continue;
        const float Sx = SX(FW(Snap.PrevX[I]) + (FW(Snap.PosX[I]) - FW(Snap.PrevX[I])) * Alpha);
        const float Sy = SY(FW(Snap.PrevY[I]) + (FW(Snap.PosY[I]) - FW(Snap.PrevY[I])) * Alpha);
        const float Frac = static_cast<float>(Snap.Hp[I]) / static_cast<float>(MaxHp);
        const float BarW = UnitPx, BarH = 2.0f * HS;
        const float BarY = Sy - UnitPx * 0.5f - 3.0f * HS;
        Blit(HealthBg, Sx, BarY, BarW, BarH);
        Blit(HealthFg, Sx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH);  // left-aligned
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
    std::snprintf(Buf, sizeof(Buf), "%d", Snap.Gold[My]);
    Text.Draw(Renderer, Buf, Pad + 22.0f * HS, PanelY, 120.0f * HS, PanelH, 15.0f * HS, GoldC,
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
    const float Gap = 6.0f * HS;
    const float PlateW = (WidthPx - 2.0f * Pad - 3.0f * Gap) / 4.0f;
    const float PlateH2 = PlateW * 1.02f;
    const float PlateY = HeightPx - BottomInsetPx - Pad - PlateH2;
    for (int Ty = 0; Ty < 4; ++Ty) {
        const float X = Pad + static_cast<float>(Ty) * (PlateW + Gap);
        PlateRect[Ty][0] = X; PlateRect[Ty][1] = PlateY;
        PlateRect[Ty][2] = PlateW; PlateRect[Ty][3] = PlateH2;
        const bool Afford = Snap.Gold[My] >= UnitTable[Ty].Cost;
        Blit(PanelEdge, X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW + 2.0f, PlateH2 + 2.0f);
        Blit(PlateBg, X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW, PlateH2);
        BlitGlyph(Ty, Afford ? PlateIconMat : PlateIconDim,
                  X + PlateW * 0.5f, PlateY + PlateH2 * 0.5f, PlateW * 0.52f);
        BlitGlyph(GlyphGold, GoldIconMat, X + 12.0f * HS, PlateY + 12.0f * HS, 13.0f * HS);
        std::snprintf(Buf, sizeof(Buf), "%d", UnitTable[Ty].Cost);
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
                         static_cast<float>(UnitTable[Ty].BuildTicks);
            if (Frac > 1.0f) Frac = 1.0f;
            const float Bw = BarW * Frac;
            if (Bw > 0.5f)
                Blit(GoldFlat, X + 6.0f * HS + Bw * 0.5f, PlateY + PlateH2 - 7.0f * HS, Bw,
                     5.0f * HS);
        }
    }

    // Dev diagnostics line + match-result banner.
    std::snprintf(Buf, sizeof(Buf), "tick %u   FOE gold %d units %d", Snap.Tick,
                  Snap.Gold[Foe], Snap.AliveCount[Foe]);
    Text.Draw(Renderer, Buf, Pad, PanelY + PanelH + 4.0f * HS, WidthPx, 16.0f * HS, 11.0f * HS,
              {0.55f, 0.62f, 0.68f, 0.9f}, EHAlign::Left, EVAlign::Top, false);
    if (Snap.Result != ResultOngoing) {
        Text.Draw(Renderer, ResultStr(Snap.Result, My), 0.0f, HeightPx * 0.42f, WidthPx,
                  40.0f * HS, 30.0f * HS, GoldC, EHAlign::Center, EVAlign::Middle, false);
    }

    // The opponent dropdown draws LAST so its open list overlays the panel.
    Selector.Draw(Renderer, "Opponent", Pad, TopInsetPx + 4.0f * HS, WidthPx - 2.0f * Pad,
                  24.0f * HS);

    Renderer->EndFrame();
}

}  // namespace Rps
