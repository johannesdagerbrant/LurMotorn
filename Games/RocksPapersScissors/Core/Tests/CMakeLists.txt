# Host correctness tests for the RPS sim core: determinism (the load-bearing
# property) + the spec's rule edges. New per-game test exe — chess's fuzz_tests as
# the pattern (there is no shared test framework; a CHECK macro + failure count).
add_executable(rps_sim_tests SimTests.cpp)
target_link_libraries(rps_sim_tests PRIVATE rps::core)
target_compile_features(rps_sim_tests PRIVATE cxx_std_20)
add_test(NAME rps_sim_tests COMMAND rps_sim_tests)
