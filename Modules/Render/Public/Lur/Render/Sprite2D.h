#pragma once
#include "Lur/Render/Renderer.h"

namespace Lur::Render {

// Thin 2D helpers over the general renderer: a flat board is just an orthographic
// camera plus quad meshes. This is the "2D layer on top of a 3D-capable renderer"
// — chess uses these, while 3D games use Camera/meshes directly.

// Pixel-space orthographic camera: (0,0) top-left, (Width,Height) bottom-right.
inline Camera MakeOrthoCamera(float Width, float Height) {
    Camera C;
    C.View = Math::Mat4::Identity();
    C.Projection = Math::Mat4::Ortho(0.0f, Width, Height, 0.0f, -1.0f, 1.0f);
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
