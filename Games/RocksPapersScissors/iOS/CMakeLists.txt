# OnlyRps — iOS app (RocksPapersScissors), built with CMake's native iOS support
# (Xcode generator), mirroring Games/Chess/iOS. Copy-first: the platform seams are
# duplicated from chess (the #42 extraction earns its keep with this second consumer).
#
# Configure on CI (see .github/workflows/macos-ci.yml):
#   cmake -S . -B build -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DMOLTENVK_DIR=... -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake_minimum_required(VERSION 3.22)
project(OnlyRps OBJCXX CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared engine + RPS core from the repo root (this file is 3 dirs up, like chess/iOS).
get_filename_component(LUR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LUR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The app's log tag, consumed by the engine's per-platform logging seams (LogTag.h #errors
# without it). Must be set BEFORE the engine root is added, since it becomes a compile
# definition for every target in the tree.
set(LUR_LOG_TAG "OnlyRps" CACHE STRING "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

# --- MoltenVK: Vulkan-on-Metal, the ONE sanctioned third-party dependency. ---
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
target_include_directories(lur_render PRIVATE ${MVK_INCLUDE})

# The app bundle. RpsMain.mm is the UIKit shim (drives LockstepPeer + GameView); the
# Vulkan/BLE seams are copied from chess (package-renamed).
add_executable(OnlyRps MACOSX_BUNDLE
    Sources/RpsMain.mm
    ${LUR_ROOT}/Modules/Transport/Platform/Ios/BleTransport.mm
    # #43 section B: engine log sink + the MoltenVK stdio guard, written once per platform.
    ${LUR_ROOT}/Modules/App/Platform/Ios/AppPlatform.mm
    # #43 section B: the shared UIKit half of the #73 reattach heal.
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosViewHost.mm
    # #43 section B: the entry point itself — LurMetalView, the app delegate, and the main()
    # that configures MoltenVK before the first vkCreateInstance.
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosApp.mm)

target_include_directories(OnlyRps PRIVATE ${MVK_INCLUDE})

# The capability macros, the log tag and the BLE service UUID, in ONE call (#198). This was four
# hand-copied copies of the same block; LUR_AGENT went MISSING from one of them for a while, which
# silently compiled the agent harness out of the iPhone app. Adding a LUR_* macro is now a one-line
# change in cmake/LurApp.cmake, and a game that forgets this call does not build.
lur_configure_app_target(OnlyRps
    LOG_TAG  OnlyRps
    BLE_UUID 4C55524D-4F54-4F52-4E00-5472616E7371)

target_link_libraries(OnlyRps PRIVATE
    rps::view      # snapshot rendering (pulls in rps::core, lur::render/hud/text)
    rps::net       # LockstepPeer + event/resync codec
    rps::runtime   # Snapshot
    rps::core
    lur::render
    lur::transport
    lur::net
    lur::app    # #43: GameHost — engine-owned identity + session lifecycle
    lur::save
    lur::sim
    lur::math
    lur::trace     # #69: named CPU scopes + the ble.toApply latency — now LUR_TRACE=1 references its symbols
    lur::input)  # #151: the shared dev-console gesture (two-finger triple-tap + drag-scroll)

# The .mm sources are Objective-C++ with ARC (CMake's Xcode generator doesn't default it on).
set_source_files_properties(
    Sources/RpsMain.mm
    ${LUR_ROOT}/Modules/Transport/Platform/Ios/BleTransport.mm
    ${LUR_ROOT}/Modules/App/Platform/Ios/AppPlatform.mm
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosViewHost.mm
    ${LUR_ROOT}/Modules/App/Platform/Ios/IosApp.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_link_libraries(OnlyRps PRIVATE
    ${MVK_LIB}
    "-framework Foundation"
    "-framework UIKit"
    "-framework CoreBluetooth"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework IOSurface"
    "-framework CoreGraphics")

set_target_properties(OnlyRps PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.lurmotorn.onlyrps"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY)
