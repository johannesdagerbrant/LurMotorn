# Host unit tests for the net session (issue #5). Dependency-free: a plain
# executable that returns non-zero on failure, registered with CTest. Links the
# chess core + serialization so the move-codec-over-session integration test is
# real, not mocked (mirrors Modules/Transport/Tests).
add_executable(net_tests NetTests.cpp)
target_link_libraries(net_tests PRIVATE
    lur::net
    lur::serialization
    chess::core)
target_compile_features(net_tests PRIVATE cxx_std_20)

add_test(NAME net_tests COMMAND net_tests)
