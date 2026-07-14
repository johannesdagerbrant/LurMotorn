# modules/text — engine text/glyph rendering. Today: the cooked MSDF font assets
# (scripts/gen-font.ps1 -> Private/Cooked/FontAtlas_*.h), embedded as raw bytes so no
# font/image decoder ships in the app. The runtime Font asset, FontRegistry, and layout
# land next (#25); the render-side MSDF pipeline is #26. Pure C++ — compiles verbatim on
# host / Android (NDK) / iOS. Games depend on it; it never depends on a game.
add_library(lur_text Private/BuiltinFonts.cpp)
target_include_directories(lur_text PUBLIC Public PRIVATE Private)
target_compile_features(lur_text PUBLIC cxx_std_20)
add_library(lur::text ALIAS lur_text)
