# modules/transport — the link abstraction, and the DECIDING half of the BLE link.
#
# Was header-only, with every platform driver compiled by an app's native build. It is a
# compiled library now because the policy is moving here (#197): send flow control first, then
# retry/backoff, the discovery watchdog, role escalation and reconnect scheduling. All of that
# used to live in Kotlin and ObjC++, where no host test could reach it — which is exactly why
# it drifted 650 lines between the two games. The platform drivers stay dumb: API verbs and
# event forwarding, no decisions.
add_library(lur_transport STATIC
    Private/BleSendQueue.cpp
    Private/BleStartRetry.cpp
    Private/BleDiscoveryTimers.cpp)
target_include_directories(lur_transport PUBLIC Public)
target_compile_features(lur_transport PUBLIC cxx_std_20)
target_link_libraries(lur_transport PUBLIC lur::core)   # EventInbox uses LUR_ASSERT
target_link_libraries(lur_transport PUBLIC lur::trace)  # EventInbox emits ble.toApply latency (#101-D)
add_library(lur::transport ALIAS lur_transport)
