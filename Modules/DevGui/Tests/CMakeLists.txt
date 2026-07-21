add_executable(devgui_tests DevGuiTests.cpp)
target_link_libraries(devgui_tests PRIVATE lur::devgui)
target_compile_features(devgui_tests PRIVATE cxx_std_20)
add_test(NAME devgui_tests COMMAND devgui_tests)
