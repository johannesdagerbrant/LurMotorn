# Modules/DevGui — the dev-GUI: the CONSOLE plus the widget geometry it is built from.
#
# Two layers, and the split is deliberate:
#
#   * The widgets (Numpad, ColorPicker, Slider, FlatList, CategoryTree, Popover, DevTheme) are
#     header-only pure logic + shared geometry. They compute rects and answer hit-tests; they never
#     hold a renderer, so the rect drawn and the rect hit-tested are the same call.
#   * Console (#201) is the one legitimate HOST of those widgets, and it does draw. It was ~650 lines
#     inside Rps::GameView with a 120-line partial reimplementation in chess; game 3 is a certain
#     third consumer, so it lives here.
#
# Console.cpp is one big `#if !LUR_SHIPPING`, so a shipping build gets an empty object and no console
# symbols exist at all. Keeping the library unconditional means game targets link it unconditionally
# and only their CALLS are guarded.
add_library(lur_devgui Private/Console.cpp)
target_include_directories(lur_devgui PUBLIC Public)
target_compile_features(lur_devgui PUBLIC cxx_std_20)
# lur::core for the CVar + DevCommand registries the console lists; lur::hud/lur::text for its own
# text field and font (it does not borrow the game's); lur::render for the draw and for Color.
target_link_libraries(lur_devgui PUBLIC lur::core lur::render lur::text lur::hud)
add_library(lur::devgui ALIAS lur_devgui)
