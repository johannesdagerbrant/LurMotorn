# Modules/Render — the renderer interface. Header-only and pure C++.
# One GPU backend (Vulkan; via MoltenVK on iOS) is compiled by each app build,
# since it needs the platform's Vulkan SDK + window handle.
add_library(lur_render INTERFACE)
target_include_directories(lur_render INTERFACE Public)
target_compile_features(lur_render INTERFACE cxx_std_20)
target_link_libraries(lur_render INTERFACE lur_math)
add_library(lur::render ALIAS lur_render)
