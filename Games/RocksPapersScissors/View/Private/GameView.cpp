#include "Rps/GameView.h"

#include <cstdio>

#include "Lur/Math/Mat4.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Text/BuiltinFonts.h"
#include "Rps/Tunables.h"

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

Lur::Render::MaterialHandle FlatMat(IRenderer* R, Color C) {
    MaterialDesc D;
    D.BaseColor = 0;  // flat white
    D.Tint = C;
    return R->CreateMaterial(D);
}

const char* ResultStr(uint8_t R) {
    switch (R) {
        case ResultTeam0Wins: return "YOU WIN";
        case ResultTeam1Wins: return "YOU LOSE";
        case ResultDraw:      return "DRAW";
        default:              return "";
    }
}

}  // namespace

float GameView::VisibleWorldHeight(float WidthPx, float HeightPx) {
    return HeightPx / Ppu(WidthPx);
}

void GameView::CreateResources(IRenderer* Renderer) {
    const Lur::Render::Quad Q = Lur::Render::MakeQuad();  // white; the material tints it
    Quad = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);

    Background = FlatMat(Renderer, {0.10f, 0.11f, 0.13f, 1.0f});
    CampMat[0] = FlatMat(Renderer, {0.20f, 0.30f, 0.55f, 1.0f});   // you (bottom): blue
    CampMat[1] = FlatMat(Renderer, {0.55f, 0.25f, 0.24f, 1.0f});   // foe (top): red
    TreeMat = FlatMat(Renderer, {0.25f, 0.55f, 0.30f, 1.0f});

    // Per-type shade, team-tinted. Placeholder flats — real cooked R8G8 sprite art
    // (the tint trick) is a later pass.
    const Color T0[4] = {{0.55f, 0.75f, 0.95f, 1.0f}, {0.30f, 0.50f, 0.95f, 1.0f},
                         {0.45f, 0.80f, 0.95f, 1.0f}, {0.40f, 0.90f, 0.80f, 1.0f}};
    const Color T1[4] = {{0.95f, 0.70f, 0.55f, 1.0f}, {0.95f, 0.45f, 0.35f, 1.0f},
                         {0.95f, 0.55f, 0.70f, 1.0f}, {0.95f, 0.80f, 0.40f, 1.0f}};
    for (int Ty = 0; Ty < 4; ++Ty) {
        UnitColor[Ty][0] = T0[Ty];
        UnitColor[Ty][1] = T1[Ty];
    }
    HealthBg = FlatMat(Renderer, {0.05f, 0.05f, 0.05f, 0.9f});
    HealthFg = FlatMat(Renderer, {0.35f, 0.95f, 0.40f, 1.0f});

    Font.Init(Lur::Text::InterFont());
    Font.UploadAtlas(*Renderer);
    Text.CreateResources(Renderer, &Font);
    Ready = true;
}

