# Host tests for the RPS lockstep netcode. The event codec's round-trip, byte budget,
# and fuzz-safety — the wire format proven before any transport or window exists.
add_executable(rps_net_tests NetTests.cpp)
target_link_libraries(rps_net_tests PRIVATE rps::net rps::core lur::serialization)
target_compile_features(rps_net_tests PRIVATE cxx_std_20)
add_test(NAME rps_net_tests COMMAND rps_net_tests)
