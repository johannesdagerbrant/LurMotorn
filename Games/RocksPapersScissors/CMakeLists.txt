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
