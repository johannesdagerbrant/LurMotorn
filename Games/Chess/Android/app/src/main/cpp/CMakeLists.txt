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

# --- Shaders: GLSL -> SPIR-V -> includable uint32 array. ---
# glslc ships with the NDK (first-party). -mfmt=num emits a comma-separated
# init list we #include straight into a uint32_t[] in the renderer — no runtime
# asset loading, no committed binaries. The same SPIR-V will feed MoltenVK on iOS.
if(NOT ANDROID_HOST_TAG)
    file(GLOB _host_tags RELATIVE "${ANDROID_NDK}/shader-tools" "${ANDROID_NDK}/shader-tools/*")
    list(GET _host_tags 0 ANDROID_HOST_TAG)
endif()
set(GLSLC "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
set(SHADER_GEN_DIR "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})

set(SHADER_INCS "")
foreach(stage IN ITEMS Sprite.vert Sprite.frag)
    set(inc "${SHADER_GEN_DIR}/${stage}.inc")
    add_custom_command(
        OUTPUT ${inc}
        COMMAND ${GLSLC} -mfmt=num "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${stage}" -o ${inc}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${stage}"
        COMMENT "Compiling shader ${stage} -> SPIR-V"
        VERBATIM)
    list(APPEND SHADER_INCS ${inc})
endforeach()
add_custom_target(onlychess_shaders DEPENDS ${SHADER_INCS})

# The app's native library. NativeActivity loads this (android.app.lib_name=onlychess).
add_library(onlychess SHARED
    AndroidMain.cpp
    VulkanRendererAndroid.cpp
    AndroidBleTransport.cpp)

# Generate the shader includes before compiling the renderer; expose their dir.
add_dependencies(onlychess onlychess_shaders)
target_include_directories(onlychess PRIVATE ${SHADER_GEN_DIR})

target_link_libraries(onlychess
    native_app_glue
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
