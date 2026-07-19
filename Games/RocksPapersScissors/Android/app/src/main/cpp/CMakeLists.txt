cmake_minimum_required(VERSION 3.22)
# C is needed for the NDK's android_native_app_glue.c; CXX for everything else.
project(onlyrps C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull in the shared, pure-C++ engine + chess core from the repo root.
# This file lives at Games/Chess/Android/app/src/main/cpp, so the root is 7 up.
get_filename_component(LUR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)

# Build the engine/game libraries only — not the host unit-test executables.
set(LUR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

# android_native_app_glue ships with the NDK (first-party, not a third-party lib).
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# The app's native library. NativeActivity loads this (android.app.lib_name=onlyrps).
# The shared Vulkan backend lives in lur::render; this app provides only the thin
# Android surface seam (AndroidVulkanSurface.cpp). Shaders are cooked to SPIR-V
# offline (scripts/gen-shaders.ps1) and embedded in the engine, so no glslc here.
add_library(onlyrps SHARED
    RpsMain.cpp
    AndroidVulkanSurface.cpp
    AndroidBleTransport.cpp)

target_link_libraries(onlyrps
    native_app_glue
    rps::view      # snapshot rendering (pulls in rps::core, lur::render/hud/text)
    rps::net       # LockstepPeer + event/resync codec
    rps::runtime   # Snapshot
    rps::core
    lur::render
    lur::transport
    lur::net
    lur::save   # persistent device id (issue #17) — stable BLE role across restarts
    lur::sim
    lur::math
    android   # NativeActivity / ANativeWindow
    log       # __android_log_print
    vulkan)   # the GPU API (Vulkan backend, task #9)

# The build-config capability macros (LUR_INTERNAL etc.) are derived by
# cmake/EngineFlags.cmake during the add_subdirectory(LUR_ROOT) above and published
# as cache vars. This app target lives in the parent (onlyrps) project scope, out
# of reach of the engine tree's add_compile_definitions, so apply them explicitly —
# AndroidMain's soak/autoplayer (issue #57/#58) is #if LUR_INTERNAL. Default config
# is Development (INTERNAL=1); a release build passes -DLUR_CONFIG=Shipping.
target_compile_definitions(onlyrps PRIVATE
    LUR_SHIPPING=${LUR_SHIPPING}
    LUR_INTERNAL=${LUR_INTERNAL}
    LUR_ASSERTS=${LUR_ASSERTS}
    LUR_SLOW=${LUR_SLOW})

# native_app_glue provides ANativeActivity_onCreate; keep the linker from stripping it.
set_target_properties(onlyrps PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")
