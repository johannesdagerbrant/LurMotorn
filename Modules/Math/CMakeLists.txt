# Modules/Math — vector / matrix / quaternion math for rendering and scene
# transforms. Header-only, pure C++.
#
# NOTE: this is render/scene math in FLOATS — deliberately distinct from the
# deterministic fixed-point in Modules/Sim. Gameplay simulation state must stay
# deterministic (Fixed); rendering may use floats freely.
add_library(lur_math INTERFACE)
target_include_directories(lur_math INTERFACE Public)
target_compile_features(lur_math INTERFACE cxx_std_20)
add_library(lur::math ALIAS lur_math)
