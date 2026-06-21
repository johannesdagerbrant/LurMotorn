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

# The app bundle. Sources:
#   AppMain.mm        — Obj-C++ UIKit entry point + the chess-core smoke test.
#   IosBleTransport.mm — the CoreBluetooth backend of Lur::Transport::ITransport.
add_executable(OnlyChess MACOSX_BUNDLE
    Sources/AppMain.mm
    Sources/IosBleTransport.mm)

target_link_libraries(OnlyChess PRIVATE
    chess::core
    lur::transport
    lur::net
    lur::sim
    lur::math)

# The .mm sources are Objective-C++ and use ARC (CoreBluetooth objects are
# ARC-managed). CMake's Xcode generator does NOT enable ARC by default, so turn it
# on explicitly for this target.
set_source_files_properties(
    Sources/AppMain.mm
    Sources/IosBleTransport.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# Apple frameworks (first-party SDK — the only dependencies allowed here).
# NO Vulkan / MoltenVK yet: this is a BLE-and-core-only skeleton.
target_link_libraries(OnlyChess PRIVATE
    "-framework Foundation"
    "-framework UIKit"
    "-framework CoreBluetooth")

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
