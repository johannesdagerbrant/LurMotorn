# modules/hud — small, reusable in-game overlay widgets (heads-up display).
# Sits above Render only: it draws engine state the app hands it (already-resolved
# strings and counters) via the IRenderer, so it knows nothing of Net or any game.
add_library(lur_hud Private/TextField.cpp Private/Dropdown.cpp Private/DebugOverlay.cpp)
target_include_directories(lur_hud PUBLIC Public)
target_compile_features(lur_hud PUBLIC cxx_std_20)
target_link_libraries(lur_hud PUBLIC lur_render lur_text)
add_library(lur::hud ALIAS lur_hud)
