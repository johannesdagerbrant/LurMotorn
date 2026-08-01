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

// Per-instance data for DrawInstances — a quad whose centre lerps Prev->Cur by the
// alpha push constant (interpolation happens in the vertex shader, so per-instance CPU
// cost is a memcpy, not a lerp). PIXEL SPACE: the game maps world->pixels when building
// the array, keeping the renderer generic (mix + MVP, like sprites). Field ORDER is the
// vertex-attribute layout the instanced pipeline binds — do not reorder.
struct InstanceData {
    float PrevX, PrevY;   // pixel centre at the previous tick
    float CurX, CurY;     // pixel centre at the current tick
    float R, G, B, A;     // tint (multiplied by the atlas shade channel)
    float Size;           // pixel size (quad spans Size x Size, centred on the lerp)
    // Atlas UV rect (U0,V0)-(U1,V1) for this instance's glyph. With a flat/white
    // material the default texture samples shade=coverage=1, so {0,0,1,1} + any rect
    // still draws a plain tinted quad — untextured callers just leave this zeroed
    // and pass a flat material.
    float U0 = 0.0f, V0 = 0.0f, U1 = 1.0f, V1 = 1.0f;
    // Optional facing: if non-zero, the quad is rotated so the glyph's TOP edge points
    // along this direction (pixel space). {0,0} (the default) draws upright. The caller
    // decides what "facing" means (velocity, a target, a blend) — the shader just orients.
    float FaceX = 0.0f, FaceY = 0.0f;
};

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
    Color         Outline{};       // ink-band colour (only used when InkHi > InkLo)
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
    // Optional: wait for the previous frame's GPU work + acquire the next image, up front.
    // Call this at the TOP of the loop, BEFORE sampling input, so the ~vsync fence-wait
    // idle happens ahead of input rather than after it — the presented frame then carries
    // the freshest input (cuts ~1 frame of touch/scroll latency). BeginFrame does it
    // lazily if you don't, so callers that skip this are unaffected. Default no-op.
    virtual void WaitForFrame() {}

    virtual void BeginFrame(const Camera& Camera) = 0;

    // Enter the GUI layer: everything drawn after this call composites ON TOP of the
    // world pass, drawn by an engine-owned ORTHOGRAPHIC camera sized to the
    // framebuffer (pixels, top-left origin, Y-down — same convention as
    // MakeOrthoCamera). No camera argument by design: "always ortho, engine-sized" is
    // the invariant, so game code never builds a UI matrix by hand. A 3D game runs a
    // perspective world camera then this ortho GUI layer; a 2D game's world camera is
    // already ortho, so the switch is a no-op-shaped repeat.
    //
    // The world pass has no depth attachment today (colour-only), so this is a camera
    // swap plus painter's-order submission. When the world pass gains depth for 3D,
    // GUI pipelines must bind with depth test/write disabled here so GUI always paints
    // over the scene. Default no-op so non-graphical backends need not implement it.
    virtual void BeginGui() {}

    // Enter the DEV-GUI layer: a THIRD pass after the game's GUI (#113, spec Addendum A),
    // same engine ortho, depth off — the console + dev panels always paint over everything.
    // Default no-op (like BeginGui), and its call site is compiled out in shipping via
    // Lur/Render/DevGuiLayer.h's BeginDevGuiLayer wrapper, so the pass never occurs in a
    // shipped build. Declared unconditionally to keep the vtable identical across configs.
    virtual void BeginDevGui() {}

    virtual void DrawMesh(MeshHandle Mesh, MaterialHandle Material, const Math::Mat4& Model) = 0;

    // Draw `Count` instances of `Quad` (a MakeQuad mesh) in ONE call, each transformed
    // per InstanceData and interpolated Prev->Cur by `Alpha` in the vertex shader. The
    // instance array is transient (rebuilt each frame in pixel space) and sub-allocated
    // from a per-frame arena, like DrawGlyphs. This is the RTS unit path: thousands of
    // units, one draw. `Material` supplies the glyph atlas (RG8 shade+coverage, sampled
    // by each instance's UV rect): per-instance colour is the fill, coverage the cutout
    // — the RTS silhouette-tint path. A flat material (BaseColor 0) keeps the old
    // plain-tinted-quad behaviour. Default no-op so non-graphical/host backends need
    // not implement it.
    virtual void DrawInstances(MeshHandle Quad, const InstanceData* Instances, uint32_t Count,
                               float Alpha, MaterialHandle Material) {
        (void)Quad; (void)Instances; (void)Count; (void)Alpha; (void)Material;
    }

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

    // Frames actually handed to the display since Init (i.e. vkQueuePresentKHR returned
    // success). The one number that distinguishes "rendering but invisible" from "not
    // presenting at all" — a black screen with this advancing is a compositor problem;
    // stuck at 0 it's a dead swapchain (issue #73). Cheap enough to poll every frame;
    // surfaced in the apps' periodic diag log lines. Default 0 for non-presenting
    // backends.
    virtual uint32_t PresentedFrames() const { return 0; }
};

} // namespace Lur::Render
