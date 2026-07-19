# Games/RocksPapersScissors — the RTS (namespace Rps), the second game and the
# proving ground for the reflex-game sim + lockstep netcode (Phase 1, tracker #12).
# Core/ is pure C++ shared verbatim by the future Android/iOS apps; it depends on
# engine Modules but never the reverse. Layout mirrors Games/Chess/ on purpose —
# that duplication is Phase 4's raw material (GameHost/IGame), not a mistake.
add_library(rps_core
    Core/Private/Sim.cpp)

target_include_directories(rps_core PUBLIC Core/Public PRIVATE Core/Private)
# lur::sim for Fixed/Random (the deterministic substrate); lur::core for LUR_ASSERT.
target_link_libraries(rps_core PUBLIC lur::sim lur::core)
target_compile_features(rps_core PUBLIC cxx_std_20)
add_library(rps::core ALIAS rps_core)

# Runtime — the dedicated sim(+transport) thread (#69's loop shape) and the
# double-buffered render snapshot (the tick-thread -> render-thread seam). Headless
# and host-testable: no Vulkan, no window. The future Desktop/Android/iOS mains own
# the render loop and consume snapshots from here.
find_package(Threads REQUIRED)
add_library(rps_runtime
    Runtime/Private/SimRunner.cpp)
target_include_directories(rps_runtime PUBLIC Runtime/Public)
target_link_libraries(rps_runtime PUBLIC rps::core lur::sim lur::core Threads::Threads)
target_compile_features(rps_runtime PUBLIC cxx_std_20)
add_library(rps::runtime ALIAS rps_runtime)

# View — the RPS presentation layer. Talks only to IRenderer (no Vulkan), so it builds
# on the host and is shared by the desktop + phone mains. Draws a Snapshot: field,
# units (CPU-interpolated bring-up, non-instanced), and the HUD.
add_library(rps_view
    View/Private/GameView.cpp)
target_include_directories(rps_view PUBLIC View/Public)
target_link_libraries(rps_view PUBLIC rps::core rps::runtime lur::render lur::hud lur::text lur::math)
target_compile_features(rps_view PUBLIC cxx_std_20)
add_library(rps::view ALIAS rps_view)

# Net — the RPS lockstep netcode: the input event codec (one codec, three uses) and
# the lockstep coordinator (production + ceiling + anchor/desync). Pure C++,
# host-testable — the whole netcode is proven in two windows on one desktop before a
# phone is in the room (#76).
add_library(rps_net
    Net/Private/LockstepPeer.cpp)
target_include_directories(rps_net PUBLIC Net/Public)
target_link_libraries(rps_net PUBLIC rps::core lur::serialization lur::net lur::sim lur::core)
target_compile_features(rps_net PUBLIC cxx_std_20)
add_library(rps::net ALIAS rps_net)
