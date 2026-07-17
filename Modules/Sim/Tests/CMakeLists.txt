# Host correctness tests for the deterministic sim primitives (TickClock + Fixed).
add_executable(sim_tests SimTests.cpp)
target_link_libraries(sim_tests PRIVATE lur::sim)
target_compile_features(sim_tests PRIVATE cxx_std_20)
add_test(NAME sim_tests COMMAND sim_tests)
