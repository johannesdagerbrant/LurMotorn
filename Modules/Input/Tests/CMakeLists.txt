# Host unit tests for Modules/Input — the shared dev-console gesture (#151). The recognizer moved
# here precisely so its timing windows and drag slop could be tested instead of hand-copied into a
# third platform shim.
add_executable(input_tests InputTests.cpp)
target_link_libraries(input_tests PRIVATE lur::input)
target_compile_features(input_tests PRIVATE cxx_std_20)
add_test(NAME input_tests COMMAND input_tests)
