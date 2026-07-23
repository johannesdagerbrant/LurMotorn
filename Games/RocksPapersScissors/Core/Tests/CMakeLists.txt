# Host correctness tests for the RPS sim core: determinism (the load-bearing
# property) + the spec's rule edges. New per-game test exe — chess's fuzz_tests as
# the pattern (there is no shared test framework; a CHECK macro + failure count).
add_executable(rps_sim_tests SimTests.cpp)
target_link_libraries(rps_sim_tests PRIVATE rps::core)
target_compile_features(rps_sim_tests PRIVATE cxx_std_20)
add_test(NAME rps_sim_tests COMMAND rps_sim_tests)

# AI opponent (#124-#126): the AiController is a pure function of sim state + tick, so it is
# host-testable without a renderer — determinism, the fixed opening, the counter choice, and
# that Hard reads the enemy more sharply than Easy.
add_executable(rps_ai_tests AiTests.cpp)
target_link_libraries(rps_ai_tests PRIVATE rps::core)
target_compile_features(rps_ai_tests PRIVATE cxx_std_20)
add_test(NAME rps_ai_tests COMMAND rps_ai_tests)
