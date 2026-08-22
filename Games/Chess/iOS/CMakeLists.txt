# OnlyChess — the iOS app. The whole shell is lur_add_ios_app() (#198); what is left here is
# chess's identity and its own libraries.
#
# WHY CMake (not XcodeGen / a hand-written .pbxproj / SwiftPM):
#   - The whole repo is already CMake. This file pulls the shared, pure-C++ engine + game core in via
#     the SAME add_subdirectory the Android app uses, so there is exactly one source of truth for the
#     core targets — no duplicated build description, no LIBRARY_SEARCH_PATHS pointing at pre-built
#     .a files.
#   - It adds NO third-party dependency. CMake cross-compiles for iOS out of the box
#     (CMAKE_SYSTEM_NAME=iOS); the .xcodeproj is GENERATED on CI, so nobody has to open or hand-edit
#     an Xcode project on a local Mac.
#   - It produces a real .app bundle (MACOSX_BUNDLE) that xcodebuild builds with code-signing
#     disabled — exactly the CI shape we need.
#
# Configure on CI (see .github/workflows/ios-ipa.yml):
#   cmake -S Games/Chess/iOS -B build -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DMOLTENVK_DIR=... -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake_minimum_required(VERSION 3.22)
project(OnlyChess OBJCXX CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The shared engine + chess core from the repo root (this file is 3 directories up).
get_filename_component(LUR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LUR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The app's log tag, consumed by the engine's per-platform logging seams (LogTag.h #errors without
# it). Must be set BEFORE the engine root is added, since it becomes a compile definition for every
# target in the tree. lur_configure_app_target checks that it agrees with LOG_TAG below.
set(LUR_LOG_TAG "OnlyChess" CACHE STRING "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

include(${LUR_ROOT}/cmake/LurIosApp.cmake)

lur_add_ios_app(OnlyChess
    BUNDLE_ID  com.lurmotorn.onlychess
    LOG_TAG    OnlyChess
    # Chess's BLE service UUID — the advertise/scan discriminator that keeps a chess phone from
    # cross-linking an RTS phone. No default exists (#200): inheriting another game's would fail
    # silently, as two phones that never discover each other.
    BLE_UUID   4C55524D-4F54-4F52-4E00-5472616E7370
    # Shown to the user in the iOS Bluetooth permission prompt, so it names the real game.
    USAGE_NOUN "local two-player chess"
    GAME_LIBS  chess::app     # #45: chess's IGame answers + its state
               chess::view    # board presentation + touch (pulls in chess::core, lur::render)
               chess::core)
