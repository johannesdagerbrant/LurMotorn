# Modules/DevGui — the in-house dev-GUI widget utilities (spec Addendum A). First member:
# the Numpad (tap-driven numeric entry — the portable answer after the #118 soft-keyboard
# no-go). Header-only pure logic + shared geometry; the render happens through IRenderer in
# whatever surface hosts it. Dev-only: the root excludes this subdir from a shipping build.
add_library(lur_devgui INTERFACE)
target_include_directories(lur_devgui INTERFACE Public)
target_compile_features(lur_devgui INTERFACE cxx_std_20)
add_library(lur::devgui ALIAS lur_devgui)
