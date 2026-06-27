// Android Vulkan backend.
//   1a: swapchain bring-up + per-frame clear (render pass loadOp).
//   1b: one unlit textured-quad pipeline + vertex/index buffers + DrawMesh, so
//       the board draws as tinted quads. Tint + MVP travel as push constants —
//       no descriptor sets yet; textures (descriptor sets/samplers) are 1c.
//
// Targets the Vulkan portability subset (triangle-list, standard formats, no
// geometry/tessellation, dynamic viewport) so the same code runs through
// MoltenVK on iOS later.
#include <android/log.h>
#include <android/native_window.h>

// Must precede <vulkan/vulkan.h> to expose VkAndroidSurfaceCreateInfoKHR and
// vkCreateAndroidSurfaceKHR (the platform-specific surface entry points).
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "Lur/Render/Vulkan/VulkanRenderer.h"

// SPIR-V compiled from Shaders/*.glsl at build time (glslc -mfmt=num), emitted
// as a uint32 init list we include directly — no runtime asset loading.
static const uint32_t SpriteVertSpv[] = {
#include "Sprite.vert.inc"
};
static const uint32_t SpriteFragSpv[] = {
#include "Sprite.frag.inc"
};

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OnlyChess", __VA_ARGS__)

// Log + carry on. The slow build->install->look loop makes a loud failure marker
// in logcat more useful than aborting; a half-initialised renderer just clears
// to the background colour, itself a visible signal that something failed.
#define VK_CHECK(Expr)                                                              \
    do {                                                                            \
        VkResult Result_ = (Expr);                                                  \
        if (Result_ != VK_SUCCESS)                                                  \
            LOGE("Vulkan call failed (%d) at %s:%d", Result_, __FILE__, __LINE__);  \
    } while (0)

namespace Lur::Render {
namespace {

// Pushed per draw. mat4 (64) + vec4 (16) = 80 bytes, well under the 128-byte
// guaranteed push-constant range.
struct PushConstants {
    float Mvp[16];
    float Tint[4];
};

struct Mesh {
    VkBuffer       VertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory VertexMemory = VK_NULL_HANDLE;
    VkBuffer       IndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory IndexMemory = VK_NULL_HANDLE;
    uint32_t       IndexCount = 0;
};

struct Material {
    Color Tint;
};

class VulkanRendererAndroid : public IRenderer {
public:
    bool Init(void* NativeWindow) override {
        Window = static_cast<ANativeWindow*>(NativeWindow);
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
        if (!CreatePipeline())         return false;
        LOGI("Vulkan renderer up: %ux%u, %u swapchain images",
             Extent.width, Extent.height, static_cast<uint32_t>(SwapImages.size()));
        Ready = true;
        return true;
    }

    void Resize(int /*WidthPx*/, int /*HeightPx*/) override {
        // The swapchain owns the real extent (from surface caps); a resize just
        // means the current one is stale. Recreate lazily on the next frame.
        NeedsRecreate = true;
    }

