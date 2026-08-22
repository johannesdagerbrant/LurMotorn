# Host unit tests for the net session (issue #5). Dependency-free: a plain executable
# that returns non-zero on failure, registered with CTest. Links NO game — everything
# here holds with no game present at all; the game-over-session integration tests moved into
# the game suites when Phase 0 restored the module wall in the test tree.
add_executable(net_tests NetTests.cpp)
target_link_libraries(net_tests PRIVATE
    lur::net
    lur::serialization)
target_compile_features(net_tests PRIVATE cxx_std_20)

add_test(NAME net_tests COMMAND net_tests)

# #163/#201: the lost/duplicated input-frame detector promoted out of Rps::LockstepPeer. The signed
# shorter-way-round-the-circle reading is what stops a DOUBLED link reading as a 255-frame hole.
add_executable(frame_sequence_tests FrameSequenceTests.cpp)
target_link_libraries(frame_sequence_tests PRIVATE lur::net)
target_compile_features(frame_sequence_tests PRIVATE cxx_std_20)
add_test(NAME frame_sequence_tests COMMAND frame_sequence_tests)

# #161/#167/#210/#212/#201: the recovery state machine promoted out of Rps::LockstepPeer. Every test in
# here is named after the bug that taught the rule — a two-peer soak can say the pair stopped
# converging, but not WHICH rule broke.
add_executable(recovery_policy_tests RecoveryPolicyTests.cpp)
target_link_libraries(recovery_policy_tests PRIVATE lur::net)
target_compile_features(recovery_policy_tests PRIVATE cxx_std_20)
add_test(NAME recovery_policy_tests COMMAND recovery_policy_tests)

# #112/#164/#166/#201: the build-fingerprint refusal promoted out of Rps::LockstepPeer. All three of its
# rules came from the same symptom — the two phones DISAGREEING about whether there was a mismatch — so
# the tests are two-sided where the rule is about symmetry.
add_executable(build_gate_tests BuildGateTests.cpp)
target_link_libraries(build_gate_tests PRIVATE lur::net)
target_compile_features(build_gate_tests PRIVATE cxx_std_20)
add_test(NAME build_gate_tests COMMAND build_gate_tests)
