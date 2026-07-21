# LurMotorn — top-level build for the SHARED, pure-C++ engine core + Chess core.
#
# This builds everything that is platform-agnostic, so it compiles and unit-tests
# on a desktop host exactly as it does inside the Android NDK / iOS toolchains.
# The hardware-touching backends (BLE, Vulkan, Metal) are NOT built here — they
# are compiled by each app's native build (Gradle / Xcode), which adds this tree
# via add_subdirectory and links the same targets.
cmake_minimum_required(VERSION 3.22)
project(LurMotorn LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LUR_BUILD_TESTS "Build host unit tests" ON)
option(LUR_DESKTOP "Build the desktop Workbench app (Windows window + Vulkan)" OFF)

# Shared compiler discipline (-Wall/-Wextra/-Werror, RTTI off) for the host build.
# Must precede the add_subdirectory calls so it applies to their targets.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EngineFlags.cmake)

# --- Pure-C++ engine modules (shared verbatim across host / Android / iOS) ---
add_subdirectory(Modules/Core)
add_subdirectory(Modules/Serialization)
add_subdirectory(Modules/Sim)
add_subdirectory(Modules/Math)
add_subdirectory(Modules/Save)
add_subdirectory(Modules/Text)
add_subdirectory(Modules/Audio)  # wait-free SFX mixer + LSF1 lossless codec (device backends in the app builds)
add_subdirectory(Modules/Trace)  # named CPU scopes (Insights-style), issue #101 — leaf module, no deps

# --- Header-only interface modules (contracts; backends live in the app builds) ---
add_subdirectory(Modules/Transport)
add_subdirectory(Modules/Pairing)
add_subdirectory(Modules/Net)
add_subdirectory(Modules/Render)
add_subdirectory(Modules/Input)
add_subdirectory(Modules/Hud)
add_subdirectory(Modules/Platform)  # desktop window + input seam (guarded by LUR_DESKTOP)
# Header-only dev-UI utilities. INTERFACE libs (no shipping symbols); their USES are
# #if !LUR_SHIPPING at the call sites, so always-adding them costs a shipping build nothing
# and keeps game targets' links unconditional.
add_subdirectory(Modules/DevGui)      # dev-GUI widget utilities (Numpad, ...) — #113
add_subdirectory(Modules/DevConsole)  # dev console model (+ later its DevGui view) — #114

# --- Games ---
add_subdirectory(Games/Chess)
add_subdirectory(Games/RocksPapersScissors)
if(LUR_DESKTOP)
    add_subdirectory(Games/Chess/Desktop)
    add_subdirectory(Games/RocksPapersScissors/Desktop)
endif()

if(LUR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Modules/Core/Tests)
    add_subdirectory(Modules/Serialization/Tests)
    add_subdirectory(Modules/Sim/Tests)
    add_subdirectory(Modules/Transport/Tests)
    add_subdirectory(Modules/Net/Tests)
    add_subdirectory(Modules/Save/Tests)
    add_subdirectory(Modules/Text/Tests)
    add_subdirectory(Modules/Audio/Tests)
    add_subdirectory(Modules/Trace/Tests)
    add_subdirectory(Modules/DevGui/Tests)
    add_subdirectory(Modules/DevConsole/Tests)
    add_subdirectory(Games/Chess/Core/Tests)
    add_subdirectory(Games/RocksPapersScissors/Core/Tests)
    add_subdirectory(Games/RocksPapersScissors/Runtime/Tests)
    add_subdirectory(Games/RocksPapersScissors/Net/Tests)
endif()
