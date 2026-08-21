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
