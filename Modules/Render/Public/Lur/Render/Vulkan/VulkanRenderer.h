#pragma once
#include "Lur/Render/Renderer.h"

namespace Lur::Render {

// The single GPU backend. Native Vulkan on Android; on iOS the same Vulkan code
// runs through MoltenVK (Vulkan-on-Metal) — the one sanctioned third-party
// dependency, because no native GPU API spans both platforms.
//
// To stay within what MoltenVK supports, the backend targets the Vulkan
// *portability subset* (VK_KHR_portability_subset): triangle-list meshes,
// vertex/fragment/compute shaders, and standard formats — and avoids the parts
// Metal can't back (geometry & tessellation shaders, wide lines, triangle fans).
// None of those are needed for 2D or typical 3D model rendering.
//
// DEFINED in each app's native build (where the Vulkan SDK + a window handle
// exist); declared here so engine/game code can obtain it without naming Vulkan.
class VulkanRenderer : public IRenderer {
public:
    // AppName is what this process reports to the driver in VkApplicationInfo — it shows up
    // in GPU captures, vendor driver logs and crash reports, so it must be the APP's name.
    // It was hardcoded to a game the engine happens to have shipped first, which meant every
    // other game's frames arrived at the driver wearing the first game's name. Required, not
    // defaulted: a default is how it came to be wrong everywhere but the one app that set it.
    static IRenderer* Create(const char* AppName);
    // ... IRenderer overrides implemented in the app builds.
};

} // namespace Lur::Render
