# Host tests for the header-only parts of Modules/Render. The Vulkan backend is not unit-tested
# (no Vulkan SDK on the VS-free host build, and lur_render is an INTERFACE target there) — these
# cover the pixel-layout helpers, which are pure C++ and where an off-by-one is invisible on screen.
add_executable(render_rg8_tests Rg8PackTests.cpp)
target_link_libraries(render_rg8_tests PRIVATE lur_render)
target_compile_features(render_rg8_tests PRIVATE cxx_std_20)
add_test(NAME render_rg8_tests COMMAND render_rg8_tests)
