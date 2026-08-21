# Host tests for the header-only parts of Modules/Render. The Vulkan backend is not unit-tested
# (no Vulkan SDK on the VS-free host build, and lur_render is an INTERFACE target there) — these
# cover the pixel-layout helpers, which are pure C++ and where an off-by-one is invisible on screen.
add_executable(render_rg8_tests Rg8PackTests.cpp)
target_link_libraries(render_rg8_tests PRIVATE lur_render)
target_compile_features(render_rg8_tests PRIVATE cxx_std_20)
add_test(NAME render_rg8_tests COMMAND render_rg8_tests)

# #201: rollback correction smoothing, promoted out of Rps::GameView. Its headline test is the one
# that would have caught the shipped "swinging arc" bug — a slot whose OCCUPANT changed must snap, not
# ease in from its predecessor — which single-player could never reproduce because nothing rolls back.
add_executable(render_smoother_tests CorrectionSmootherTests.cpp)
target_link_libraries(render_smoother_tests PRIVATE lur_render)
target_compile_features(render_smoother_tests PRIVATE cxx_std_20)
add_test(NAME render_smoother_tests COMMAND render_smoother_tests)

# #201: ColorMath moved here from Modules/DevGui and absorbed RPS's own HSV, which was written in a
# different convention. The headline test reproduces the old RPS formula and asserts the two agree
# across the hue circle — without that, consolidating them could shift the team palette invisibly.
add_executable(render_colormath_tests ColorMathTests.cpp)
target_link_libraries(render_colormath_tests PRIVATE lur_render)
target_compile_features(render_colormath_tests PRIVATE cxx_std_20)
add_test(NAME render_colormath_tests COMMAND render_colormath_tests)

# #201: the unit-rect mesh builders promoted out of Rps::GameView, with geometry split from
# CreateMesh so it is testable with no GPU. The headline test is that the disc is a triangle
# LIST: a fan renders correctly on Android and is outside the portability subset MoltenVK runs,
# so getting it wrong fails only on the iPhone.
add_executable(render_mesh2d_tests Mesh2DTests.cpp)
target_link_libraries(render_mesh2d_tests PRIVATE lur_render)
target_compile_features(render_mesh2d_tests PRIVATE cxx_std_20)
add_test(NAME render_mesh2d_tests COMMAND render_mesh2d_tests)
