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

// Pixel layout of the bytes handed to LoadTexture. Rgba8 is the general case;
// Rg8 is the slim two-channel case (2 bytes/texel) used by the tint-trick sprites
// — R carries a shade/luminance value, G carries coverage (silhouette alpha) —
// so the piece art keeps its tonal detail without a full RGBA upload (issue #30).
enum class ETextureFormat { Rgba8, Rg8 };

// A material is a shader plus its parameters. 2D sprites use an unlit textured
// material; 3D meshes can opt into lighting. Kept tiny on purpose — richer PBR
// fields slot in here later without changing the interface.
//
// The sprite shading is deliberately GENERIC — the fields below are engine knobs a
// game sets per material, not game-specific logic baked into the shader:
//   * Tint     — the fill colour (multiplied by the texture's shade channel, R).
//   * Gamma    — tone curve on the fill's shade; 1 = linear. >1 deepens the darker
//                tones while leaving highlights (shade ~1) put; <1 lifts them.
//   * Outline  — colour that the texture's dark "ink" band maps to (see Ink below).
//   * InkLo/Hi — the shade band treated as "ink": shade below the band blends fully
//                to Outline, above it stays the tinted fill. Disabled (plain
//                tint×shade) when InkHi <= InkLo, which is the default.
// Chess uses these to render both piece colours from one mask set: e.g. a dark
// outline + gentle gamma for white pieces, a white outline + steep gamma for black.
struct MaterialDesc {
    TextureHandle BaseColor = 0;   // 0 = flat white
    Color         Tint;
    bool          Lit = false;     // unlit for 2D, lit for 3D
    Color         Outline;         // ink-band colour (only used when InkHi > InkLo)
    float         Gamma = 1.0f;    // fill tone curve; 1 = linear (no change)
    float         InkLo = 0.0f;    // ink band; InkHi <= InkLo disables the recolour
    float         InkHi = 0.0f;
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
    virtual TextureHandle  LoadTexture(const uint8_t* Data, int Width, int Height,
                                       ETextureFormat Format = ETextureFormat::Rgba8) = 0;
    virtual MaterialHandle CreateMaterial(const MaterialDesc& Desc) = 0;

    // --- Per-frame. ---
    virtual void BeginFrame(const Camera& Camera) = 0;
    virtual void DrawMesh(MeshHandle Mesh, MaterialHandle Material, const Math::Mat4& Model) = 0;

    // Draw a batch of dynamic 2D glyph quads (MSDF text) in one call. Vertices/Indices
    // are transient — the HUD text layer rebuilds them each frame in pixel space
    // (screen-fixed); Indices are 0-based into Vertices. `Atlas` is a material bound to
    // the font's MSDF atlas texture (its Tint multiplies the per-vertex colour), and
    // `DistanceRange` is the font's msdfgen -pxrange (atlas texels). Uses the MSDF text
    // pipeline. Default no-op so non-text backends need not implement it.
    virtual void DrawGlyphs(const Vertex* Vertices, uint32_t VertexCount,
                            const uint32_t* Indices, uint32_t IndexCount,
                            MaterialHandle Atlas, float DistanceRange) {
        (void)Vertices; (void)VertexCount; (void)Indices; (void)IndexCount;
        (void)Atlas; (void)DistanceRange;
    }

    virtual void EndFrame() = 0;  // submit + present
};

} // namespace Lur::Render
