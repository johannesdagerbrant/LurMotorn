// Shared Vulkan backend — compiles identically for Android (native Vulkan) and
// iOS (Vulkan via MoltenVK). The ONLY platform-specific pieces are behind the
// Lur::Render::Vk seam (PlatformSurface.h): surface creation, the surface
// instance extensions, drawable size, and logging. Everything here — swapchain,
// the 2D textured-quad pipeline, buffers, textures, descriptors, per-frame draw —
// is shared.
//
// Targets the Vulkan portability subset (triangle-list, standard formats, no
// geometry/tessellation, dynamic viewport) so it runs unchanged through MoltenVK.
#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Render/Vulkan/PlatformSurface.h"
#include "Lur/Render/Sprite2D.h"   // MakeOrthoCamera — canonical pixel-space GUI camera

// Older SDK headers may predate VK_KHR_portability_enumeration; define the flag
// so the source compiles everywhere (only ever set when the extension is present).
#ifndef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

// SPIR-V cooked from Shaders/*.glsl (scripts/gen-shaders.ps1), committed as uint32
// arrays so no shader compiler is needed in either platform's build.
static const uint32_t SpriteVertSpv[] = {
#include "Shaders/Sprite.vert.inc"
};
static const uint32_t SpriteFragSpv[] = {
#include "Shaders/Sprite.frag.inc"
};
static const uint32_t TextVertSpv[] = {
#include "Shaders/Text.vert.inc"
};
static const uint32_t TextFragSpv[] = {
#include "Shaders/Text.frag.inc"
};
static const uint32_t InstancedVertSpv[] = {
#include "Shaders/Instanced.vert.inc"
};
static const uint32_t InstancedFragSpv[] = {
#include "Shaders/Instanced.frag.inc"
};

namespace Lur::Render {
namespace {

void LogF(bool Error, const char* Fmt, ...) {
    char Buf[256];
    va_list Args;
    va_start(Args, Fmt);
    std::vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);
    Vk::PlatformLog(Error, Buf);
}
#define LOGI(...) LogF(false, __VA_ARGS__)
#define LOGE(...) LogF(true, __VA_ARGS__)

// Log + carry on. A half-initialised renderer just clears to the background,
// itself a visible signal — more useful than aborting on these dev devices.
#define VK_CHECK(Expr)                                                              \
    do {                                                                            \
        VkResult Result_ = (Expr);                                                  \
        if (Result_ != VK_SUCCESS)                                                  \
            LOGE("Vulkan call failed (%d) at %s:%d", Result_, __FILE__, __LINE__);  \
    } while (0)

// Swapchain / device-loss policy (#93, decision #4): mobile GPUs hit transient losses
// on thermal spikes / rotation, so in Development (and Shipping) we HEAL + loud-log
// (the #73 self-healer recreates the swapchain; a genuine device-loss just logs and the
// next frames retry). Only the dedicated Debugging build (LUR_SLOW) TRAPS on a real
// VK_ERROR_DEVICE_LOST, for maximum signal when someone is actively debugging it.
#if defined(LUR_SLOW) && LUR_SLOW
    #define LUR_ON_DEVICE_LOST(Where) \
        do { LOGE("VK_ERROR_DEVICE_LOST at %s — trapping (Debugging)", Where); __builtin_trap(); } while (0)
#else
    #define LUR_ON_DEVICE_LOST(Where) \
        do { LOGE("VK_ERROR_DEVICE_LOST at %s — healing (Development)", Where); } while (0)
#endif

// Pushed per draw. mat4 (64) + 3x vec4 (48) = 112 bytes, under the 128-byte portable
// minimum. One layout is shared by both pipelines: the sprite shader reads Tint +
// Outline + Shape.xyz (ink band + gamma); the MSDF text shader reads Tint and the
// distance range from Shape.w. Unused fields are simply left zero per draw.
struct PushConstants {
    float Mvp[16];      // offset 0
    float Tint[4];      // offset 64  fill colour
    float Outline[4];   // offset 80  sprite ink-band colour
    float Shape[4];     // offset 96  x=InkLo, y=InkHi, z=Gamma, w=DistanceRange (text)
};

struct Mesh {
    VkBuffer       VertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory VertexMemory = VK_NULL_HANDLE;
    VkBuffer       IndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory IndexMemory = VK_NULL_HANDLE;
    uint32_t       IndexCount = 0;
};

struct Texture {
    VkImage        Image = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkImageView    View = VK_NULL_HANDLE;
};

struct Material {
    Color           Tint;
    Color           Outline;                         // ink-band colour (see MaterialDesc)
    float           Gamma = 1.0f;
    float           InkLo = 0.0f;
    float           InkHi = 0.0f;
    VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;  // binds the base-colour texture
};

class VulkanRendererImpl : public IRenderer {
public:
    bool Init(void* NativeWindow) override {
        Window = NativeWindow;
        if (Window == nullptr) {
            LOGE("Renderer Init: null native window");
            return false;
        }
        if (!CreateInstance())         return false;
        if (!CreateSurface())          return false;
        if (!PickPhysicalDevice())     return false;
        if (!CreateDevice())           return false;
        if (!CreateCommandResources()) return false;
        if (!CreateSyncObjects())      return false;
        if (!CreateSwapchain())        return false;
        if (!CreateDescriptorResources()) return false;
        if (!CreatePipeline())         return false;
        if (!CreateTextBuffers())      return false;
        if (!CreateInstanceBuffer())   return false;
        const uint8_t White[4] = {255, 255, 255, 255};
        DefaultTexture = LoadTexture(White, 1, 1, ETextureFormat::Rgba8);
        if (DefaultTexture == 0)       return false;
        LOGI("Vulkan renderer up: %ux%u, %u swapchain images",
             Extent.width, Extent.height, static_cast<uint32_t>(SwapImages.size()));
        Ready = true;
        return true;
    }

    void Resize(int /*WidthPx*/, int /*HeightPx*/) override { NeedsRecreate = true; }

