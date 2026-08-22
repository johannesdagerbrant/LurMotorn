#pragma once
#include "Lur/Render/Renderer.h"

namespace Lur::Render {

// Thin 2D helpers over the general renderer: a flat board is just an orthographic
// camera plus quad meshes. This is the "2D layer on top of a 3D-capable renderer"
// — a 2D game uses these, while a 3D game uses Camera/meshes directly.

// A flat-colour, unlit material: no texture, so the TINT is the colour.
//
// Promoted in #201 from five independent copies of the same five lines — `FlatMat` in
// Rps::GameView and in Lur::DevGui::Console, plus bare `MaterialDesc{0, C, false}` aggregates in
// Chess::BoardView (x4), Lur::Hud::Dropdown and Lur::Hud::TextField. Every 2D surface in the tree
// needs it, which is what makes it a helper rather than a game's idiom.
//
// `BaseColor = 0` is the load-bearing bit and it reads as a mistake until you know the contract:
// texture handle 0 means "flat white" (see MaterialDesc), and the shader multiplies sampled colour
// by tint — so sampling white and tinting is how you get a solid colour without a 1x1 texture.
//
// The two named copies had DRIFTED, harmlessly: one set `Lit = false` explicitly and one relied on
// the default. Same result today, and precisely the kind of difference that stops being harmless the
// day the default changes.
inline MaterialHandle MakeFlatMaterial(IRenderer* R, Color C) {
    MaterialDesc D;
    D.BaseColor = 0;   // 0 = flat white; the tint supplies the colour
    D.Tint = C;
    D.Lit = false;     // 2D: never lit. Explicit, not inherited from the default.
    return R->CreateMaterial(D);
}

// Pixel-space orthographic camera: (0,0) top-left, (Width,Height) bottom-right.
//
// Vulkan clip space is Y-down (NDC y=-1 is the TOP of the framebuffer), so a
// top-left pixel origin maps Bottom=0 -> Top=Height: world y=0 lands at NDC y=-1
// (top) and y=Height at NDC y=+1 (bottom). This keeps the Y flip in the
// projection, valid on the Vulkan 1.0 baseline — no negative-height viewport
// (which would need KHR_maintenance1). Verified against the Android backend.
inline Camera MakeOrthoCamera(float Width, float Height) {
    Camera C;
    C.View = Math::Mat4::Identity();
    C.Projection = Math::Mat4::Ortho(0.0f, Width, 0.0f, Height, -1.0f, 1.0f);
    return C;
}

// A unit quad on the Z=0 plane, spanning (0,0)-(1,1). Position/size it with the
// model matrix at draw time. Uploaded once via IRenderer::CreateMesh.
struct Quad {
    Vertex   Vertices[4];
    uint32_t Indices[6];
};

inline Quad MakeQuad(Color Tint = {}) {
    const Math::Vec4 C{Tint.R, Tint.G, Tint.B, Tint.A};
    const Math::Vec3 N{0.0f, 0.0f, 1.0f};
    Quad Q{};
    Q.Vertices[0] = {{0.0f, 0.0f, 0.0f}, N, {0.0f, 0.0f}, C};
    Q.Vertices[1] = {{1.0f, 0.0f, 0.0f}, N, {1.0f, 0.0f}, C};
    Q.Vertices[2] = {{1.0f, 1.0f, 0.0f}, N, {1.0f, 1.0f}, C};
    Q.Vertices[3] = {{0.0f, 1.0f, 0.0f}, N, {0.0f, 1.0f}, C};
    Q.Indices[0] = 0; Q.Indices[1] = 1; Q.Indices[2] = 2;  // triangle list (not fans —
    Q.Indices[3] = 0; Q.Indices[4] = 2; Q.Indices[5] = 3;  // MoltenVK has no triangle fans)
    return Q;
}

} // namespace Lur::Render
