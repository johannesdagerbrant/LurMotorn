# Host unit tests for the transport contract (Phase A of issue #3). Dependency-
# free: a plain executable that returns non-zero on failure, registered with CTest.
# Links NO game: Modules may not depend on Games, and that holds for the test targets
# too — a game's move-over-transport integration test lives in that game's own suite.
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

# The BLE send-queue policy, host-tested against a fake radio (#197). This logic used to live in
# BleShim.kt where no test could reach it — which is why it drifted between the two games. The
# fake is the point: it can refuse a write, lose a completion, or drop the link on demand, none
# of which a real radio does when asked.
add_executable(ble_send_queue_tests BleSendQueueTests.cpp)
target_link_libraries(ble_send_queue_tests PRIVATE lur::transport)
target_compile_features(ble_send_queue_tests PRIVATE cxx_std_20)
add_test(NAME ble_send_queue_tests COMMAND ble_send_queue_tests)
