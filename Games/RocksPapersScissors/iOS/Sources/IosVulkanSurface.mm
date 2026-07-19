// iOS implementation of the Vulkan platform seam (Lur::Render::Vk), via MoltenVK.
// The shared backend in Modules/Render calls these; this is the only iOS-specific
// Vulkan code. (The Android counterpart is AndroidVulkanSurface.cpp.)
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <os/log.h>

// Define before the FIRST include of vulkan.h so the Metal surface symbols are
// exposed (PlatformSurface.h re-includes vulkan.h, guarded — so the define must
// come first, here).
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include "Lur/Render/Vulkan/PlatformSurface.h"

namespace Lur::Render::Vk {

const char* const* PlatformSurfaceExtensions(uint32_t* Count) {
    // Metal surface + portability enumeration (MoltenVK is a portability driver).
    static const char* Exts[] = {
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    };
    *Count = 2;
    return Exts;
}

VkResult CreatePlatformSurface(VkInstance Instance, void* NativeHandle,
                               VkSurfaceKHR* OutSurface) {
    VkMetalSurfaceCreateInfoEXT Info{};
    Info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    Info.pLayer = (__bridge CAMetalLayer*)NativeHandle;
    return vkCreateMetalSurfaceEXT(Instance, &Info, nullptr, OutSurface);
}

void PlatformDrawableSize(void* NativeHandle, uint32_t* Width, uint32_t* Height) {
    CAMetalLayer* Layer = (__bridge CAMetalLayer*)NativeHandle;
    const CGSize Size = Layer.drawableSize;
    *Width = static_cast<uint32_t>(Size.width);
    *Height = static_cast<uint32_t>(Size.height);
}

// Every backend log line streams to the dev host over syslog
// (scripts/ios-syslog.bat) — the logcat analog. The "OnlyRps:" tag is the
// filter; %{public}s is load-bearing: without it iOS redacts the dynamic string
// to "<private>" when the device isn't attached to Xcode (i.e. exactly our case).
void PlatformLog(bool Error, const char* Message) {
    os_log_with_type(OS_LOG_DEFAULT,
                     Error ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
                     "OnlyRps: %{public}s", Message);
}

} // namespace Lur::Render::Vk