void GameView::Render(IRenderer* Renderer, const Snapshot& Snap, float Alpha, float CameraY,
                      float WidthPx, float HeightPx, bool FlipY) {
    if (!Ready) return;
    const float P = Ppu(WidthPx);
    const float WHf = FW(WorldHeight);

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

    Blit(Background, WidthPx * 0.5f, HeightPx * 0.5f, WidthPx, HeightPx);

    // Camps (locations, not entities) — faint 3-unit markers at each end.
    const float CampPx = 3.0f * P;
    Blit(CampMat[0], SX(FW(CampX)), SY(FW(Camp0Y)), CampPx, CampPx);
    Blit(CampMat[1], SX(FW(CampX)), SY(FW(Camp1Y)), CampPx, CampPx);

    // Trees.
    const float TreePx = 0.9f * P;
    for (int T = 0; T < NumTrees; ++T)
        Blit(TreeMat, SX(FW(Snap.TreeX[T])), SY(FW(Snap.TreeY[T])), TreePx, TreePx);

    // Units — ONE instanced draw. Each instance carries prev+cur pixel centres; the
    // vertex shader lerps by Alpha, so there is no per-unit CPU interpolation (design §6).
    // We map prev/cur to pixels here (a cheap affine per unit, not a lerp).
    const float UnitPx = 1.1f * P;
    uint32_t N = 0;
    for (int32_t I = 0; I < Snap.Count && N < static_cast<uint32_t>(MaxUnits); ++I) {
        if (!Snap.IsAlive(I)) continue;
        const uint8_t Ty = Snap.Type[I], Tm = Snap.Team[I];
        const Color C = UnitColor[Ty][Tm];
        Lur::Render::InstanceData& D = Instances[N++];
        D.PrevX = SX(FW(Snap.PrevX[I])); D.PrevY = SY(FW(Snap.PrevY[I]));
        D.CurX = SX(FW(Snap.PosX[I]));   D.CurY = SY(FW(Snap.PosY[I]));
        D.R = C.R; D.G = C.G; D.B = C.B; D.A = C.A;
        D.Size = UnitPx;
    }
    Renderer->DrawInstances(Quad, Instances, N, Alpha);

    // Health bars on top of the units (sparse: only hurt units). Kept on the per-mesh
    // path — a second instanced draw is a later refinement.
    for (int32_t I = 0; I < Snap.Count; ++I) {
        if (!Snap.IsAlive(I)) continue;
        const int32_t MaxHp = UnitTable[Snap.Type[I]].MaxHp;
        if (Snap.Hp[I] <= 0 || Snap.Hp[I] >= MaxHp) continue;
        const float Sx = SX(FW(Snap.PrevX[I]) + (FW(Snap.PosX[I]) - FW(Snap.PrevX[I])) * Alpha);
        const float Sy = SY(FW(Snap.PrevY[I]) + (FW(Snap.PosY[I]) - FW(Snap.PrevY[I])) * Alpha);
        const float Frac = static_cast<float>(Snap.Hp[I]) / static_cast<float>(MaxHp);
        const float BarW = UnitPx, BarH = 2.0f;
        const float BarY = Sy - UnitPx * 0.5f - 3.0f;
        Blit(HealthBg, Sx, BarY, BarW, BarH);
        Blit(HealthFg, Sx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH);  // left-aligned
    }

    // HUD (GUI layer, pixel space).
    Renderer->BeginGui();
    char L[4][64];
    std::snprintf(L[0], sizeof(L[0]), "tick %u  %s", Snap.Tick, ResultStr(Snap.Result));
    std::snprintf(L[1], sizeof(L[1]), "YOU  wood %d  units %d  q%d", Snap.Wood[0], Snap.AliveCount[0],
                  Snap.QueueLen[0]);
    std::snprintf(L[2], sizeof(L[2]), "FOE  wood %d  units %d  q%d", Snap.Wood[1], Snap.AliveCount[1],
                  Snap.QueueLen[1]);
    std::snprintf(L[3], sizeof(L[3]), "1-4 you  5-8 foe  drag: pan");
    const float Size = 15.0f, LineH = Size * 1.35f, X = 8.0f;
    const Color Shadow{0.0f, 0.0f, 0.0f, 0.85f};
    const Color Ink{0.90f, 0.95f, 1.0f, 1.0f};
    for (int I = 0; I < 4; ++I) {
        const float Y = 6.0f + LineH * static_cast<float>(I);
        Text.Draw(Renderer, L[I], X + 1.0f, Y + 1.0f, WidthPx, LineH, Size, Shadow,
                  Lur::Text::EHAlign::Left, Lur::Text::EVAlign::Top, false);
        Text.Draw(Renderer, L[I], X, Y, WidthPx, LineH, Size, Ink, Lur::Text::EHAlign::Left,
                  Lur::Text::EVAlign::Top, false);
    }

    Renderer->EndFrame();
}

}  // namespace Rps
