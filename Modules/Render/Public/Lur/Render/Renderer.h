#pragma once
#include <cstdint>
#include "Lur/Math/Mat4.h"
#include "Lur/Math/Vec.h"

namespace Lur::Render {

// General mesh vertex — enough for both 2D sprites (Position.Z = 0, Normal unused)
// and lit 3D meshes. One vertex layout keeps the backend simple while leaving the
// door open to 3D models.
struct Vertex {
    Math::Vec3 Position;
    Math::Vec3 Normal;
    Math::Vec2 Uv;
    Math::Vec4 Color;
};

struct Color { float R = 1.0f, G = 1.0f, B = 1.0f, A = 1.0f; };

// Opaque GPU resource handles (0 = none / invalid).
using MeshHandle     = uint32_t;
using TextureHandle  = uint32_t;
using MaterialHandle = uint32_t;

// A material is a shader plus its parameters. 2D sprites use an unlit textured
// material; 3D meshes can opt into lighting. Kept tiny on purpose — richer PBR
// fields slot in here later without changing the interface.
struct MaterialDesc {
    TextureHandle BaseColor = 0;   // 0 = flat white
    Color         Tint;
    bool          Lit = false;     // unlit for 2D, lit for 3D
};

// View + projection. Projection is Mat4::Ortho() for 2D and Mat4::Perspective()
// for 3D — at this layer, that is the only difference between a flat board and a
// 3D scene.
struct Camera {
    Math::Mat4 View;
    Math::Mat4 Projection;
};

// The renderer games draw against. One implementation today: Vulkan, running
// natively on Android and via MoltenVK on iOS. Deliberately 3D-capable from the
// start — meshes + depth + camera + materials — with 2D as the orthographic
// special case (see Sprite2D.h). That keeps 3D games open without a rewrite.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // --- Lifecycle. NativeWindow is ANativeWindow* (Android) / CAMetalLayer* (iOS). ---
    virtual bool Init(void* NativeWindow) = 0;
    virtual void Resize(int WidthPx, int HeightPx) = 0;
    virtual void Shutdown() = 0;

    // --- Resources (created once, reused across frames). ---
    virtual MeshHandle     CreateMesh(const Vertex* Vertices, uint32_t VertexCount,
                                      const uint32_t* Indices, uint32_t IndexCount) = 0;
    virtual TextureHandle  LoadTexture(const uint8_t* Rgba, int Width, int Height) = 0;
    virtual MaterialHandle CreateMaterial(const MaterialDesc& Desc) = 0;

    // --- Per-frame. ---
    virtual void BeginFrame(const Camera& Camera) = 0;
    virtual void DrawMesh(MeshHandle Mesh, MaterialHandle Material, const Math::Mat4& Model) = 0;
    virtual void EndFrame() = 0;  // submit + present
};

} // namespace Lur::Render
