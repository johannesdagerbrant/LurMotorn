# Host unit tests for the serialization module. Dependency-free: a plain
# executable that returns non-zero on failure, registered with CTest.
add_executable(bitstream_tests BitstreamTests.cpp)
target_link_libraries(bitstream_tests PRIVATE lur::serialization)
target_compile_features(bitstream_tests PRIVATE cxx_std_20)

add_test(NAME bitstream_tests COMMAND bitstream_tests)
