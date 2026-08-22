# OnlyRps — the iOS app. The whole shell is lur_add_ios_app() (#198); what is left here is RPS's
# identity and its own libraries.
#
# This file used to be chess's iOS CMakeLists copied, and said so: "Copy-first: the platform seams are
# duplicated from chess". That copy is where LUR_AGENT went missing for a while, which silently
# compiled the agent harness out of the iPhone app — the platform with no touch injection, i.e. the
# one that most needs it.
#
# Configure on CI (see .github/workflows/ios-ipa.yml):
#   cmake -S Games/RocksPapersScissors/iOS -B build -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DMOLTENVK_DIR=... -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake_minimum_required(VERSION 3.22)
project(OnlyRps OBJCXX CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The shared engine + RPS core from the repo root (this file is 3 directories up).
get_filename_component(LUR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LUR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Must be set BEFORE the engine root is added — it becomes a compile definition for every target in
# the tree. lur_configure_app_target checks that it agrees with LOG_TAG below, so the engine's sources
# and the app's cannot end up filing their lines under different tags.
set(LUR_LOG_TAG "OnlyRps" CACHE STRING "" FORCE)
add_subdirectory(${LUR_ROOT} ${CMAKE_BINARY_DIR}/lurmotorn)

include(${LUR_ROOT}/cmake/LurIosApp.cmake)

lur_add_ios_app(OnlyRps
    BUNDLE_ID  com.lurmotorn.onlyrps
    LOG_TAG    OnlyRps
    # RPS's own BLE service UUID — one hex digit from chess's, and that difference is what keeps an
    # RTS phone from cross-linking a chess phone. No default exists (#200).
    BLE_UUID   4C55524D-4F54-4F52-4E00-5472616E7371
    # Shown to the user in the iOS Bluetooth permission prompt, so it names the real game.
    USAGE_NOUN "local two-player RTS"
    MAIN       Sources/RpsMain.mm
    GAME_LIBS  rps::view      # snapshot rendering (pulls in rps::core, lur::render/hud/text)
               rps::net       # LockstepPeer + event/resync codec
               rps::runtime   # Snapshot + the sim thread
               rps::core)
