# Host unit tests for the render-math module. Dependency-free: a plain executable
# that returns non-zero on failure, registered with CTest.
add_executable(spring_tests SpringTests.cpp)
target_link_libraries(spring_tests PRIVATE lur::math)
target_compile_features(spring_tests PRIVATE cxx_std_20)

add_test(NAME spring_tests COMMAND spring_tests)

# #201: the nearest-feasible-point projection promoted out of RPS's magnetic placement. Tested here
# because its defining property is CONTINUITY in the input, which is a property of the algorithm and
# not of any game's entity layout.
add_executable(math_projection_tests ProjectionTests.cpp)
target_link_libraries(math_projection_tests PRIVATE lur_math)
target_compile_features(math_projection_tests PRIVATE cxx_std_20)
add_test(NAME math_projection_tests COMMAND math_projection_tests)
