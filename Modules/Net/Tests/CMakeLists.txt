# Host unit tests for the net session (issue #5). Dependency-free: a plain executable
# that returns non-zero on failure, registered with CTest. Links NO game — everything
# here holds with no game present at all; the chess-over-session integration tests moved
# to Games/Chess/Core/Tests when Phase 0 restored the module wall in the test tree.
add_executable(net_tests NetTests.cpp)
target_link_libraries(net_tests PRIVATE
    lur::net
    lur::serialization)
target_compile_features(net_tests PRIVATE cxx_std_20)

add_test(NAME net_tests COMMAND net_tests)
