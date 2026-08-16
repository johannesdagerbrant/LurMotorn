# The screen<->world arithmetic every RPS main needs (issue #43, section D). Links rps::core only —
# these are free functions over the tunables, with no view, no renderer and no platform, which is
# precisely why having three copies of them was so easy and so quiet.
add_executable(rps_view_metrics_tests ViewMetricsTests.cpp)
target_link_libraries(rps_view_metrics_tests PRIVATE rps::core)
target_include_directories(rps_view_metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Public)
target_compile_features(rps_view_metrics_tests PRIVATE cxx_std_20)
add_test(NAME rps_view_metrics_tests COMMAND rps_view_metrics_tests)
