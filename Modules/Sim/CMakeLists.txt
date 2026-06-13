# modules/sim — deterministic simulation primitives. Header-only, pure C++.
add_library(lur_sim INTERFACE)
target_include_directories(lur_sim INTERFACE Public)
target_compile_features(lur_sim INTERFACE cxx_std_20)
add_library(lur::sim ALIAS lur_sim)
