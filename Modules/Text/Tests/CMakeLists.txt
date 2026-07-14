# Host unit tests for lur::text: validate the cooked MSDF font asset (#24) is
# well-formed. Dependency-free executable, non-zero exit on failure, registered with
# CTest. Mirrors save_tests / net_tests.
add_executable(text_tests FontTests.cpp)
target_link_libraries(text_tests PRIVATE lur::text)
target_compile_features(text_tests PRIVATE cxx_std_20)

add_test(NAME text_tests COMMAND text_tests)
