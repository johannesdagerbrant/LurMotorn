# modules/core — engine-wide primitives with no dependencies (LUR_ASSERT, later
# Guid, Log). Header-only, pure C++; every other module may depend on it.
add_library(lur_core INTERFACE)
target_include_directories(lur_core INTERFACE Public)
target_compile_features(lur_core INTERFACE cxx_std_20)
add_library(lur::core ALIAS lur_core)
