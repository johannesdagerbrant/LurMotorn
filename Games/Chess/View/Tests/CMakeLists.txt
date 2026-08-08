# Host tests for the view layer's game-facing logic. Links chess::view for SfxLibrary's
# variation policy (issue #78) — no audio device is involved, only the picker.
add_executable(chess_sfx_tests ChessSfxTests.cpp)
target_link_libraries(chess_sfx_tests PRIVATE chess::view)
target_compile_features(chess_sfx_tests PRIVATE cxx_std_20)

add_test(NAME chess_sfx_tests COMMAND chess_sfx_tests)
