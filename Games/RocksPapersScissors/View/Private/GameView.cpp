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

    CampMat[0] = FlatMat(Renderer, {0.25f, 0.66f, 0.86f, 1.0f});   // you (bottom): team blue #3FA8DC
    CampMat[1] = FlatMat(Renderer, {0.88f, 0.29f, 0.19f, 1.0f});   // foe (top): team red #E04A31
    MineMat = FlatMat(Renderer, {0.85f, 0.66f, 0.24f, 1.0f});   // gold-bearing rock (locked palette #D9A93C)

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
    GoldBarFg = FlatMat(Renderer, {0.85f, 0.66f, 0.24f, 1.0f});

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

    // Camps (locations, not entities) — faint 3-unit markers at each end.
    const float CampPx = 3.0f * P;
    Blit(CampMat[0], SX(FW(CampX)), SY(FW(Camp0Y)), CampPx, CampPx);
    Blit(CampMat[1], SX(FW(CampX)), SY(FW(Camp1Y)), CampPx, CampPx);

    // Mines — finite (#84): a depleted mine is gone; live ones carry a gold reserve
    // bar above them (same visual language as unit health, gold fill).
    const float MinePx = 1.4f * P;
    for (int T = 0; T < NumMines; ++T) {
        if (Snap.MineGold[T] <= 0) continue;
        const float Mx = SX(FW(Snap.MineX[T])), My = SY(FW(Snap.MineY[T]));
        Blit(MineMat, Mx, My, MinePx, MinePx);
        const float Frac = static_cast<float>(Snap.MineGold[T]) / static_cast<float>(MineGoldCapacity);
        const float BarW = MinePx, BarH = 2.0f, BarY = My - MinePx * 0.5f - 3.0f;
        Blit(HealthBg, Mx, BarY, BarW, BarH);
        Blit(GoldBarFg, Mx - BarW * 0.5f + BarW * Frac * 0.5f, BarY, BarW * Frac, BarH);
    }

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
    const int32_t Q0 = Snap.QueueCount[0][0] + Snap.QueueCount[0][1] + Snap.QueueCount[0][2] + Snap.QueueCount[0][3];
    const int32_t Q1 = Snap.QueueCount[1][0] + Snap.QueueCount[1][1] + Snap.QueueCount[1][2] + Snap.QueueCount[1][3];
    std::snprintf(L[1], sizeof(L[1]), "YOU  gold %d  units %d  q%d", Snap.Gold[0], Snap.AliveCount[0], Q0);
    std::snprintf(L[2], sizeof(L[2]), "FOE  gold %d  units %d  q%d", Snap.Gold[1], Snap.AliveCount[1], Q1);
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
