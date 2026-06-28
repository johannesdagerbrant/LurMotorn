#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

// The ONLY platform-specific parts of the Vulkan backend. Everything else in
// Modules/Render is shared C++. Each app provides one tiny implementation:
//   Android: ANativeWindow + VK_KHR_android_surface  (AndroidVulkanSurface.cpp)
//   iOS:     CAMetalLayer   + VK_EXT_metal_surface    (IosVulkanSurface.mm, MoltenVK)
namespace Lur::Render::Vk {

// Instance extensions the platform's surface needs, in ADDITION to VK_KHR_surface.
// Returns a static array; *Count is its length.
const char* const* PlatformSurfaceExtensions(uint32_t* Count);

// Create a presentable surface for NativeHandle (ANativeWindow* on Android,
// CAMetalLayer* on iOS).
VkResult CreatePlatformSurface(VkInstance Instance, void* NativeHandle,
                               VkSurfaceKHR* OutSurface);

// Pixel size of the drawable behind NativeHandle — used only as a fallback when
// the surface capabilities report an undefined (0xFFFFFFFF) current extent.
void PlatformDrawableSize(void* NativeHandle, uint32_t* Width, uint32_t* Height);

// Platform logging sink (Android logcat / iOS NSLog).
void PlatformLog(bool Error, const char* Message);

} // namespace Lur::Render::Vk
