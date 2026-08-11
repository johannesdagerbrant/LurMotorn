# Host unit tests for Lur::App (issue #43). Links NO game: GameHost's choreography is engine
# behaviour, and proving that on the host is the whole reason it moved out of the two mains.
add_executable(app_tests AppTests.cpp)
target_link_libraries(app_tests PRIVATE lur::app lur::transport)
target_compile_features(app_tests PRIVATE cxx_std_20)

add_test(NAME app_tests COMMAND app_tests)
