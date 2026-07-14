# modules/text — engine text/glyph rendering: cooked MSDF font assets
# (scripts/gen-font.ps1 -> Private/Cooked/FontAtlas_*.h, embedded as raw bytes so no
# font/image decoder ships in the app), the runtime Font + FontRegistry, and resolution-
# independent layout (greedy word-wrap, measure, overflow). Depends on lur_render only for
# the atlas TextureHandle type (header-only on host); the render-side MSDF pipeline is #26.
# Pure C++ — compiles verbatim on host / Android (NDK) / iOS. Games depend on it; it never
# depends on a game.
add_library(lur_text
    Private/BuiltinFonts.cpp
    Private/Font.cpp
    Private/FontRegistry.cpp
    Private/TextLayout.cpp)
target_include_directories(lur_text PUBLIC Public PRIVATE Private)
target_compile_features(lur_text PUBLIC cxx_std_20)
target_link_libraries(lur_text PUBLIC lur_render)
add_library(lur::text ALIAS lur_text)
