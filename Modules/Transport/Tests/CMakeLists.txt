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
# BleProtocol.h has no default service UUID (#200) — a game that forgets to define one
# must fail to build rather than silently adopt another game's. The test binary is not a
# game, so it gets a value that belongs to no game and can never cross-link anything real.
target_compile_definitions(transport_tests PRIVATE
    "LUR_BLE_SERVICE_UUID=\"4C55524D-4F54-4F52-4E00-546573745F30\"")
target_compile_features(transport_tests PRIVATE cxx_std_20)

add_test(NAME transport_tests COMMAND transport_tests)
