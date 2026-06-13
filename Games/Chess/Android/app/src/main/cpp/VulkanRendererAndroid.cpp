// Android Vulkan backend. STUB so the app links and the engine wiring can be
// exercised on-device; the real swapchain / pipeline / mesh+sprite drawing is
// task #9. (On iOS the same Vulkan code runs through MoltenVK.)
#include <android/log.h>

#include "Lur/Render/Vulkan/VulkanRenderer.h"

namespace Lur::Render {
namespace {

class VulkanRendererStub : public IRenderer {
public:
    bool Init(void* /*NativeWindow*/) override {
        __android_log_print(ANDROID_LOG_INFO, "OnlyChess", "VulkanRenderer: stub init (TODO #9)");
        return true;
    }
    void Resize(int /*W*/, int /*H*/) override {}
    void Shutdown() override {}

    MeshHandle     CreateMesh(const Vertex*, uint32_t, const uint32_t*, uint32_t) override { return 0; }
    TextureHandle  LoadTexture(const uint8_t*, int, int) override { return 0; }
    MaterialHandle CreateMaterial(const MaterialDesc&) override { return 0; }

    void BeginFrame(const Camera&) override {}
    void DrawMesh(MeshHandle, MaterialHandle, const Math::Mat4&) override {}
    void EndFrame() override {}
};

} // namespace

IRenderer* VulkanRenderer::Create() { return new VulkanRendererStub(); }

} // namespace Lur::Render
