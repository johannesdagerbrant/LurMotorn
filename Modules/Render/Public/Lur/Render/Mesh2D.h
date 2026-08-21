#pragma once
// Unit-rect 2D mesh builders: colour ramps, gradient strips, and a filled disc.
//
// Promoted out of Rps::GameView (#201). Zero game content — they build geometry in the unit rect
// (0,0)-(1,1) and the caller scales it, exactly like Sprite2D's MakeQuad, which is the sibling these
// belong next to.
//
// ---- TRIANGLE LISTS, NEVER FANS ----
// The disc is emitted as a triangle LIST even though a fan is the obvious encoding, because fans are
// outside the Vulkan portability subset MoltenVK runs on iOS (CLAUDE.md). Having that constraint
// enforced in one engine function rather than remembered at each call site is a large part of why this
// is worth promoting: it is the kind of rule a new game breaks once and only discovers on a device.
//
// ---- Geometry is separated from mesh creation ----
// Each builder comes in two parts: a pure Build* that fills caller-owned vertex/index arrays, and a
// thin Make* that uploads through IRenderer. That split exists so the geometry is host-testable with
// no GPU — the winding, the index bounds and the stop interpolation are all checkable, and they are
// precisely the things whose breakage looks like an art bug rather than a code bug.
#include <cmath>
#include <cstdint>

#include "Lur/Math/Vec.h"
#include "Lur/Render/Renderer.h"

