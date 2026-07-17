# modules/hud — small, reusable in-game overlay widgets (heads-up display).
# Sits above Render + Net: it visualises engine state (e.g. link status) via the
# IRenderer, so it depends on both. Games depend on it; it never depends on a game.
add_library(lur_hud Private/LinkStatusBar.cpp Private/TextField.cpp Private/Dropdown.cpp
                    Private/DebugOverlay.cpp)
target_include_directories(lur_hud PUBLIC Public)
target_compile_features(lur_hud PUBLIC cxx_std_20)
target_link_libraries(lur_hud PUBLIC lur_render lur_net lur_text)
add_library(lur::hud ALIAS lur_hud)