    void Shutdown() override {
        if (Device != VK_NULL_HANDLE) vkDeviceWaitIdle(Device);
        for (Mesh& M : Meshes) DestroyMesh(M);
        Meshes.clear();
        Materials.clear();
        if (Device != VK_NULL_HANDLE) {
            if (Pipeline != VK_NULL_HANDLE)       vkDestroyPipeline(Device, Pipeline, nullptr);
            if (PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
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
        *this = VulkanRendererAndroid{};  // reset all handles to null
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
        return static_cast<MeshHandle>(Meshes.size());  // handle = index + 1
    }

    TextureHandle LoadTexture(const uint8_t*, int, int) override { return 0; }  // 1c

    MaterialHandle CreateMaterial(const MaterialDesc& Desc) override {
        Materials.push_back(Material{Desc.Tint});
        return static_cast<MaterialHandle>(Materials.size());  // handle = index + 1
    }

    void BeginFrame(const Camera& Cam) override {
        CurrentCamera = Cam;
        Recording = false;
        if (!Ready) return;

        if (NeedsRecreate) { RecreateSwapchain(); NeedsRecreate = false; }
        if (Swapchain == VK_NULL_HANDLE) return;  // window not presentable (e.g. minimised)

        vkWaitForFences(Device, 1, &InFlight, VK_TRUE, UINT64_MAX);

        VkResult Acq = vkAcquireNextImageKHR(Device, Swapchain, UINT64_MAX,
                                             ImageAvailable, VK_NULL_HANDLE, &ImageIndex);
        if (Acq == VK_ERROR_OUT_OF_DATE_KHR) { NeedsRecreate = true; return; }
        if (Acq != VK_SUCCESS && Acq != VK_SUBOPTIMAL_KHR) {
            LOGE("vkAcquireNextImageKHR failed (%d)", Acq);
            return;
        }

        vkResetFences(Device, 1, &InFlight);
        vkResetCommandBuffer(CommandBuffer, 0);

        VkCommandBufferBeginInfo BeginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(CommandBuffer, &BeginInfo));

        // A muted slate blue-grey behind the board.
        VkClearValue Clear{};
        Clear.color = {{0.16f, 0.20f, 0.26f, 1.0f}};

        VkRenderPassBeginInfo Pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        Pass.renderPass = RenderPass;
        Pass.framebuffer = Framebuffers[ImageIndex];
        Pass.renderArea.extent = Extent;
        Pass.clearValueCount = 1;
        Pass.pClearValues = &Clear;
        vkCmdBeginRenderPass(CommandBuffer, &Pass, VK_SUBPASS_CONTENTS_INLINE);

        // Pipeline + viewport/scissor are dynamic, so a swapchain resize never
        // rebuilds the pipeline — we just set the new extent each frame here.
        vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
        VkViewport Vp{0.0f, 0.0f, static_cast<float>(Extent.width),
                      static_cast<float>(Extent.height), 0.0f, 1.0f};
        VkRect2D Sc{{0, 0}, Extent};
        vkCmdSetViewport(CommandBuffer, 0, 1, &Vp);
        vkCmdSetScissor(CommandBuffer, 0, 1, &Sc);
        Recording = true;
    }

    void DrawMesh(MeshHandle MeshId, MaterialHandle MaterialId,
                  const Math::Mat4& Model) override {
        if (!Recording) return;
        if (MeshId == 0 || MeshId > Meshes.size()) return;
        const Mesh& M = Meshes[MeshId - 1];

        PushConstants Pc;
        const Math::Mat4 Mvp = CurrentCamera.Projection * CurrentCamera.View * Model;
        std::memcpy(Pc.Mvp, Mvp.M, sizeof(Pc.Mvp));
        const Color T = (MaterialId != 0 && MaterialId <= Materials.size())
                            ? Materials[MaterialId - 1].Tint : Color{};
        Pc.Tint[0] = T.R; Pc.Tint[1] = T.G; Pc.Tint[2] = T.B; Pc.Tint[3] = T.A;
        vkCmdPushConstants(CommandBuffer, PipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(Pc), &Pc);

        VkDeviceSize Offset = 0;
        vkCmdBindVertexBuffers(CommandBuffer, 0, 1, &M.VertexBuffer, &Offset);
        vkCmdBindIndexBuffer(CommandBuffer, M.IndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(CommandBuffer, M.IndexCount, 1, 0, 0, 0);
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
        if (Pres == VK_ERROR_OUT_OF_DATE_KHR || Pres == VK_SUBOPTIMAL_KHR) NeedsRecreate = true;
        else if (Pres != VK_SUCCESS) LOGE("vkQueuePresentKHR failed (%d)", Pres);
    }

private:
    // ---- Init steps ----

    bool CreateInstance() {
        VkApplicationInfo App{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        App.pApplicationName = "OnlyChess";
        App.apiVersion = VK_API_VERSION_1_0;  // minSdk 26 guarantees Vulkan 1.0

        const char* Extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        VkInstanceCreateInfo Info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        Info.pApplicationInfo = &App;
        Info.enabledExtensionCount = 2;
        Info.ppEnabledExtensionNames = Extensions;
        VkResult R = vkCreateInstance(&Info, nullptr, &Instance);
        if (R != VK_SUCCESS) { LOGE("vkCreateInstance failed (%d)", R); return false; }
        return true;
    }

    bool CreateSurface() {
        VkAndroidSurfaceCreateInfoKHR Info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        Info.window = Window;
        VkResult R = vkCreateAndroidSurfaceKHR(Instance, &Info, nullptr, &Surface);
        if (R != VK_SUCCESS) { LOGE("vkCreateAndroidSurfaceKHR failed (%d)", R); return false; }
        return true;
    }

    bool PickPhysicalDevice() {
        uint32_t Count = 0;
        vkEnumeratePhysicalDevices(Instance, &Count, nullptr);
        if (Count == 0) { LOGE("No Vulkan physical devices"); return false; }
        std::vector<VkPhysicalDevice> Devices(Count);
        vkEnumeratePhysicalDevices(Instance, &Count, Devices.data());

        // Phones have a single GPU; take the first that has a queue family which
        // both renders and presents to our surface (universal on mobile).
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

        const char* DeviceExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo Info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        Info.queueCreateInfoCount = 1;
        Info.pQueueCreateInfos = &QueueInfo;
        Info.enabledExtensionCount = 1;
        Info.ppEnabledExtensionNames = DeviceExt;
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
        FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first BeginFrame won't block
        VK_CHECK(vkCreateSemaphore(Device, &SemInfo, nullptr, &ImageAvailable));
        VK_CHECK(vkCreateSemaphore(Device, &SemInfo, nullptr, &RenderFinished));
        VK_CHECK(vkCreateFence(Device, &FenceInfo, nullptr, &InFlight));
        return true;
    }

    // ---- Pipeline (built once; viewport/scissor are dynamic) ----

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
        LayoutInfo.pushConstantRangeCount = 1;
        LayoutInfo.pPushConstantRanges = &Push;
        VkResult LR = vkCreatePipelineLayout(Device, &LayoutInfo, nullptr, &PipelineLayout);
        if (LR != VK_SUCCESS) { LOGE("vkCreatePipelineLayout failed (%d)", LR); return false; }

        VkShaderModule Vert = CreateShaderModule(SpriteVertSpv, sizeof(SpriteVertSpv));
        VkShaderModule Frag = CreateShaderModule(SpriteFragSpv, sizeof(SpriteFragSpv));

        VkPipelineShaderStageCreateInfo Stages[2]{};
        Stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        Stages[0].module = Vert;
        Stages[0].pName = "main";
        Stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        Stages[1].module = Frag;
        Stages[1].pName = "main";

        // Vertex layout mirrors Lur::Render::Vertex (48 bytes, all floats).
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
        Viewport.scissorCount = 1;  // both dynamic (set each frame)

        VkPipelineRasterizationStateCreateInfo Raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        Raster.polygonMode = VK_POLYGON_MODE_FILL;
        Raster.cullMode = VK_CULL_MODE_NONE;  // 2D quads: winding-agnostic
        Raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        Raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo Multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Straight alpha blending, so the textured pieces (1c) composite over the
        // board. Opaque board quads (alpha 1) are unaffected.
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
        VkResult R = vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &Info, nullptr, &Pipeline);

        // Modules can be destroyed once the pipeline is built.
        vkDestroyShaderModule(Device, Vert, nullptr);
        vkDestroyShaderModule(Device, Frag, nullptr);
        if (R != VK_SUCCESS) { LOGE("vkCreateGraphicsPipelines failed (%d)", R); return false; }
        return true;
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

    // Host-visible coherent buffer, uploaded by map+memcpy. Fine for the small,
    // static board geometry; device-local + staging is a later optimisation.
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

    // ---- Swapchain (recreated on resize / out-of-date) ----

    bool CreateSwapchain() {
        VkSurfaceCapabilitiesKHR Caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Physical, Surface, &Caps);

        Extent = Caps.currentExtent;
        if (Extent.width == 0xFFFFFFFFu) {  // surface lets us choose; use the window size
            Extent.width  = static_cast<uint32_t>(ANativeWindow_getWidth(Window));
            Extent.height = static_cast<uint32_t>(ANativeWindow_getHeight(Window));
        }
        if (Extent.width == 0 || Extent.height == 0) return false;  // minimised

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
        Info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // one queue family
        Info.preTransform = Caps.currentTransform;
        Info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        Info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; vsync'd
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
        VkAttachmentDescription Color{};
        Color.format = SurfaceFormat.format;
        Color.samples = VK_SAMPLE_COUNT_1_BIT;
        Color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // this is what clears the screen
        Color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        Color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        Color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        Color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription Subpass{};
        Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        Subpass.colorAttachmentCount = 1;
        Subpass.pColorAttachments = &ColorRef;

        // Wait for the acquired image before writing it.
        VkSubpassDependency Dep{};
        Dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        Dep.dstSubpass = 0;
        Dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        Dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        Dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo Info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        Info.attachmentCount = 1;
        Info.pAttachments = &Color;
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
        CreateSwapchain();  // render pass is identical, so the pipeline still matches
    }

    // ---- State ----
    ANativeWindow*   Window = nullptr;
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
    VkPipeline       Pipeline = VK_NULL_HANDLE;

    VkCommandPool   CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkSemaphore     ImageAvailable = VK_NULL_HANDLE;
    VkSemaphore     RenderFinished = VK_NULL_HANDLE;
    VkFence         InFlight = VK_NULL_HANDLE;

    std::vector<Mesh>     Meshes;
    std::vector<Material> Materials;

    Camera   CurrentCamera{};
    uint32_t ImageIndex = 0;
    bool     Ready = false;
    bool     Recording = false;
    bool     NeedsRecreate = false;
};

} // namespace

IRenderer* VulkanRenderer::Create() { return new VulkanRendererAndroid(); }

} // namespace Lur::Render
