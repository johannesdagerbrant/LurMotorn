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
    Sources/IosVulkanSurface.mm
    Sources/IosBleTransport.mm)

target_include_directories(OnlyRps PRIVATE ${MVK_INCLUDE})

target_compile_definitions(OnlyRps PRIVATE
    LUR_SHIPPING=${LUR_SHIPPING}
    LUR_INTERNAL=${LUR_INTERNAL}
    LUR_ASSERTS=${LUR_ASSERTS}
    LUR_SLOW=${LUR_SLOW})

target_link_libraries(OnlyRps PRIVATE
    rps::view      # snapshot rendering (pulls in rps::core, lur::render/hud/text)
    rps::net       # LockstepPeer + event/resync codec
    rps::runtime   # Snapshot
    rps::core
    lur::render
    lur::transport
    lur::net
    lur::save
    lur::sim
    lur::math)

# The .mm sources are Objective-C++ with ARC (CMake's Xcode generator doesn't default it on).
set_source_files_properties(
    Sources/RpsMain.mm
    Sources/IosVulkanSurface.mm
    Sources/IosBleTransport.mm
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
