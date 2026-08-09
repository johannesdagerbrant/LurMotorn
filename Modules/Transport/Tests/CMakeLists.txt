# Host unit tests for the transport contract (Phase A of issue #3). Dependency-
# free: a plain executable that returns non-zero on failure, registered with CTest.
# Links NO game: Modules may not depend on Games, and that holds for the test targets
# too — the chess move-over-transport integration test lives in chess's own suite.
find_package(Threads REQUIRED)  # EventInbox threaded test uses std::thread
add_executable(transport_tests TransportTests.cpp)
target_link_libraries(transport_tests PRIVATE
    lur::transport
    lur::serialization
    lur::save        # #146: IsValidDeviceId — the device-id shape the role tie-break demands
    Threads::Threads)
target_compile_features(transport_tests PRIVATE cxx_std_20)

add_test(NAME transport_tests COMMAND transport_tests)
