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
