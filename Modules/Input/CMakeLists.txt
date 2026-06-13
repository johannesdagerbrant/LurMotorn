# modules/input — touch input plumbing. Header-only, pure C++.
add_library(lur_input INTERFACE)
target_include_directories(lur_input INTERFACE Public)
target_compile_features(lur_input INTERFACE cxx_std_20)
add_library(lur::input ALIAS lur_input)
