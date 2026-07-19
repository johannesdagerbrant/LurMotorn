// Android implementation of the Vulkan platform seam (Lur::Render::Vk). The
// shared backend in Modules/Render calls these; this is the only Android-specific
// Vulkan code. (The iOS counterpart is IosVulkanSurface.mm, via MoltenVK.)
#include <android/log.h>
#include <android/native_window.h>

// Define before the FIRST include of vulkan.h so the Android surface symbols are
// exposed (PlatformSurface.h re-includes vulkan.h, but the header guard makes
// that a no-op — so the define must come first, here).
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "Lur/Render/Vulkan/PlatformSurface.h"

namespace Lur::Render::Vk {

const char* const* PlatformSurfaceExtensions(uint32_t* Count) {
    static const char* Exts[] = {VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    *Count = 1;
    return Exts;
}

VkResult CreatePlatformSurface(VkInstance Instance, void* NativeHandle,
                               VkSurfaceKHR* OutSurface) {
    VkAndroidSurfaceCreateInfoKHR Info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    Info.window = static_cast<ANativeWindow*>(NativeHandle);
    return vkCreateAndroidSurfaceKHR(Instance, &Info, nullptr, OutSurface);
}

void PlatformDrawableSize(void* NativeHandle, uint32_t* Width, uint32_t* Height) {
    ANativeWindow* W = static_cast<ANativeWindow*>(NativeHandle);
    *Width = static_cast<uint32_t>(ANativeWindow_getWidth(W));
    *Height = static_cast<uint32_t>(ANativeWindow_getHeight(W));
}

void PlatformLog(bool Error, const char* Message) {
    __android_log_print(Error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                        "OnlyRps", "%s", Message);
}

} // namespace Lur::Render::Vk
