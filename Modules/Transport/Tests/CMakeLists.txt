# Host unit tests for the transport contract (Phase A of issue #3). Dependency-
# free: a plain executable that returns non-zero on failure, registered with
# CTest. Links the chess core + serialization so the move-codec-over-transport
# integration test is real, not mocked.
add_executable(transport_tests TransportTests.cpp)
target_link_libraries(transport_tests PRIVATE
    lur::transport
    lur::serialization
    chess::core)
target_compile_features(transport_tests PRIVATE cxx_std_20)

add_test(NAME transport_tests COMMAND transport_tests)
