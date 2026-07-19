# Host tests for the tick-thread + snapshot seam (#69). Threading is exercised for
# real (a live SimRunner), but every ASSERTION is timing-independent: it compares
# the threaded run to a synchronous one by tick number, so it can't flake.
add_executable(rps_runtime_tests RunnerTests.cpp)
target_link_libraries(rps_runtime_tests PRIVATE rps::runtime rps::core)
target_compile_features(rps_runtime_tests PRIVATE cxx_std_20)
add_test(NAME rps_runtime_tests COMMAND rps_runtime_tests)