namespace Lur::Render {

// ---- Colour ramps -----------------------------------------------------------------------------

// A stop at position P in [0,1] with colour C. Stops must be in ascending P.
struct GradStop {
    float P;
    Color C;
};

// Largest ramp the fixed-size builders below accept. Bigger inputs are CLAMPED, not overflowed —
// the original wrote into `Vertex V[2 * 8]` with no bounds check at all, so an eight-stop limit was
// enforced only by nobody having tried a ninth.
inline constexpr int MaxGradStops = 8;

// Sample the ramp at T. Clamps outside the first/last stop rather than extrapolating, and treats a
// zero-width span as "take the later stop" so duplicate positions cannot divide by zero.
inline Color GradSample(const GradStop* S, int N, float T) {
    if (N <= 0) return Color{};
    if (T <= S[0].P) return S[0].C;
    for (int I = 1; I < N; ++I) {
        if (T <= S[I].P) {
            const float Span = S[I].P - S[I - 1].P;
            const float K = Span > 0.0f ? (T - S[I - 1].P) / Span : 1.0f;
            const Color& A = S[I - 1].C;
            const Color& B = S[I].C;
            return {A.R + (B.R - A.R) * K, A.G + (B.G - A.G) * K, A.B + (B.B - A.B) * K,
                    A.A + (B.A - A.A) * K};
        }
    }
    return S[N - 1].C;
}

// ---- Gradient strips --------------------------------------------------------------------------
// One vertex row per stop with the stop's colour baked per vertex; the GPU interpolates between rows.
// Vertical (stop drives Y) and horizontal (stop drives X) are kept as SEPARATE functions rather than
// parameterised on an axis — that was the original author's call, on the grounds that two short
// functions read better than one with a branch in the vertex loop, and a relocation is not the place
// to overturn it.
//
// Fills OutV[0 .. 2N) and OutI[0 .. 6(N-1)). Caller-owned arrays must hold MaxGradStops worth.
// Returns the number of stops actually used (clamped to MaxGradStops).
inline int BuildGradientStripV(const GradStop* Stops, int N, float Alpha, Vertex* OutV,
                               uint32_t* OutI, uint32_t& VCount, uint32_t& ICount) {
    if (N > MaxGradStops) N = MaxGradStops;
    if (N < 2) { VCount = 0; ICount = 0; return 0; }
    const Lur::Math::Vec3 Nrm{0.0f, 0.0f, 1.0f};
    for (int I = 0; I < N; ++I) {
        const Color& C = Stops[I].C;
        const Lur::Math::Vec4 VC{C.R, C.G, C.B, C.A * Alpha};
        OutV[2 * I + 0] = {{0.0f, Stops[I].P, 0.0f}, Nrm, {0.0f, Stops[I].P}, VC};
        OutV[2 * I + 1] = {{1.0f, Stops[I].P, 0.0f}, Nrm, {1.0f, Stops[I].P}, VC};
    }
    uint32_t K = 0;
    for (int I = 0; I < N - 1; ++I) {
        const uint32_t A = 2 * static_cast<uint32_t>(I);
        OutI[K++] = A; OutI[K++] = A + 1; OutI[K++] = A + 3;
        OutI[K++] = A; OutI[K++] = A + 3; OutI[K++] = A + 2;
    }
    VCount = static_cast<uint32_t>(2 * N);
    ICount = K;
    return N;
}

inline int BuildGradientStripH(const GradStop* Stops, int N, float Alpha, Vertex* OutV,
                               uint32_t* OutI, uint32_t& VCount, uint32_t& ICount) {
    if (N > MaxGradStops) N = MaxGradStops;
    if (N < 2) { VCount = 0; ICount = 0; return 0; }
    const Lur::Math::Vec3 Nrm{0.0f, 0.0f, 1.0f};
    for (int I = 0; I < N; ++I) {
        const Color& C = Stops[I].C;
        const Lur::Math::Vec4 VC{C.R, C.G, C.B, C.A * Alpha};
        OutV[2 * I + 0] = {{Stops[I].P, 0.0f, 0.0f}, Nrm, {Stops[I].P, 0.0f}, VC};
        OutV[2 * I + 1] = {{Stops[I].P, 1.0f, 0.0f}, Nrm, {Stops[I].P, 1.0f}, VC};
    }
    uint32_t K = 0;
    for (int I = 0; I < N - 1; ++I) {
        const uint32_t A = 2 * static_cast<uint32_t>(I);
        OutI[K++] = A; OutI[K++] = A + 1; OutI[K++] = A + 3;
        OutI[K++] = A; OutI[K++] = A + 3; OutI[K++] = A + 2;
    }
    VCount = static_cast<uint32_t>(2 * N);
    ICount = K;
    return N;
}

// ---- Filled disc ------------------------------------------------------------------------------
// Inscribed in the unit rect (centre 0.5,0.5, radius 0.5), WHITE like MakeQuad so a material tint
// colours it — which is what lets a caller share an existing material handle instead of creating a
// look-alike copy of its colour.
inline constexpr int MaxDiscSegments = 64;

inline void BuildDisc(int Segments, Vertex* OutV, uint32_t* OutI, uint32_t& VCount, uint32_t& ICount) {
    if (Segments > MaxDiscSegments) Segments = MaxDiscSegments;
    if (Segments < 3) Segments = 3;
    const Lur::Math::Vec3 Nrm{0.0f, 0.0f, 1.0f};
    const Lur::Math::Vec4 White{1.0f, 1.0f, 1.0f, 1.0f};
    OutV[0] = {{0.5f, 0.5f, 0.0f}, Nrm, {0.5f, 0.5f}, White};   // centre
    for (int I = 0; I <= Segments; ++I) {
        const float A = 6.2831853f * static_cast<float>(I) / static_cast<float>(Segments);
        const float X = 0.5f + 0.5f * std::cos(A), Y = 0.5f + 0.5f * std::sin(A);
        OutV[1 + I] = {{X, Y, 0.0f}, Nrm, {X, Y}, White};
    }
    uint32_t K = 0;
    for (int I = 0; I < Segments; ++I) {          // triangle LIST, not a fan — see the header note
        OutI[K++] = 0;
        OutI[K++] = static_cast<uint32_t>(1 + I);
        OutI[K++] = static_cast<uint32_t>(2 + I);
    }
    VCount = static_cast<uint32_t>(Segments + 2);
    ICount = K;
}

// ---- Upload wrappers --------------------------------------------------------------------------

inline MeshHandle MakeGradientStripV(IRenderer* R, const GradStop* Stops, int N, float Alpha = 1.0f) {
    Vertex V[2 * MaxGradStops];
    uint32_t Idx[6 * (MaxGradStops - 1)];
    uint32_t VC = 0, IC = 0;
    if (BuildGradientStripV(Stops, N, Alpha, V, Idx, VC, IC) == 0) return 0;
    return R->CreateMesh(V, VC, Idx, IC);
}

inline MeshHandle MakeGradientStripH(IRenderer* R, const GradStop* Stops, int N, float Alpha = 1.0f) {
    Vertex V[2 * MaxGradStops];
    uint32_t Idx[6 * (MaxGradStops - 1)];
    uint32_t VC = 0, IC = 0;
    if (BuildGradientStripH(Stops, N, Alpha, V, Idx, VC, IC) == 0) return 0;
    return R->CreateMesh(V, VC, Idx, IC);
}

inline MeshHandle MakeDiscMesh(IRenderer* R, int Segments) {
    Vertex V[MaxDiscSegments + 2];
    uint32_t Idx[3 * MaxDiscSegments];
    uint32_t VC = 0, IC = 0;
    BuildDisc(Segments, V, Idx, VC, IC);
    return R->CreateMesh(V, VC, Idx, IC);
}

}  // namespace Lur::Render
