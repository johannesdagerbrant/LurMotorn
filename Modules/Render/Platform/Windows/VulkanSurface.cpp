// Windows implementation of the Vulkan platform seam (Lur::Render::Vk). The shared
// backend in Modules/Render calls these; this is the only Windows-specific Vulkan
// code. It exists for the desktop Workbench build (roadmap Phase 0.5) — the fast
// iteration loop — and mirrors AndroidVulkanSurface.cpp / IosVulkanSurface.mm.
//
// NativeHandle is an HWND (the Win32 window the desktop shell creates).
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdio>

#include "Lur/Render/Vulkan/PlatformSurface.h"

namespace Lur::Render::Vk {

const char* const* PlatformSurfaceExtensions(uint32_t* Count) {
    static const char* Exts[] = {VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    *Count = 1;
    return Exts;
}

VkResult CreatePlatformSurface(VkInstance Instance, void* NativeHandle,
                               VkSurfaceKHR* OutSurface) {
    VkWin32SurfaceCreateInfoKHR Info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    Info.hinstance = GetModuleHandleW(nullptr);
    Info.hwnd      = static_cast<HWND>(NativeHandle);
    return vkCreateWin32SurfaceKHR(Instance, &Info, nullptr, OutSurface);
}

void PlatformDrawableSize(void* NativeHandle, uint32_t* Width, uint32_t* Height) {
    RECT R{};
    GetClientRect(static_cast<HWND>(NativeHandle), &R);
    *Width  = static_cast<uint32_t>(R.right - R.left);
    *Height = static_cast<uint32_t>(R.bottom - R.top);
}

void PlatformLog(bool Error, const char* Message) {
    std::fprintf(Error ? stderr : stdout, "[Vk] %s\n", Message);
}

} // namespace Lur::Render::Vk
