add_executable(devgui_tests DevGuiTests.cpp)
target_link_libraries(devgui_tests PRIVATE lur::devgui)
target_compile_features(devgui_tests PRIVATE cxx_std_20)
add_test(NAME devgui_tests COMMAND devgui_tests)

# #121/#201: the dev console's fold-aware row list, promoted out of Rps::GameView. The property under
# test is that ONE flattened array feeds painting, hit-testing and the anchored editors — a console
# where the row you tap is not the row you see is the classic dev-UI bug.
add_executable(devgui_flatlist_tests FlatListTests.cpp)
target_link_libraries(devgui_flatlist_tests PRIVATE lur::devgui)
target_compile_features(devgui_flatlist_tests PRIVATE cxx_std_20)
add_test(NAME devgui_flatlist_tests COMMAND devgui_flatlist_tests)
