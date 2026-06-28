# Modules/Render — renderer interface + the SHARED Vulkan backend.
#
# The backend (Private/Vulkan/VulkanBackend.cpp) is platform-neutral; the per-OS
# surface/extension/log seam (Public/Lur/Render/Vulkan/PlatformSurface.h) is
# implemented by each app (Android NDK / iOS MoltenVK). Vulkan headers come from
# the consumer toolchain: the Android NDK sysroot, or the MoltenVK include dir the
# iOS app adds to this target.
#
# On the HOST (the VS-free unit-test build) there is no Vulkan SDK, and the
# renderer isn't unit-tested, so we stay header-only there — only device builds
# compile the backend.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(lur_render STATIC Private/Vulkan/VulkanBackend.cpp)
    target_include_directories(lur_render PUBLIC Public)
    target_compile_features(lur_render PUBLIC cxx_std_20)
    target_link_libraries(lur_render PUBLIC lur_math)
else()
    add_library(lur_render INTERFACE)
    target_include_directories(lur_render INTERFACE Public)
    target_compile_features(lur_render INTERFACE cxx_std_20)
    target_link_libraries(lur_render INTERFACE lur_math)
endif()
add_library(lur::render ALIAS lur_render)
