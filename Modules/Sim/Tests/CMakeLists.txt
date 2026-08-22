# Host correctness tests for the deterministic sim primitives (TickClock + Fixed).
add_executable(sim_tests SimTests.cpp)
target_link_libraries(sim_tests PRIVATE lur::sim)
target_compile_features(sim_tests PRIVATE cxx_std_20)
add_test(NAME sim_tests COMMAND sim_tests)

# #201: the deterministic CSR broadphase promoted out of Rps::Sim. Tested here because what it
# guarantees is a DETERMINISM contract (ascending-within-cell, fixed bin order, grid == brute force),
# and a violation of that shows up as two peers diverging with nothing on the wire to blame.
add_executable(csr_buckets_tests CsrBucketsTests.cpp)
target_link_libraries(csr_buckets_tests PRIVATE lur::sim)
target_compile_features(csr_buckets_tests PRIVATE cxx_std_20)
add_test(NAME csr_buckets_tests COMMAND csr_buckets_tests)

# #201: the fixed-timestep sim THREAD promoted out of Rps::SimRunner. Threads::Threads is linked
# here rather than on lur::sim, because SimThread.h is the only header in the module that spawns one
# and the module's many header-only consumers should not inherit the dependency.
find_package(Threads REQUIRED)
add_executable(sim_thread_tests SimThreadTests.cpp)
target_link_libraries(sim_thread_tests PRIVATE lur::sim Threads::Threads)
target_compile_features(sim_thread_tests PRIVATE cxx_std_20)
add_test(NAME sim_thread_tests COMMAND sim_thread_tests)

# #201: the input-thread -> sim-thread event inbox promoted out of Rps::SoloInputInbox and
# LockstepPeer::PendingLocalEvents. Threaded conservation test, hence Threads::Threads.
add_executable(input_inbox_tests InputInboxTests.cpp)
target_link_libraries(input_inbox_tests PRIVATE lur::sim Threads::Threads)
target_compile_features(input_inbox_tests PRIVATE cxx_std_20)
add_test(NAME input_inbox_tests COMMAND input_inbox_tests)

# #201: the rollback snapshot ring promoted out of Rps::SnapshotRing. What it guarantees is that an
# EVICTED tick answers null rather than the slot's current (valid-looking, wrong) occupant — a
# violation restores the sim to a tick nobody asked for and diverges silently.
add_executable(snapshot_ring_tests SnapshotRingTests.cpp)
target_link_libraries(snapshot_ring_tests PRIVATE lur::sim)
target_compile_features(snapshot_ring_tests PRIVATE cxx_std_20)
add_test(NAME snapshot_ring_tests COMMAND snapshot_ring_tests)
