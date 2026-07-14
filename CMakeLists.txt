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

# --- Pure-C++ engine modules (shared verbatim across host / Android / iOS) ---
add_subdirectory(Modules/Serialization)
add_subdirectory(Modules/Sim)
add_subdirectory(Modules/Math)
add_subdirectory(Modules/Save)
add_subdirectory(Modules/Text)

# --- Header-only interface modules (contracts; backends live in the app builds) ---
add_subdirectory(Modules/Transport)
add_subdirectory(Modules/Pairing)
add_subdirectory(Modules/Net)
add_subdirectory(Modules/Render)
add_subdirectory(Modules/Input)
add_subdirectory(Modules/Hud)

# --- Games ---
add_subdirectory(Games/Chess)

if(LUR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Modules/Serialization/Tests)
    add_subdirectory(Modules/Transport/Tests)
    add_subdirectory(Modules/Net/Tests)
    add_subdirectory(Modules/Save/Tests)
    add_subdirectory(Modules/Text/Tests)
    add_subdirectory(Games/Chess/Core/Tests)
endif()