    void Shutdown() override {
        if (Device != VK_NULL_HANDLE) vkDeviceWaitIdle(Device);
        for (Mesh& M : Meshes) DestroyMesh(M);
        Meshes.clear();
        for (Texture& T : Textures) DestroyTexture(T);
        Textures.clear();
        Materials.clear();  // sets freed with the pool below
        if (Device != VK_NULL_HANDLE) {
            if (TextVB != VK_NULL_HANDLE)    vkDestroyBuffer(Device, TextVB, nullptr);
            if (TextVBMem != VK_NULL_HANDLE) vkFreeMemory(Device, TextVBMem, nullptr);
            if (TextIB != VK_NULL_HANDLE)    vkDestroyBuffer(Device, TextIB, nullptr);
            if (TextIBMem != VK_NULL_HANDLE) vkFreeMemory(Device, TextIBMem, nullptr);
            if (InstanceVB != VK_NULL_HANDLE)    vkDestroyBuffer(Device, InstanceVB, nullptr);
            if (InstanceVBMem != VK_NULL_HANDLE) vkFreeMemory(Device, InstanceVBMem, nullptr);
            if (InstancePipeline != VK_NULL_HANDLE) vkDestroyPipeline(Device, InstancePipeline, nullptr);
            if (InstanceLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(Device, InstanceLayout, nullptr);
            if (TextPipeline != VK_NULL_HANDLE)   vkDestroyPipeline(Device, TextPipeline, nullptr);
            if (Pipeline != VK_NULL_HANDLE)       vkDestroyPipeline(Device, Pipeline, nullptr);
            if (PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
            if (DescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(Device, DescriptorPool, nullptr);
            if (DescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(Device, DescriptorSetLayout, nullptr);
            if (Sampler != VK_NULL_HANDLE)        vkDestroySampler(Device, Sampler, nullptr);
        }
        DestroySwapchain();
        if (Device != VK_NULL_HANDLE) {
            if (RenderFinished != VK_NULL_HANDLE) vkDestroySemaphore(Device, RenderFinished, nullptr);
            if (ImageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(Device, ImageAvailable, nullptr);
            if (InFlight != VK_NULL_HANDLE)       vkDestroyFence(Device, InFlight, nullptr);
            if (CommandPool != VK_NULL_HANDLE)    vkDestroyCommandPool(Device, CommandPool, nullptr);
            vkDestroyDevice(Device, nullptr);
        }
        if (Surface != VK_NULL_HANDLE && Instance != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(Instance, Surface, nullptr);
        if (Instance != VK_NULL_HANDLE) vkDestroyInstance(Instance, nullptr);
        *this = VulkanRendererImpl{};
    }

    MeshHandle CreateMesh(const Vertex* Vertices, uint32_t VertexCount,
                          const uint32_t* Indices, uint32_t IndexCount) override {
        Mesh M;
        M.IndexCount = IndexCount;
        if (!CreateBuffer(static_cast<VkDeviceSize>(VertexCount) * sizeof(Vertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, Vertices,
                          M.VertexBuffer, M.VertexMemory)) return 0;
        if (!CreateBuffer(static_cast<VkDeviceSize>(IndexCount) * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, Indices,
                          M.IndexBuffer, M.IndexMemory)) return 0;
        Meshes.push_back(M);
        return static_cast<MeshHandle>(Meshes.size());
    }

    TextureHandle LoadTexture(const uint8_t* Data, int Width, int Height,
                              ETextureFormat Format) override {
        // R8G8_UNORM for the slim two-channel (shade + coverage) sprites, else
        // R8G8B8A8_UNORM. Both are mandatory optimal-tiling sampled formats, so no
        // feature query is needed (holds on MoltenVK's portability subset too).
        const int      Bpp   = (Format == ETextureFormat::Rg8) ? 2 : 4;
        const VkFormat VkFmt = (Format == ETextureFormat::Rg8) ? VK_FORMAT_R8G8_UNORM
                                                               : VK_FORMAT_R8G8B8A8_UNORM;
        const VkDeviceSize Size = static_cast<VkDeviceSize>(Width) * Height * Bpp;

        VkBuffer Staging = VK_NULL_HANDLE;
        VkDeviceMemory StagingMem = VK_NULL_HANDLE;
        if (!CreateBuffer(Size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Data, Staging, StagingMem))
            return 0;

        Texture Tex;
        VkImageCreateInfo ImgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ImgInfo.imageType = VK_IMAGE_TYPE_2D;
        ImgInfo.format = VkFmt;
        ImgInfo.extent = {static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), 1};
        ImgInfo.mipLevels = 1;
        ImgInfo.arrayLayers = 1;
        ImgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        ImgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        ImgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ImgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ImgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(Device, &ImgInfo, nullptr, &Tex.Image) != VK_SUCCESS) {
            LOGE("vkCreateImage failed (%dx%d)", Width, Height);
            vkDestroyBuffer(Device, Staging, nullptr); vkFreeMemory(Device, StagingMem, nullptr);
            return 0;
        }

        VkMemoryRequirements Req{};
        vkGetImageMemoryRequirements(Device, Tex.Image, &Req);
        VkMemoryAllocateInfo Alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        Alloc.allocationSize = Req.size;
        Alloc.memoryTypeIndex = FindMemoryType(Req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(Device, &Alloc, nullptr, &Tex.Memory);
        vkBindImageMemory(Device, Tex.Image, Tex.Memory, 0);

        VkCommandBuffer Cmd = BeginOneTimeCommands();
        TransitionLayout(Cmd, Tex.Image, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy Copy{};
        Copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Copy.imageSubresource.layerCount = 1;
        Copy.imageExtent = {static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), 1};
        vkCmdCopyBufferToImage(Cmd, Staging, Tex.Image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Copy);
        TransitionLayout(Cmd, Tex.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EndOneTimeCommands(Cmd);

        vkDestroyBuffer(Device, Staging, nullptr);
        vkFreeMemory(Device, StagingMem, nullptr);

        VkImageViewCreateInfo ViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ViewInfo.image = Tex.Image;
        ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ViewInfo.format = VkFmt;
        ViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ViewInfo.subresourceRange.levelCount = 1;
        ViewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(Device, &ViewInfo, nullptr, &Tex.View);

        Textures.push_back(Tex);
        return static_cast<TextureHandle>(Textures.size());
    }

    MaterialHandle CreateMaterial(const MaterialDesc& Desc) override {
        Material M;
        M.Tint = Desc.Tint;
        M.Outline = Desc.Outline;
        M.Gamma = Desc.Gamma;
        M.InkLo = Desc.InkLo;
        M.InkHi = Desc.InkHi;
        const TextureHandle Tex = (Desc.BaseColor != 0) ? Desc.BaseColor : DefaultTexture;
        M.DescriptorSet = AllocateDescriptorSet(Textures[Tex - 1].View);
        Materials.push_back(M);
        return static_cast<MaterialHandle>(Materials.size());
    }

    // Wait for the previous frame's GPU work and acquire the next image. SPLIT OUT of
    // BeginFrame so the app loop can call it BEFORE it samples input: the ~vsync fence-wait
    // idle then happens ahead of input, so the presented frame carries the FRESHEST input
    // instead of input captured a whole frame earlier (scroll/touch latency). Idempotent
    // per frame via FrameAcquired; BeginFrame calls it lazily for callers that don't.
    void WaitForFrame() override {
        if (!Ready || FrameAcquired) return;

        // Self-heal (issue #73): a NULL swapchain is a recreate TRIGGER, never terminal.
        // A relaunch can race the window server so the first recreate fails; every dead
        // frame retries, and a long streak escalates to rebuilding the surface. Before
        // this, one silent failure left the app running on a black screen forever.
        if (NeedsRecreate || Swapchain == VK_NULL_HANDLE) {
            RecreateSwapchain();
            NeedsRecreate = false;
        }
        if (Swapchain == VK_NULL_HANDLE) {
            if (++DeadFrames == 1 || DeadFrames % 60 == 0)
                LOGE("WaitForFrame: no swapchain for %u frame(s), retrying", DeadFrames);
            return;
        }

        vkWaitForFences(Device, 1, &InFlight, VK_TRUE, UINT64_MAX);

        VkResult Acq = vkAcquireNextImageKHR(Device, Swapchain, UINT64_MAX,
                                             ImageAvailable, VK_NULL_HANDLE, &ImageIndex);
        if (Acq == VK_ERROR_OUT_OF_DATE_KHR) {
            NeedsRecreate = true;
            if (++DeadFrames == 1 || DeadFrames % 60 == 0)
                LOGE("WaitForFrame: acquire OUT_OF_DATE (%u dead frame(s)), recreating", DeadFrames);
            return;
        }
        if (Acq != VK_SUCCESS && Acq != VK_SUBOPTIMAL_KHR) {
            if (Acq == VK_ERROR_DEVICE_LOST) LUR_ON_DEVICE_LOST("acquire");
            ++DeadFrames;
            LOGE("vkAcquireNextImageKHR failed (%d)", Acq);
            return;
        }
        FrameAcquired = true;
    }

    void BeginFrame(const Camera& Cam) override {
        CurrentCamera = Cam;
        Recording = false;
        if (!Ready) return;
        if (!FrameAcquired) WaitForFrame();  // caller didn't pre-wait (desktop/iOS) — wait now
        if (!FrameAcquired) return;          // still no image (dead frame / recreating)
        FrameAcquired = false;               // consume this frame's acquisition

        vkResetFences(Device, 1, &InFlight);
        vkResetCommandBuffer(CommandBuffer, 0);

        VkCommandBufferBeginInfo BeginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(CommandBuffer, &BeginInfo));

        VkClearValue Clear{};
        Clear.color = {{0.16f, 0.20f, 0.26f, 1.0f}};

        VkRenderPassBeginInfo Pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        Pass.renderPass = RenderPass;
        Pass.framebuffer = Framebuffers[ImageIndex];
        Pass.renderArea.extent = Extent;
        Pass.clearValueCount = 1;
        Pass.pClearValues = &Clear;
        vkCmdBeginRenderPass(CommandBuffer, &Pass, VK_SUBPASS_CONTENTS_INLINE);

        // Viewport/scissor are dynamic state (independent of the bound pipeline).
        // Pipelines are bound lazily per draw (DrawMesh/DrawGlyphs) so draw order can
        // freely mix meshes and text; the text arena resets each frame.
        VkViewport Vp{0.0f, 0.0f, static_cast<float>(Extent.width),
                      static_cast<float>(Extent.height), 0.0f, 1.0f};
        VkRect2D Sc{{0, 0}, Extent};
        vkCmdSetViewport(CommandBuffer, 0, 1, &Vp);
        vkCmdSetScissor(CommandBuffer, 0, 1, &Sc);
        BoundPipeline = VK_NULL_HANDLE;
        TextVBCursor = 0;
        TextIBCursor = 0;
        InstanceCursor = 0;
        Recording = true;
    }

    void BeginGui() override {
        // Switch to the engine-owned orthographic GUI camera sized to the framebuffer.
        // Everything drawn after this composites on top of the world pass (painter's
        // order — no depth attachment yet; see IRenderer::BeginGui). MakeOrthoCamera
        // is the single source of the pixel-space top-left / Y-down convention.
        CurrentCamera = MakeOrthoCamera(static_cast<float>(Extent.width),
                                        static_cast<float>(Extent.height));
    }

    void BeginDevGui() override {
        // #113: the DEV-GUI pass — a third phase after the game GUI, same engine ortho,
        // painter's-order over everything (no depth). Identical camera to BeginGui; the
        // separate entry point marks the dev overlay as its own layer (and lets a future
        // backend give it distinct state). Compiled out of shipping at the call site.
        CurrentCamera = MakeOrthoCamera(static_cast<float>(Extent.width),
                                        static_cast<float>(Extent.height));
    }

    void SetViewportRect(int X, int Y, int W, int H) override {
        // #115 desktop --tune split: draw into a framebuffer sub-rect with an ortho sized to
        // it. Viewport maps NDC to [X,X+W]x[Y,Y+H]; scissor clips there; ortho(W,H) makes
        // pixel coords [0,W]x[0,H] land in the rect.
        if (!Recording) return;
        VkViewport Vp{static_cast<float>(X), static_cast<float>(Y), static_cast<float>(W),
                      static_cast<float>(H), 0.0f, 1.0f};
        VkRect2D Sc{{X, Y}, {static_cast<uint32_t>(W), static_cast<uint32_t>(H)}};
        vkCmdSetViewport(CommandBuffer, 0, 1, &Vp);
        vkCmdSetScissor(CommandBuffer, 0, 1, &Sc);
        CurrentCamera = MakeOrthoCamera(static_cast<float>(W), static_cast<float>(H));
    }

    void BindPipeline(VkPipeline P) {
        if (BoundPipeline != P) {
            vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, P);
            BoundPipeline = P;
        }
    }

    void DrawMesh(MeshHandle MeshId, MaterialHandle MaterialId,
                  const Math::Mat4& Model) override {
        if (!Recording) return;
        if (MeshId == 0 || MeshId > Meshes.size()) return;
        if (MaterialId == 0 || MaterialId > Materials.size()) return;
        const Mesh& M = Meshes[MeshId - 1];
        const Material& Mat = Materials[MaterialId - 1];

        BindPipeline(Pipeline);

        PushConstants Pc{};
        const Math::Mat4 Mvp = CurrentCamera.Projection * CurrentCamera.View * Model;
        std::memcpy(Pc.Mvp, Mvp.M, sizeof(Pc.Mvp));
        Pc.Tint[0] = Mat.Tint.R; Pc.Tint[1] = Mat.Tint.G;
        Pc.Tint[2] = Mat.Tint.B; Pc.Tint[3] = Mat.Tint.A;
        Pc.Outline[0] = Mat.Outline.R; Pc.Outline[1] = Mat.Outline.G;
        Pc.Outline[2] = Mat.Outline.B; Pc.Outline[3] = Mat.Outline.A;
        Pc.Shape[0] = Mat.InkLo; Pc.Shape[1] = Mat.InkHi; Pc.Shape[2] = Mat.Gamma;
        vkCmdPushConstants(CommandBuffer, PipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(Pc), &Pc);

        vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                PipelineLayout, 0, 1, &Mat.DescriptorSet, 0, nullptr);

        VkDeviceSize Offset = 0;
        vkCmdBindVertexBuffers(CommandBuffer, 0, 1, &M.VertexBuffer, &Offset);
        vkCmdBindIndexBuffer(CommandBuffer, M.IndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(CommandBuffer, M.IndexCount, 1, 0, 0, 0);
    }

    void DrawGlyphs(const Vertex* Vertices, uint32_t VertexCount,
                    const uint32_t* Indices, uint32_t IndexCount,
                    MaterialHandle MaterialId, float DistanceRange) override {
        if (!Recording || VertexCount == 0 || IndexCount == 0) return;
        if (MaterialId == 0 || MaterialId > Materials.size()) return;
        // Sub-allocate this batch from the per-frame arena; drop it (visibly) if full.
        if (TextVBCursor + VertexCount > MaxTextVertices ||
            TextIBCursor + IndexCount  > MaxTextIndices) {
            LOGE("text arena full (%u verts / %u idx) — batch dropped",
                 TextVBCursor + VertexCount, TextIBCursor + IndexCount);
            return;
        }
        const Material& Mat = Materials[MaterialId - 1];

        std::memcpy(static_cast<Vertex*>(TextVBMapped) + TextVBCursor,
                    Vertices, static_cast<size_t>(VertexCount) * sizeof(Vertex));
        std::memcpy(static_cast<uint32_t*>(TextIBMapped) + TextIBCursor,
                    Indices, static_cast<size_t>(IndexCount) * sizeof(uint32_t));

        BindPipeline(TextPipeline);

        PushConstants Pc{};
        const Math::Mat4 Mvp = CurrentCamera.Projection * CurrentCamera.View;  // model = identity
        std::memcpy(Pc.Mvp, Mvp.M, sizeof(Pc.Mvp));
        Pc.Tint[0] = Mat.Tint.R; Pc.Tint[1] = Mat.Tint.G;
        Pc.Tint[2] = Mat.Tint.B; Pc.Tint[3] = Mat.Tint.A;
        Pc.Shape[3] = DistanceRange;   // text reads the msdf range from Shape.w
        vkCmdPushConstants(CommandBuffer, PipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(Pc), &Pc);

        vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                PipelineLayout, 0, 1, &Mat.DescriptorSet, 0, nullptr);

        VkDeviceSize Offset = 0;
        vkCmdBindVertexBuffers(CommandBuffer, 0, 1, &TextVB, &Offset);
        vkCmdBindIndexBuffer(CommandBuffer, TextIB, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(CommandBuffer, IndexCount, 1, TextIBCursor,
                         static_cast<int32_t>(TextVBCursor), 0);

        TextVBCursor += VertexCount;
        TextIBCursor += IndexCount;
    }

    void DrawInstances(MeshHandle MeshId, const InstanceData* Instances, uint32_t Count,
                       float Alpha, MaterialHandle MaterialId) override {
        if (!Recording || Count == 0) return;
        if (MeshId == 0 || MeshId > Meshes.size()) return;
        if (MaterialId == 0 || MaterialId > Materials.size()) return;
        if (InstanceCursor + Count > MaxInstances) {
            LOGE("instance arena full (%u) — batch dropped", InstanceCursor + Count);
            return;
        }
        const Mesh& M = Meshes[MeshId - 1];
        const Material& Mat = Materials[MaterialId - 1];

        std::memcpy(static_cast<InstanceData*>(InstanceMapped) + InstanceCursor,
                    Instances, static_cast<size_t>(Count) * sizeof(InstanceData));

        BindPipeline(InstancePipeline);

        PushConstants Pc{};
        const Math::Mat4 Mvp = CurrentCamera.Projection * CurrentCamera.View;  // model in the instance data
        std::memcpy(Pc.Mvp, Mvp.M, sizeof(Pc.Mvp));
        Pc.Shape[0] = Alpha;  // the vertex shader lerps Prev->Cur by this
        vkCmdPushConstants(CommandBuffer, InstanceLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(Pc), &Pc);

        // The glyph atlas (RG8 shade+coverage). Flat materials bind the default 1x1
        // white texture, which keeps the plain-tinted-quad behaviour.
        vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                InstanceLayout, 0, 1, &Mat.DescriptorSet, 0, nullptr);

        // Binding 0 = the shared quad; binding 1 = this batch's instances (bound at the
        // arena offset, so firstInstance stays 0).
        VkBuffer VBs[2] = {M.VertexBuffer, InstanceVB};
        VkDeviceSize Offsets[2] = {0, static_cast<VkDeviceSize>(InstanceCursor) * sizeof(InstanceData)};
        vkCmdBindVertexBuffers(CommandBuffer, 0, 2, VBs, Offsets);
        vkCmdBindIndexBuffer(CommandBuffer, M.IndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(CommandBuffer, M.IndexCount, Count, 0, 0, 0);

        InstanceCursor += Count;
    }

    void EndFrame() override {
        if (!Ready || !Recording) return;
        Recording = false;

        vkCmdEndRenderPass(CommandBuffer);
        VK_CHECK(vkEndCommandBuffer(CommandBuffer));

        VkPipelineStageFlags WaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo Submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        Submit.waitSemaphoreCount = 1;
        Submit.pWaitSemaphores = &ImageAvailable;
        Submit.pWaitDstStageMask = &WaitStage;
        Submit.commandBufferCount = 1;
        Submit.pCommandBuffers = &CommandBuffer;
        Submit.signalSemaphoreCount = 1;
        Submit.pSignalSemaphores = &RenderFinished;
        VK_CHECK(vkQueueSubmit(GraphicsQueue, 1, &Submit, InFlight));

        VkPresentInfoKHR Present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        Present.waitSemaphoreCount = 1;
        Present.pWaitSemaphores = &RenderFinished;
        Present.swapchainCount = 1;
        Present.pSwapchains = &Swapchain;
        Present.pImageIndices = &ImageIndex;
        VkResult Pres = vkQueuePresentKHR(GraphicsQueue, &Present);
        if (Pres == VK_SUCCESS || Pres == VK_SUBOPTIMAL_KHR) {
            // SUBOPTIMAL still presented — count it, but also refresh the swapchain.
            ++FramesPresented;
            if (FramesPresented == 1)
                LOGI("first frame presented (%ux%u)", Extent.width, Extent.height);
            if (DeadFrames > 0) {
                LOGI("present recovered after %u dead frame(s) (%ux%u, recreate #%u)",
                     DeadFrames, Extent.width, Extent.height, RecreateCount);
                DeadFrames = 0;
            }
        }
        if (Pres == VK_ERROR_OUT_OF_DATE_KHR || Pres == VK_SUBOPTIMAL_KHR) {
            NeedsRecreate = true;
            if (Pres == VK_ERROR_OUT_OF_DATE_KHR)
                LOGE("present OUT_OF_DATE, recreating");
        } else if (Pres != VK_SUCCESS) {
            if (Pres == VK_ERROR_DEVICE_LOST) LUR_ON_DEVICE_LOST("present");
            LOGE("vkQueuePresentKHR failed (%d)", Pres);
        }
    }

    uint32_t PresentedFrames() const override { return FramesPresented; }

private:
    // ---- Init steps ----

    bool CreateInstance() {
        VkApplicationInfo App{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        App.pApplicationName = "OnlyChess";
        App.apiVersion = VK_API_VERSION_1_0;

        // Which instance extensions does this Vulkan implementation actually expose?
        // VK_KHR_portability_enumeration is a LOADER extension: it lets the loader
        // enumerate portability drivers. On iOS we link MoltenVK statically and call
        // it directly (no loader), so MoltenVK doesn't advertise it — requesting it
        // unconditionally made vkCreateInstance fail with VK_ERROR_EXTENSION_NOT_
        // PRESENT (-7). Add each requested extension only if it's present.
        uint32_t AvailCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &AvailCount, nullptr);
        std::vector<VkExtensionProperties> Available(AvailCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &AvailCount, Available.data());
        auto Has = [&](const char* Name) {
            for (const VkExtensionProperties& E : Available)
                if (std::strcmp(E.extensionName, Name) == 0) return true;
            return false;
        };

        uint32_t PlatCount = 0;
        const char* const* PlatExts = Vk::PlatformSurfaceExtensions(&PlatCount);
        std::vector<const char*> Exts;
        Exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        bool Portability = false;
        for (uint32_t i = 0; i < PlatCount; ++i) {
            const bool IsPortabilityEnum =
                std::strcmp(PlatExts[i], "VK_KHR_portability_enumeration") == 0;
            if (IsPortabilityEnum && !Has(PlatExts[i])) continue;  // loader-only; skip
            Exts.push_back(PlatExts[i]);
            if (IsPortabilityEnum) Portability = true;
        }

        VkInstanceCreateInfo Info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        Info.pApplicationInfo = &App;
        Info.enabledExtensionCount = static_cast<uint32_t>(Exts.size());
        Info.ppEnabledExtensionNames = Exts.data();
        if (Portability) Info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        VkResult R = vkCreateInstance(&Info, nullptr, &Instance);
        if (R != VK_SUCCESS) { LOGE("vkCreateInstance failed (%d)", R); return false; }
        return true;
    }

    bool CreateSurface() {
        VkResult R = Vk::CreatePlatformSurface(Instance, Window, &Surface);
        if (R != VK_SUCCESS) { LOGE("CreatePlatformSurface failed (%d)", R); return false; }
        return true;
    }

    bool PickPhysicalDevice() {
        uint32_t Count = 0;
        vkEnumeratePhysicalDevices(Instance, &Count, nullptr);
        if (Count == 0) { LOGE("No Vulkan physical devices"); return false; }
        std::vector<VkPhysicalDevice> Devices(Count);
        vkEnumeratePhysicalDevices(Instance, &Count, Devices.data());

        for (VkPhysicalDevice Dev : Devices) {
            uint32_t Families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(Dev, &Families, nullptr);
            std::vector<VkQueueFamilyProperties> Props(Families);
            vkGetPhysicalDeviceQueueFamilyProperties(Dev, &Families, Props.data());
            for (uint32_t i = 0; i < Families; ++i) {
                VkBool32 PresentOk = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(Dev, i, Surface, &PresentOk);
                const bool GraphicsOk = (Props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                if (GraphicsOk && PresentOk) {
                    Physical = Dev;
                    QueueFamily = i;
                    return true;
                }
            }
        }
        LOGE("No graphics+present queue family found");
        return false;
    }

    bool CreateDevice() {
        float Priority = 1.0f;
        VkDeviceQueueCreateInfo QueueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        QueueInfo.queueFamilyIndex = QueueFamily;
        QueueInfo.queueCount = 1;
        QueueInfo.pQueuePriorities = &Priority;

        // Always swapchain; add portability_subset when present (required to enable
        // it if the device exposes it — i.e. on MoltenVK).
        uint32_t ExtCount = 0;
        vkEnumerateDeviceExtensionProperties(Physical, nullptr, &ExtCount, nullptr);
        std::vector<VkExtensionProperties> Avail(ExtCount);
        vkEnumerateDeviceExtensionProperties(Physical, nullptr, &ExtCount, Avail.data());
        std::vector<const char*> DevExts;
        DevExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        for (const VkExtensionProperties& E : Avail)
            if (std::strcmp(E.extensionName, "VK_KHR_portability_subset") == 0)
                DevExts.push_back("VK_KHR_portability_subset");

        VkDeviceCreateInfo Info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        Info.queueCreateInfoCount = 1;
        Info.pQueueCreateInfos = &QueueInfo;
        Info.enabledExtensionCount = static_cast<uint32_t>(DevExts.size());
        Info.ppEnabledExtensionNames = DevExts.data();
        VkResult R = vkCreateDevice(Physical, &Info, nullptr, &Device);
        if (R != VK_SUCCESS) { LOGE("vkCreateDevice failed (%d)", R); return false; }
        vkGetDeviceQueue(Device, QueueFamily, 0, &GraphicsQueue);
        vkGetPhysicalDeviceMemoryProperties(Physical, &MemoryProps);
        return true;
    }

    bool CreateCommandResources() {
        VkCommandPoolCreateInfo PoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        PoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        PoolInfo.queueFamilyIndex = QueueFamily;
        VK_CHECK(vkCreateCommandPool(Device, &PoolInfo, nullptr, &CommandPool));

        VkCommandBufferAllocateInfo Alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        Alloc.commandPool = CommandPool;
        Alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        Alloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(Device, &Alloc, &CommandBuffer));
        return CommandPool != VK_NULL_HANDLE && CommandBuffer != VK_NULL_HANDLE;
    }

    bool CreateSyncObjects() {
        VkSemaphoreCreateInfo SemInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo FenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateSemaphore(Device, &SemInfo, nullptr, &ImageAvailable));
        VK_CHECK(vkCreateSemaphore(Device, &SemInfo, nullptr, &RenderFinished));
        VK_CHECK(vkCreateFence(Device, &FenceInfo, nullptr, &InFlight));
        return true;
    }

    // ---- Pipeline ----

    VkShaderModule CreateShaderModule(const uint32_t* Code, size_t SizeBytes) {
        VkShaderModuleCreateInfo Info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        Info.codeSize = SizeBytes;
        Info.pCode = Code;
        VkShaderModule Module = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(Device, &Info, nullptr, &Module));
        return Module;
    }

    bool CreatePipeline() {
        VkPushConstantRange Push{};
        Push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        Push.offset = 0;
        Push.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo LayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        LayoutInfo.setLayoutCount = 1;
        LayoutInfo.pSetLayouts = &DescriptorSetLayout;
        LayoutInfo.pushConstantRangeCount = 1;
        LayoutInfo.pPushConstantRanges = &Push;
        VkResult LR = vkCreatePipelineLayout(Device, &LayoutInfo, nullptr, &PipelineLayout);
        if (LR != VK_SUCCESS) { LOGE("vkCreatePipelineLayout failed (%d)", LR); return false; }

        // Two pipelines share this layout: the sprite/quad pipeline and the MSDF text
        // pipeline (same vertex layout + push-constant prefix, different shaders).
        return CreateGraphicsPipeline(SpriteVertSpv, sizeof(SpriteVertSpv),
                                      SpriteFragSpv, sizeof(SpriteFragSpv), Pipeline)
            && CreateGraphicsPipeline(TextVertSpv, sizeof(TextVertSpv),
                                      TextFragSpv, sizeof(TextFragSpv), TextPipeline)
            && CreateInstancePipeline();
    }

    // The instanced unit pipeline: two vertex bindings (per-vertex quad + per-instance
    // data). Set 0 is the same one-sampler layout the sprite pipeline uses — the glyph
    // atlas (#85) — so a material's descriptor set binds unchanged. Same render pass /
    // blend / dynamic state as the others — only the vertex input and shaders differ.
    bool CreateInstancePipeline() {
        VkPushConstantRange Push{};
        Push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        Push.offset = 0;
        Push.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo LayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        LayoutInfo.setLayoutCount = 1;
        LayoutInfo.pSetLayouts = &DescriptorSetLayout;
        LayoutInfo.pushConstantRangeCount = 1;
        LayoutInfo.pPushConstantRanges = &Push;
        if (vkCreatePipelineLayout(Device, &LayoutInfo, nullptr, &InstanceLayout) != VK_SUCCESS) {
            LOGE("instance pipeline layout failed");
            return false;
        }

        VkShaderModule Vert = CreateShaderModule(InstancedVertSpv, sizeof(InstancedVertSpv));
        VkShaderModule Frag = CreateShaderModule(InstancedFragSpv, sizeof(InstancedFragSpv));

        VkPipelineShaderStageCreateInfo Stages[2]{};
        Stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        Stages[0].module = Vert;
        Stages[0].pName = "main";
        Stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        Stages[1].module = Frag;
        Stages[1].pName = "main";

        VkVertexInputBindingDescription Bindings[2]{};
        Bindings[0] = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        Bindings[1] = {1, sizeof(InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE};

        VkVertexInputAttributeDescription Attrs[7]{};
        Attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, Position)};       // quad corner
        Attrs[1] = {4, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, PrevX)};    // prev centre
        Attrs[2] = {5, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, CurX)};     // cur centre
        Attrs[3] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, R)};        // colour
        Attrs[4] = {7, 1, VK_FORMAT_R32_SFLOAT,          offsetof(InstanceData, Size)};     // pixel size
        Attrs[5] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, U0)};       // atlas UV rect
        Attrs[6] = {9, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, FaceX)};    // facing dir (0,0=upright)

        VkPipelineVertexInputStateCreateInfo VertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VertexInput.vertexBindingDescriptionCount = 2;
        VertexInput.pVertexBindingDescriptions = Bindings;
        VertexInput.vertexAttributeDescriptionCount = 7;
        VertexInput.pVertexAttributeDescriptions = Attrs;

        VkPipelineInputAssemblyStateCreateInfo InputAsm{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        InputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo Viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        Viewport.viewportCount = 1;
        Viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo Raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        Raster.polygonMode = VK_POLYGON_MODE_FILL;
        Raster.cullMode = VK_CULL_MODE_NONE;
        Raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        Raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo Multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState Blend{};
        Blend.blendEnable = VK_TRUE;
        Blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        Blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        Blend.colorBlendOp = VK_BLEND_OP_ADD;
        Blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        Blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        Blend.alphaBlendOp = VK_BLEND_OP_ADD;
        Blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo ColorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        ColorBlend.attachmentCount = 1;
        ColorBlend.pAttachments = &Blend;

        VkDynamicState Dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo Dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        Dynamic.dynamicStateCount = 2;
        Dynamic.pDynamicStates = Dynamics;

        VkGraphicsPipelineCreateInfo Info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        Info.stageCount = 2;
        Info.pStages = Stages;
        Info.pVertexInputState = &VertexInput;
        Info.pInputAssemblyState = &InputAsm;
        Info.pViewportState = &Viewport;
        Info.pRasterizationState = &Raster;
        Info.pMultisampleState = &Multisample;
        Info.pColorBlendState = &ColorBlend;
        Info.pDynamicState = &Dynamic;
        Info.layout = InstanceLayout;
        Info.renderPass = RenderPass;
        Info.subpass = 0;
        VkResult R = vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &Info, nullptr, &InstancePipeline);

        vkDestroyShaderModule(Device, Vert, nullptr);
        vkDestroyShaderModule(Device, Frag, nullptr);
        if (R != VK_SUCCESS) { LOGE("instance pipeline create failed (%d)", R); return false; }
        return true;
    }

    bool CreateInstanceBuffer() {
        const VkDeviceSize Size = static_cast<VkDeviceSize>(MaxInstances) * sizeof(InstanceData);
        return CreateMappedBuffer(Size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, InstanceVB, InstanceVBMem,
                                  InstanceMapped);
    }

    bool CreateGraphicsPipeline(const uint32_t* VertSpv, size_t VertSize,
                                const uint32_t* FragSpv, size_t FragSize,
                                VkPipeline& Out) {
        VkShaderModule Vert = CreateShaderModule(VertSpv, VertSize);
        VkShaderModule Frag = CreateShaderModule(FragSpv, FragSize);

        VkPipelineShaderStageCreateInfo Stages[2]{};
        Stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        Stages[0].module = Vert;
        Stages[0].pName = "main";
        Stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        Stages[1].module = Frag;
        Stages[1].pName = "main";

        VkVertexInputBindingDescription Binding{};
        Binding.binding = 0;
        Binding.stride = sizeof(Vertex);
        Binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription Attrs[4]{};
        Attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, Position)};
        Attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, Normal)};
        Attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, Uv)};
        Attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, Color)};

        VkPipelineVertexInputStateCreateInfo VertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VertexInput.vertexBindingDescriptionCount = 1;
        VertexInput.pVertexBindingDescriptions = &Binding;
        VertexInput.vertexAttributeDescriptionCount = 4;
        VertexInput.pVertexAttributeDescriptions = Attrs;

        VkPipelineInputAssemblyStateCreateInfo InputAsm{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        InputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo Viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        Viewport.viewportCount = 1;
        Viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo Raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        Raster.polygonMode = VK_POLYGON_MODE_FILL;
        Raster.cullMode = VK_CULL_MODE_NONE;
        Raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        Raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo Multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState Blend{};
        Blend.blendEnable = VK_TRUE;
        Blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        Blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        Blend.colorBlendOp = VK_BLEND_OP_ADD;
        Blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        Blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        Blend.alphaBlendOp = VK_BLEND_OP_ADD;
        Blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo ColorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        ColorBlend.attachmentCount = 1;
        ColorBlend.pAttachments = &Blend;

        VkDynamicState Dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo Dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        Dynamic.dynamicStateCount = 2;
        Dynamic.pDynamicStates = Dynamics;

        VkGraphicsPipelineCreateInfo Info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        Info.stageCount = 2;
        Info.pStages = Stages;
        Info.pVertexInputState = &VertexInput;
        Info.pInputAssemblyState = &InputAsm;
        Info.pViewportState = &Viewport;
        Info.pRasterizationState = &Raster;
        Info.pMultisampleState = &Multisample;
        Info.pColorBlendState = &ColorBlend;
        Info.pDynamicState = &Dynamic;
        Info.layout = PipelineLayout;
        Info.renderPass = RenderPass;
        Info.subpass = 0;
        VkResult R = vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &Info, nullptr, &Out);

        vkDestroyShaderModule(Device, Vert, nullptr);
        vkDestroyShaderModule(Device, Frag, nullptr);
        if (R != VK_SUCCESS) { LOGE("vkCreateGraphicsPipelines failed (%d)", R); return false; }
        return true;
    }

    // Per-frame dynamic arena for text glyph quads (host-visible, persistently mapped).
    // Single-buffered is safe: BeginFrame waits on the in-flight fence before the arena
    // is reset/reused, so the GPU has finished reading last frame's glyphs.
    bool CreateTextBuffers() {
        const VkDeviceSize VbSize = static_cast<VkDeviceSize>(MaxTextVertices) * sizeof(Vertex);
        const VkDeviceSize IbSize = static_cast<VkDeviceSize>(MaxTextIndices) * sizeof(uint32_t);
        if (!CreateMappedBuffer(VbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, TextVB, TextVBMem, TextVBMapped))
            return false;
        if (!CreateMappedBuffer(IbSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, TextIB, TextIBMem, TextIBMapped))
            return false;
        return true;
    }

    bool CreateMappedBuffer(VkDeviceSize Size, VkBufferUsageFlags Usage,
                            VkBuffer& OutBuffer, VkDeviceMemory& OutMemory, void*& OutMapped) {
        VkBufferCreateInfo BufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        BufInfo.size = Size;
        BufInfo.usage = Usage;
        BufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(Device, &BufInfo, nullptr, &OutBuffer) != VK_SUCCESS) {
            LOGE("vkCreateBuffer (dynamic) failed (size=%llu)", (unsigned long long)Size);
            return false;
        }
        VkMemoryRequirements Req{};
        vkGetBufferMemoryRequirements(Device, OutBuffer, &Req);
        VkMemoryAllocateInfo Alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        Alloc.allocationSize = Req.size;
        Alloc.memoryTypeIndex = FindMemoryType(Req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(Device, &Alloc, nullptr, &OutMemory) != VK_SUCCESS) {
            LOGE("vkAllocateMemory (dynamic) failed"); return false;
        }
        vkBindBufferMemory(Device, OutBuffer, OutMemory, 0);
        vkMapMemory(Device, OutMemory, 0, Size, 0, &OutMapped);  // persistently mapped
        return OutMapped != nullptr;
    }

    // ---- Buffers ----

    uint32_t FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Want) {
        for (uint32_t i = 0; i < MemoryProps.memoryTypeCount; ++i) {
            const bool Allowed = (TypeBits & (1u << i)) != 0;
            const bool HasProps = (MemoryProps.memoryTypes[i].propertyFlags & Want) == Want;
            if (Allowed && HasProps) return i;
        }
        LOGE("No suitable memory type (bits=%u)", TypeBits);
        return 0;
    }

    bool CreateBuffer(VkDeviceSize Size, VkBufferUsageFlags Usage, const void* Data,
                      VkBuffer& OutBuffer, VkDeviceMemory& OutMemory) {
        VkBufferCreateInfo BufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        BufInfo.size = Size;
        BufInfo.usage = Usage;
        BufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(Device, &BufInfo, nullptr, &OutBuffer) != VK_SUCCESS) {
            LOGE("vkCreateBuffer failed (size=%llu)", (unsigned long long)Size);
            return false;
        }

        VkMemoryRequirements Req{};
        vkGetBufferMemoryRequirements(Device, OutBuffer, &Req);
        VkMemoryAllocateInfo Alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        Alloc.allocationSize = Req.size;
        Alloc.memoryTypeIndex = FindMemoryType(Req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(Device, &Alloc, nullptr, &OutMemory) != VK_SUCCESS) {
            LOGE("vkAllocateMemory failed (size=%llu)", (unsigned long long)Req.size);
            return false;
        }
        vkBindBufferMemory(Device, OutBuffer, OutMemory, 0);

        void* Mapped = nullptr;
        vkMapMemory(Device, OutMemory, 0, Size, 0, &Mapped);
        std::memcpy(Mapped, Data, static_cast<size_t>(Size));
        vkUnmapMemory(Device, OutMemory);
        return true;
    }

    void DestroyMesh(Mesh& M) {
        if (Device == VK_NULL_HANDLE) return;
        if (M.VertexBuffer) vkDestroyBuffer(Device, M.VertexBuffer, nullptr);
        if (M.VertexMemory) vkFreeMemory(Device, M.VertexMemory, nullptr);
        if (M.IndexBuffer)  vkDestroyBuffer(Device, M.IndexBuffer, nullptr);
        if (M.IndexMemory)  vkFreeMemory(Device, M.IndexMemory, nullptr);
        M = Mesh{};
    }

    // ---- Textures / descriptors ----

    bool CreateDescriptorResources() {
        VkSamplerCreateInfo SamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        SamplerInfo.magFilter = VK_FILTER_LINEAR;
        SamplerInfo.minFilter = VK_FILTER_LINEAR;
        SamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        SamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        SamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(Device, &SamplerInfo, nullptr, &Sampler));

        VkDescriptorSetLayoutBinding Binding{};
        Binding.binding = 0;
        Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        Binding.descriptorCount = 1;
        Binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo LayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        LayoutInfo.bindingCount = 1;
        LayoutInfo.pBindings = &Binding;
        VK_CHECK(vkCreateDescriptorSetLayout(Device, &LayoutInfo, nullptr, &DescriptorSetLayout));

        VkDescriptorPoolSize PoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxMaterials};
        VkDescriptorPoolCreateInfo PoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        PoolInfo.maxSets = MaxMaterials;
        PoolInfo.poolSizeCount = 1;
        PoolInfo.pPoolSizes = &PoolSize;
        VK_CHECK(vkCreateDescriptorPool(Device, &PoolInfo, nullptr, &DescriptorPool));
        return Sampler != VK_NULL_HANDLE && DescriptorSetLayout != VK_NULL_HANDLE &&
               DescriptorPool != VK_NULL_HANDLE;
    }

    VkDescriptorSet AllocateDescriptorSet(VkImageView View) {
        VkDescriptorSetAllocateInfo Alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        Alloc.descriptorPool = DescriptorPool;
        Alloc.descriptorSetCount = 1;
        Alloc.pSetLayouts = &DescriptorSetLayout;
        VkDescriptorSet Set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(Device, &Alloc, &Set));

        VkDescriptorImageInfo ImageInfo{};
        ImageInfo.sampler = Sampler;
        ImageInfo.imageView = View;
        ImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet Write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        Write.dstSet = Set;
        Write.dstBinding = 0;
        Write.descriptorCount = 1;
        Write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        Write.pImageInfo = &ImageInfo;
        vkUpdateDescriptorSets(Device, 1, &Write, 0, nullptr);
        return Set;
    }

    VkCommandBuffer BeginOneTimeCommands() {
        VkCommandBufferAllocateInfo Alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        Alloc.commandPool = CommandPool;
        Alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        Alloc.commandBufferCount = 1;
        VkCommandBuffer Cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(Device, &Alloc, &Cmd);
        VkCommandBufferBeginInfo Begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(Cmd, &Begin);
        return Cmd;
    }

    void EndOneTimeCommands(VkCommandBuffer Cmd) {
        vkEndCommandBuffer(Cmd);
        VkSubmitInfo Submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        Submit.commandBufferCount = 1;
        Submit.pCommandBuffers = &Cmd;
        vkQueueSubmit(GraphicsQueue, 1, &Submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(GraphicsQueue);
        vkFreeCommandBuffers(Device, CommandPool, 1, &Cmd);
    }

    void TransitionLayout(VkCommandBuffer Cmd, VkImage Image,
                          VkImageLayout Old, VkImageLayout New) {
        VkImageMemoryBarrier Barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        Barrier.oldLayout = Old;
        Barrier.newLayout = New;
        Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        Barrier.image = Image;
        Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Barrier.subresourceRange.levelCount = 1;
        Barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags SrcStage, DstStage;
        if (Old == VK_IMAGE_LAYOUT_UNDEFINED &&
            New == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            Barrier.srcAccessMask = 0;
            Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            SrcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            DstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else {
            Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            SrcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            DstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        vkCmdPipelineBarrier(Cmd, SrcStage, DstStage, 0, 0, nullptr, 0, nullptr, 1, &Barrier);
    }

    void DestroyTexture(Texture& T) {
        if (Device == VK_NULL_HANDLE) return;
        if (T.View)   vkDestroyImageView(Device, T.View, nullptr);
        if (T.Image)  vkDestroyImage(Device, T.Image, nullptr);
        if (T.Memory) vkFreeMemory(Device, T.Memory, nullptr);
        T = Texture{};
    }

    // ---- Swapchain ----

    bool CreateSwapchain() {
        VkSurfaceCapabilitiesKHR Caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Physical, Surface, &Caps);

        Extent = Caps.currentExtent;
        if (Extent.width == 0xFFFFFFFFu) {  // surface lets us choose: ask the platform
            Vk::PlatformDrawableSize(Window, &Extent.width, &Extent.height);
        }
        if (Extent.width == 0 || Extent.height == 0) {
            // Was a SILENT false (issue #73): a relaunch race can report 0x0 here, and
            // the resulting null swapchain used to be terminal. BeginFrame now retries.
            LOGE("CreateSwapchain: surface reports 0-extent (%ux%u)", Extent.width, Extent.height);
            return false;
        }

        uint32_t ImageCount = Caps.minImageCount + 1;
        if (Caps.maxImageCount > 0 && ImageCount > Caps.maxImageCount)
            ImageCount = Caps.maxImageCount;

        SurfaceFormat = ChooseSurfaceFormat();

        VkSwapchainCreateInfoKHR Info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        Info.surface = Surface;
        Info.minImageCount = ImageCount;
        Info.imageFormat = SurfaceFormat.format;
        Info.imageColorSpace = SurfaceFormat.colorSpace;
        Info.imageExtent = Extent;
        Info.imageArrayLayers = 1;
        Info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        Info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Info.preTransform = Caps.currentTransform;
        Info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        Info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        Info.clipped = VK_TRUE;
        VkResult R = vkCreateSwapchainKHR(Device, &Info, nullptr, &Swapchain);
        if (R != VK_SUCCESS) { LOGE("vkCreateSwapchainKHR failed (%d)", R); return false; }

        uint32_t Got = 0;
        vkGetSwapchainImagesKHR(Device, Swapchain, &Got, nullptr);
        SwapImages.resize(Got);
        vkGetSwapchainImagesKHR(Device, Swapchain, &Got, SwapImages.data());

        return CreateImageViews() && CreateRenderPass() && CreateFramebuffers();
    }

    VkSurfaceFormatKHR ChooseSurfaceFormat() {
        uint32_t Count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(Physical, Surface, &Count, nullptr);
        std::vector<VkSurfaceFormatKHR> Formats(Count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(Physical, Surface, &Count, Formats.data());
        for (const VkSurfaceFormatKHR& F : Formats) {
            if (F.format == VK_FORMAT_B8G8R8A8_UNORM &&
                F.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return F;
        }
        return Formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                               : Formats[0];
    }

    bool CreateImageViews() {
        SwapViews.resize(SwapImages.size());
        for (size_t i = 0; i < SwapImages.size(); ++i) {
            VkImageViewCreateInfo Info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            Info.image = SwapImages[i];
            Info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            Info.format = SurfaceFormat.format;
            Info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            Info.subresourceRange.levelCount = 1;
            Info.subresourceRange.layerCount = 1;
            VK_CHECK(vkCreateImageView(Device, &Info, nullptr, &SwapViews[i]));
        }
        return true;
    }

    bool CreateRenderPass() {
        VkAttachmentDescription ColorAtt{};
        ColorAtt.format = SurfaceFormat.format;
        ColorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        ColorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ColorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ColorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ColorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ColorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ColorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription Subpass{};
        Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        Subpass.colorAttachmentCount = 1;
        Subpass.pColorAttachments = &ColorRef;

        VkSubpassDependency Dep{};
        Dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        Dep.dstSubpass = 0;
        Dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        Dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        Dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo Info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        Info.attachmentCount = 1;
        Info.pAttachments = &ColorAtt;
        Info.subpassCount = 1;
        Info.pSubpasses = &Subpass;
        Info.dependencyCount = 1;
        Info.pDependencies = &Dep;
        VkResult R = vkCreateRenderPass(Device, &Info, nullptr, &RenderPass);
        if (R != VK_SUCCESS) { LOGE("vkCreateRenderPass failed (%d)", R); return false; }
        return true;
    }

    bool CreateFramebuffers() {
        Framebuffers.resize(SwapViews.size());
        for (size_t i = 0; i < SwapViews.size(); ++i) {
            VkFramebufferCreateInfo Info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            Info.renderPass = RenderPass;
            Info.attachmentCount = 1;
            Info.pAttachments = &SwapViews[i];
            Info.width = Extent.width;
            Info.height = Extent.height;
            Info.layers = 1;
            VK_CHECK(vkCreateFramebuffer(Device, &Info, nullptr, &Framebuffers[i]));
        }
        return true;
    }

    void DestroySwapchain() {
        if (Device == VK_NULL_HANDLE) return;
        for (VkFramebuffer Fb : Framebuffers) if (Fb) vkDestroyFramebuffer(Device, Fb, nullptr);
        Framebuffers.clear();
        if (RenderPass != VK_NULL_HANDLE) { vkDestroyRenderPass(Device, RenderPass, nullptr); RenderPass = VK_NULL_HANDLE; }
        for (VkImageView V : SwapViews) if (V) vkDestroyImageView(Device, V, nullptr);
        SwapViews.clear();
        if (Swapchain != VK_NULL_HANDLE) { vkDestroySwapchainKHR(Device, Swapchain, nullptr); Swapchain = VK_NULL_HANDLE; }
        SwapImages.clear();
    }

    void RecreateSwapchain() {
        vkDeviceWaitIdle(Device);
        DestroySwapchain();
        FrameAcquired = false;  // any pre-acquired image index is invalid after recreate
        ++RecreateCount;

        // Escalation (issue #73): a long dead streak means recreating against the
        // CURRENT surface isn't healing — the VkSurfaceKHR itself is presumed stale
        // (bound to a window-server surface that died in a relaunch race). Rebuild it
        // from the stored native window/layer, then build the swapchain against the
        // fresh one. Same platform seam as Init, so this is OS-agnostic.
        if (DeadFrames >= SurfaceEscalateAfter || Surface == VK_NULL_HANDLE) {
            if (DeadFrames % 30 == 0)
                LOGE("swapchain dead for %u frames - rebuilding VkSurfaceKHR (recreate #%u)",
                     DeadFrames, RecreateCount);
            if (Surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(Instance, Surface, nullptr);
            Surface = VK_NULL_HANDLE;
            if (!CreateSurface()) return;  // logged inside; retried next frame (Surface
                                           // stays null, so this branch runs again)
            DeadFrames = 0;                // fresh surface: restart the escalation clock
        }

        const bool Ok = CreateSwapchain();
        // Loud on every path (issue #73): the old code recreated silently, so a failed
        // recreate (-> null swapchain) was indistinguishable from health. Rate-limited
        // on the dead streak so a retry loop reports ~2 lines/s, not 60.
        if (DeadFrames == 0 || DeadFrames % 30 == 0) {
            if (Ok)
                LOGI("swapchain recreated #%u: %ux%u", RecreateCount, Extent.width, Extent.height);
            else
                LOGE("swapchain recreate #%u FAILED - will retry next frame", RecreateCount);
        }
    }

    // ---- State ----
    void*            Window = nullptr;  // ANativeWindow* (Android) / CAMetalLayer* (iOS)
    VkInstance       Instance = VK_NULL_HANDLE;
    VkSurfaceKHR     Surface = VK_NULL_HANDLE;
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice         Device = VK_NULL_HANDLE;
    uint32_t         QueueFamily = 0;
    VkQueue          GraphicsQueue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties MemoryProps{};

    VkSwapchainKHR             Swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR         SurfaceFormat{};
    VkExtent2D                 Extent{};
    std::vector<VkImage>       SwapImages;
    std::vector<VkImageView>   SwapViews;
    VkRenderPass               RenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> Framebuffers;

    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline       Pipeline = VK_NULL_HANDLE;       // sprite / quad
    VkPipeline       TextPipeline = VK_NULL_HANDLE;   // MSDF text
    VkPipelineLayout InstanceLayout = VK_NULL_HANDLE; // push-const only (no sampler)
    VkPipeline       InstancePipeline = VK_NULL_HANDLE;  // instanced units
    VkPipeline       BoundPipeline = VK_NULL_HANDLE;  // currently bound (lazy rebind)

    // Per-frame dynamic text arena (fixed capacity — ~1024 glyphs/frame).
    static constexpr uint32_t MaxTextVertices = 4096;
    static constexpr uint32_t MaxTextIndices  = 6144;
    VkBuffer       TextVB = VK_NULL_HANDLE, TextIB = VK_NULL_HANDLE;
    VkDeviceMemory TextVBMem = VK_NULL_HANDLE, TextIBMem = VK_NULL_HANDLE;
    void*          TextVBMapped = nullptr;

    // Per-frame instance arena (host-visible, persistently mapped) — same single-buffer
    // safety as the text arena: BeginFrame waits on the in-flight fence before reset.
    static constexpr uint32_t MaxInstances = 8192;
    VkBuffer       InstanceVB = VK_NULL_HANDLE;
    VkDeviceMemory InstanceVBMem = VK_NULL_HANDLE;
    void*          InstanceMapped = nullptr;
    uint32_t       InstanceCursor = 0;
    void*          TextIBMapped = nullptr;
    uint32_t       TextVBCursor = 0, TextIBCursor = 0;

    static constexpr uint32_t MaxMaterials = 96;   // RPS HUD (#85): grid LUT + panel/plate/icon materials
    VkSampler             Sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      DescriptorPool = VK_NULL_HANDLE;
    std::vector<Texture>  Textures;
    TextureHandle         DefaultTexture = 0;

    VkCommandPool   CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkSemaphore     ImageAvailable = VK_NULL_HANDLE;
    VkSemaphore     RenderFinished = VK_NULL_HANDLE;
    VkFence         InFlight = VK_NULL_HANDLE;
    bool            FrameAcquired = false;  // WaitForFrame acquired an image this frame (consumed by BeginFrame)

    std::vector<Mesh>     Meshes;
    std::vector<Material> Materials;

    Camera   CurrentCamera{};
    uint32_t ImageIndex = 0;
    bool     Ready = false;
    bool     Recording = false;
    bool     NeedsRecreate = false;

    // Present-health bookkeeping (issue #73). Every swapchain failure used to be
    // silent AND terminal (a failed recreate left Swapchain null with no retry path)
    // — the app ran on, black, with an empty log. These counters make the state loud
    // and drive the self-heal in BeginFrame.
    uint32_t FramesPresented = 0;   // vkQueuePresentKHR successes since Init
    uint32_t DeadFrames = 0;        // consecutive BeginFrames with no usable swapchain
    uint32_t RecreateCount = 0;     // swapchain recreates since Init
    static constexpr uint32_t SurfaceEscalateAfter = 60;  // dead frames before surface rebuild
};

} // namespace

IRenderer* VulkanRenderer::Create() { return new VulkanRendererImpl(); }

} // namespace Lur::Render
