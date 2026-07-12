# Host unit tests for lur::save (issue #17, Phase A). Dependency-free: a plain
# executable that returns non-zero on failure, registered with CTest. Uses the
# system temp dir for scratch storage, so it never touches the repo tree.
add_executable(save_tests SaveTests.cpp)
target_link_libraries(save_tests PRIVATE lur::save)
target_compile_features(save_tests PRIVATE cxx_std_20)

add_test(NAME save_tests COMMAND save_tests)
