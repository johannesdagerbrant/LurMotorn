# The screen<->world arithmetic every RPS main needs (issue #43, section D). Links rps::core only —
# these are free functions over the tunables, with no view, no renderer and no platform, which is
# precisely why having three copies of them was so easy and so quiet.
add_executable(rps_view_metrics_tests ViewMetricsTests.cpp)
target_link_libraries(rps_view_metrics_tests PRIVATE rps::core)
target_include_directories(rps_view_metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Public)
target_compile_features(rps_view_metrics_tests PRIVATE cxx_std_20)
add_test(NAME rps_view_metrics_tests COMMAND rps_view_metrics_tests)

# What a touch MEANS in RPS (issue #43, section D). This dispatch existed FOUR times — Android, iOS
# and both halves of DesktopMain.cpp — and had drifted in six separate ways, none of which any test
# could see because it lived in platform mains. Links rps::view for the real GameView: a fake would
# only prove the router calls the methods it was written to call.
add_executable(rps_touch_router_tests TouchRouterTests.cpp)
target_link_libraries(rps_touch_router_tests PRIVATE rps::view)
target_compile_features(rps_touch_router_tests PRIVATE cxx_std_20)
add_test(NAME rps_touch_router_tests COMMAND rps_touch_router_tests)
