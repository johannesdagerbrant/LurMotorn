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
# The app's log tag, consumed by the engine's per-platform logging seams (LogTag.h #errors
# without it). Must be set BEFORE the engine root is added, since it becomes a compile
# definition for every target in the tree.
set(LUR_LOG_TAG "OnlyRps" CACHE STRING "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

# android_native_app_glue ships with the NDK (first-party, not a third-party lib).
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# The app's native library. NativeActivity loads this (android.app.lib_name=onlyrps).
# The shared Vulkan backend lives in lur::render; this app provides only the thin
# Android surface seam (now in lur::render). Shaders are cooked to SPIR-V
# offline (scripts/gen-shaders.ps1) and embedded in the engine, so no glslc here.
add_library(onlyrps SHARED
    RpsMain.cpp
    # The Android BLE bridge is ENGINE code, one copy for every game
    # (Modules/Transport/Platform/Android). It stays in the app TARGET rather than in
    # lur_transport because it needs the NDK's <jni.h> and the app's LUR_LOG_TAG, and
    # because only an app build has an Android toolchain at all.
    ${LUR_ROOT}/Modules/Transport/Platform/Android/BleTransport.cpp)

target_link_libraries(onlyrps
    native_app_glue
    rps::view      # snapshot rendering (pulls in rps::core, lur::render/hud/text)
    rps::net       # LockstepPeer + event/resync codec
    rps::runtime   # Snapshot
    rps::core
    lur::render
    lur::transport
    lur::net
    lur::app    # #43: GameHost — engine-owned identity + session lifecycle
    lur::save   # persistent device id (issue #17) — stable BLE role across restarts
    lur::sim
    lur::math
    lur::trace  # named CPU scopes (#101) — RpsMain emits the aggregated TRACE line
    lur::input  # #151: the shared dev-console gesture (two-finger triple-tap + drag-scroll)
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
    LUR_SLOW=${LUR_SLOW}
    LUR_TRACE=${LUR_TRACE}
    # Assistant-only remote control / capture. Off unless the build was explicitly configured
    # with -DLUR_AGENT=ON, so an ordinary Gradle build — the one a player installs — has none of
    # it compiled in at all. LUR_AGENT_ON is the derived value EngineFlags publishes.
    LUR_AGENT=${LUR_AGENT_ON}
    # The app's log tag, for the engine platform sources compiled into THIS target.
    # Re-applied here because an out-of-tree app target cannot see the engine tree's
    # add_compile_definitions — the same reason the LUR_* capability macros are repeated
    # above (CLAUDE.md). Lur/Core/LogTag.h #errors without it.
    LUR_LOG_TAG="${LUR_LOG_TAG}"
    # Distinct per-game BLE service UUID (matches the Kotlin BleShim SERVICE_UUID and
    # the iOS build) so RPS never cross-links a chess phone.
    "LUR_BLE_SERVICE_UUID=\"4C55524D-4F54-4F52-4E00-5472616E7371\"")

# native_app_glue provides ANativeActivity_onCreate; keep the linker from stripping it.
set_target_properties(onlyrps PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")
