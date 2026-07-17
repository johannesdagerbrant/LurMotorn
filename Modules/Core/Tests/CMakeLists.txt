# Host unit tests for Modules/Core (the Lur::Log seam; LUR_ASSERT is header-only).
add_executable(core_tests CoreTests.cpp)
target_link_libraries(core_tests PRIVATE lur::core)
target_compile_features(core_tests PRIVATE cxx_std_20)
add_test(NAME core_tests COMMAND core_tests)
