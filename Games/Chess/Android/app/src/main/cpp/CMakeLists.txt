cmake_minimum_required(VERSION 3.22)
# C is needed for the NDK's android_native_app_glue.c; CXX for everything else.
project(onlychess C CXX)

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

# The app's native library. NativeActivity loads this (android.app.lib_name=onlychess).
# The shared Vulkan backend lives in lur::render; this app provides only the thin
# Android surface seam (AndroidVulkanSurface.cpp). Shaders are cooked to SPIR-V
# offline (scripts/gen-shaders.ps1) and embedded in the engine, so no glslc here.
add_library(onlychess SHARED
    AndroidMain.cpp
    AndroidVulkanSurface.cpp
    AndroidBleTransport.cpp)

target_link_libraries(onlychess
    native_app_glue
    chess::view   # shared board presentation + touch (pulls in chess::core, lur::render)
    chess::core
    lur::render
    lur::transport
    lur::net
    lur::sim
    lur::math
    android   # NativeActivity / ANativeWindow
    log       # __android_log_print
    vulkan)   # the GPU API (Vulkan backend, task #9)

# native_app_glue provides ANativeActivity_onCreate; keep the linker from stripping it.
set_target_properties(onlychess PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")
