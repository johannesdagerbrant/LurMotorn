# Host tests for the dev console MODEL (completion / MRU / dispatch / scrollback), all
# render-free pure logic.
add_executable(devconsole_tests DevConsoleTests.cpp)
target_link_libraries(devconsole_tests PRIVATE lur::devconsole lur::core lur::sim)
target_compile_features(devconsole_tests PRIVATE cxx_std_20)
add_test(NAME devconsole_tests COMMAND devconsole_tests)
