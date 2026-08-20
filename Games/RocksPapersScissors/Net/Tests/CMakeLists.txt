# Host tests for the RPS lockstep netcode. The event codec's round-trip, byte budget,
# and fuzz-safety — the wire format proven before any transport or window exists.
add_executable(rps_net_tests NetTests.cpp)
target_link_libraries(rps_net_tests PRIVATE rps::net rps::core lur::serialization lur::net lur::transport)
target_compile_features(rps_net_tests PRIVATE cxx_std_20)
add_test(NAME rps_net_tests COMMAND rps_net_tests)

# The two-peer soak (#211). A HOST TEST, deliberately not a desktop flag: the desktop's two-window
# loopback needs a window and a Vulkan device, and the soak needs neither — everything it touches
# (LockstepPeer, Sim, AiController) already builds here. Shares LockstepHarness.h with rps_net_tests,
# which is the whole reason those helpers moved out of NetTests.cpp.
#
# The CTest entry runs the SHORT (gate) duration. `--long` is the manual/nightly one and is not wired
# into build.ps1: the everyday loop stays ~1 minute, and a soak nobody waits for gets skipped.
add_executable(rps_two_peer_soak TwoPeerSoak.cpp)
target_link_libraries(rps_two_peer_soak PRIVATE rps::net rps::core lur::serialization lur::net)
target_compile_features(rps_two_peer_soak PRIVATE cxx_std_20)
add_test(NAME rps_two_peer_soak COMMAND rps_two_peer_soak)

# The agent verb table (issue #43, section E). Built with LUR_AGENT=1, which is the point: this is
# agent-only code, so it is absent from every app build, and until it moved out of the two platform
# mains there was NOWHERE a test could reach it. That is why the two copies were free to drift.
# rps::net itself is built without the flag; the router is header-only, so the test compiles its own.
add_executable(rps_agent_router_tests AgentRouterTests.cpp)
target_link_libraries(rps_agent_router_tests PRIVATE rps::net)
target_compile_options(rps_agent_router_tests PRIVATE -ULUR_AGENT -DLUR_AGENT=1)
target_compile_features(rps_agent_router_tests PRIVATE cxx_std_20)
add_test(NAME rps_agent_router_tests COMMAND rps_agent_router_tests)
