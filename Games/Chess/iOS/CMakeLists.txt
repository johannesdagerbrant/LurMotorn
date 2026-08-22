# OnlyChess — iOS app, built with CMake's NATIVE iOS support (Xcode generator).
#
# WHY CMake (not XcodeGen / a hand-written .pbxproj / SwiftPM):
#   - The whole repo is already CMake. This file pulls the shared, pure-C++ engine
#     + chess core in via the SAME add_subdirectory the Android app uses, so there
#     is exactly one source of truth for the core targets — no duplicated build
#     description, no LIBRARY_SEARCH_PATHS pointing at pre-built .a files.
#   - It adds NO third-party dependency. CMake >= 3.14 cross-compiles for iOS out
#     of the box (CMAKE_SYSTEM_NAME=iOS); the Xcode *.xcodeproj is GENERATED on CI,
#     so nobody ever has to open or hand-edit a project in Xcode on a local Mac.
#   - It produces a real .app bundle (MACOSX_BUNDLE) that xcodebuild builds for the
#     iOS Simulator with code-signing disabled — exactly the CI shape we need.
#
# Configure on CI (from this directory) with:
#   cmake -S . -B build -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphonesimulator \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
# then build the generated project (see README.md for the exact xcodebuild line).
cmake_minimum_required(VERSION 3.22)
project(OnlyChess OBJCXX CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull in the shared, pure-C++ engine + chess core from the repo root.
# This file lives at Games/Chess/iOS, so the root is 3 directories up.
get_filename_component(LUR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Build the engine/game libraries only — not the host unit-test executables.
set(LUR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The app's log tag, consumed by the engine's per-platform logging seams (LogTag.h #errors
# without it). Must be set BEFORE the engine root is added, since it becomes a compile
# definition for every target in the tree.
set(LUR_LOG_TAG "OnlyChess" CACHE STRING "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

# --- MoltenVK: Vulkan-on-Metal, the ONE sanctioned third-party dependency. ---
# MOLTENVK_DIR is the extracted package root (contains include/ and static/),
# provided by CI (see .github/workflows/macos-ci.yml). Pick the static
# xcframework slice that matches the SDK (device vs simulator).
if(NOT DEFINED MOLTENVK_DIR)
    message(FATAL_ERROR "MOLTENVK_DIR is not set — point it at the extracted MoltenVK package.")
endif()
if(CMAKE_OSX_SYSROOT MATCHES "simulator")
    set(MVK_SLICE "ios-arm64_x86_64-simulator")
else()
    set(MVK_SLICE "ios-arm64")
endif()
set(MVK_INCLUDE "${MOLTENVK_DIR}/include")
set(MVK_LIB "${MOLTENVK_DIR}/static/MoltenVK.xcframework/${MVK_SLICE}/libMoltenVK.a")

# The shared Vulkan backend (lur_render) is compiled for iOS here; give it the
# MoltenVK Vulkan headers (iOS has no system Vulkan).
target_include_directories(lur_render PRIVATE ${MVK_INCLUDE})

# The app bundle. Sources:
#   AppMain.mm          — UIKit shim: Metal-backed view drives the shared BoardView.
#   (the iOS Vulkan platform seam now ships with lur::render, one copy per platform)
#   (the CoreBluetooth backend now ships with the engine, one copy per platform:
#    Modules/Transport/Platform/Ios/BleTransport.mm)
add_executable(OnlyChess MACOSX_BUNDLE
    Sources/AppMain.mm
    ${LUR_ROOT}/Modules/Transport/Platform/Ios/BleTransport.mm
    # #43 section B: engine log sink + the MoltenVK stdio guard, written once per platform.
    ${LUR_ROOT}/Modules/App/Platform/Ios/AppPlatform.mm
    # #43 section B: the shared UIKit half of the #73 reattach heal.
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosViewHost.mm
    # #43 section B: the entry point itself — LurMetalView, the app delegate, and the main()
    # that configures MoltenVK before the first vkCreateInstance.
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosApp.mm)

target_include_directories(OnlyChess PRIVATE ${MVK_INCLUDE})

# The capability macros, the log tag and the BLE service UUID, in ONE call (#198). This was four
# hand-copied copies of the same block; LUR_AGENT went MISSING from one of them for a while, which
# silently compiled the agent harness out of the iPhone app. Adding a LUR_* macro is now a one-line
# change in cmake/LurApp.cmake, and a game that forgets this call does not build.
lur_configure_app_target(OnlyChess
    LOG_TAG  OnlyChess
    BLE_UUID 4C55524D-4F54-4F52-4E00-5472616E7370)

target_link_libraries(OnlyChess PRIVATE
    chess::app
    chess::view   # shared board presentation + touch (pulls in chess::core, lur::render)
    chess::core
    lur::render
    lur::transport
    lur::net
    lur::app    # #43: GameHost — the session + persistence choreography, engine-owned
    lur::save   # persistent device id (issue #17) — stable BLE role across restarts
    lur::sim
    lur::math
    lur::audio  # wait-free SFX mixer + LSF1 codec, incl. its RemoteIO device seam
    lur::trace) # named CPU scopes (#101) + the touch->present latency (#192)

# The .mm sources are Objective-C++ and use ARC. CMake's Xcode generator does NOT
# enable ARC by default, so turn it on explicitly for this target.
set_source_files_properties(
    Sources/AppMain.mm
    ${LUR_ROOT}/Modules/Transport/Platform/Ios/BleTransport.mm
    ${LUR_ROOT}/Modules/App/Platform/Ios/AppPlatform.mm
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosViewHost.mm
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosApp.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# MoltenVK + Apple frameworks. MoltenVK is linked as the matching static slice;
# Metal/QuartzCore/IOSurface/CoreGraphics are its runtime deps.
target_link_libraries(OnlyChess PRIVATE
    ${MVK_LIB}
    "-framework Foundation"
    "-framework UIKit"
    "-framework CoreBluetooth"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework IOSurface"
    "-framework CoreGraphics"
    "-framework AudioToolbox"   # RemoteIO Audio Unit (lur::audio's iOS device seam)
    "-framework AVFAudio")      # AVAudioSession (playback category + IO buffer)

# iOS app bundle metadata. The Info.plist (with the Bluetooth usage strings) is
# generated from Info.plist.in by configure_file below and fed to the bundle.
set_target_properties(OnlyChess PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.lurmotorn.onlychess"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    # Simulator + device both run arm64 on Apple silicon; min iOS 13 covers the
    # CoreBluetooth + UIScene APIs used here.
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY)
