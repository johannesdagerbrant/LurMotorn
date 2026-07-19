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
#   IosVulkanSurface.mm — iOS implementation of the Vulkan platform seam (MoltenVK).
#   IosBleTransport.mm  — the CoreBluetooth backend of Lur::Transport::ITransport.
add_executable(OnlyChess MACOSX_BUNDLE
    Sources/AppMain.mm
    Sources/IosVulkanSurface.mm
    Sources/IosBleTransport.mm
    Sources/IosAudioDevice.mm)

target_include_directories(OnlyChess PRIVATE ${MVK_INCLUDE})

# Build-config capability macros (LUR_INTERNAL etc.), derived by EngineFlags.cmake in
# the add_subdirectory(root) above. Like the Android main, this app target is in the
# parent scope, out of the engine tree's add_compile_definitions reach, so apply them
# here. Default Development; a release build passes -DLUR_CONFIG=Shipping.
target_compile_definitions(OnlyChess PRIVATE
    LUR_SHIPPING=${LUR_SHIPPING}
    LUR_INTERNAL=${LUR_INTERNAL}
    LUR_ASSERTS=${LUR_ASSERTS}
    LUR_SLOW=${LUR_SLOW})

target_link_libraries(OnlyChess PRIVATE
    chess::view   # shared board presentation + touch (pulls in chess::core, lur::render)
    chess::core
    lur::render
    lur::transport
    lur::net
    lur::save   # persistent device id (issue #17) — stable BLE role across restarts
    lur::sim
    lur::math
    lur::audio) # wait-free SFX mixer + LSF1 codec (IosAudioDevice.mm is its RemoteIO backend)

# The .mm sources are Objective-C++ and use ARC. CMake's Xcode generator does NOT
# enable ARC by default, so turn it on explicitly for this target.
set_source_files_properties(
    Sources/AppMain.mm
    Sources/IosVulkanSurface.mm
    Sources/IosBleTransport.mm
    Sources/IosAudioDevice.mm
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
    "-framework AudioToolbox"   # RemoteIO Audio Unit (IosAudioDevice.mm)
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
