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
# The app's log tag, consumed by the engine's per-platform logging seams (LogTag.h #errors
# without it). Must be set BEFORE the engine root is added, since it becomes a compile
# definition for every target in the tree.
set(LUR_LOG_TAG "OnlyChess" CACHE STRING "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

# android_native_app_glue ships with the NDK (first-party, not a third-party lib).
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# The app's native library. NativeActivity loads this (android.app.lib_name=onlychess).
# The Vulkan backend AND its Android surface seam both live in lur::render now — the app
# supplies no render code at all, only its log tag. Shaders are cooked to SPIR-V offline
# (scripts/gen-shaders.ps1) and embedded in the engine, so no glslc here.
add_library(onlychess SHARED
    AndroidMain.cpp
    # The Android BLE bridge is ENGINE code, one copy for every game
    # (Modules/Transport/Platform/Android). It stays in the app TARGET rather than in
    # lur_transport because it needs the NDK's <jni.h> and the app's LUR_LOG_TAG, and
    # because only an app build has an Android toolchain at all.
    ${LUR_ROOT}/Modules/Transport/Platform/Android/BleTransport.cpp
    # #43 section B: the engine log sink, written once per platform. Same reason it lives in the
    # app TARGET: it needs <android/log.h> and the app's LUR_LOG_TAG.
    ${LUR_ROOT}/Modules/App/Platform/Android/AppPlatform.cpp
    # #43 section B: the NativeActivity entry-point ceremony (glue callbacks, window/renderer
    # lifecycle, the looper drain, internalDataPath). App target for the same reason again — it
    # needs the NDK's native_app_glue, which only an app build has.
    ${LUR_ROOT}/Modules/App/Platform/Android/AndroidApp.cpp)

target_link_libraries(onlychess
    native_app_glue
    chess::app    # #45: chess's IGame answers + its state (pulls in chess::view)
    chess::view   # shared board presentation + touch (pulls in chess::core, lur::render)
    chess::core
    lur::render
    lur::transport
    lur::net
    lur::app    # #43: GameHost — the session + persistence choreography, engine-owned
    lur::save   # persistent device id (issue #17) — stable BLE role across restarts
    lur::sim
    lur::math
    lur::audio  # wait-free SFX mixer + LSF1 codec (AndroidAudioDevice.cpp is its backend)
    lur::trace  # named CPU scopes (#101) — AndroidMain emits the aggregated TRACE line (#192)
    android   # NativeActivity / ANativeWindow
    log       # __android_log_print
    aaudio    # low-latency audio output (NDK, API 26+)
    vulkan)   # the GPU API (Vulkan backend, task #9)

# The capability macros, the log tag and the BLE service UUID, in ONE call (#198). This was four
# hand-copied copies of the same block; LUR_AGENT went MISSING from one of them for a while, which
# silently compiled the agent harness out of the iPhone app. Adding a LUR_* macro is now a one-line
# change in cmake/LurApp.cmake, and a game that forgets this call does not build.
lur_configure_app_target(onlychess
    LOG_TAG  OnlyChess
    BLE_UUID 4C55524D-4F54-4F52-4E00-5472616E7370)

# native_app_glue provides ANativeActivity_onCreate; keep the linker from stripping it.
set_target_properties(onlychess PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")
