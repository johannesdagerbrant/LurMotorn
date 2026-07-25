# Host unit tests for the render-math module. Dependency-free: a plain executable
# that returns non-zero on failure, registered with CTest.
add_executable(spring_tests SpringTests.cpp)
target_link_libraries(spring_tests PRIVATE lur::math)
target_compile_features(spring_tests PRIVATE cxx_std_20)

add_test(NAME spring_tests COMMAND spring_tests)
