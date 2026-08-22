# Host tests for the view layer's game-facing logic. Links chess::view for SfxLibrary's
# variation policy (issue #78) — no audio device is involved, only the picker.
add_executable(chess_sfx_tests ChessSfxTests.cpp)
target_link_libraries(chess_sfx_tests PRIVATE chess::view)
target_compile_features(chess_sfx_tests PRIVATE cxx_std_20)

add_test(NAME chess_sfx_tests COMMAND chess_sfx_tests)

# #201: chess's first CVars — the board palette. Drives BoardView::Render against a recording
# IRenderer, because the property that matters is that the palette reaches the renderer EVERY FRAME:
# a colour applied only at material-creation time is a constant with extra steps.
add_executable(chess_palette_tests PaletteCvarTests.cpp)
target_link_libraries(chess_palette_tests PRIVATE chess::view)
target_compile_features(chess_palette_tests PRIVATE cxx_std_20)
add_test(NAME chess_palette_tests COMMAND chess_palette_tests)
