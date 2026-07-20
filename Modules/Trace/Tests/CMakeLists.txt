# Host unit tests for lur::trace (issue #101). Plain executable, non-zero on failure,
# registered with CTest. LUR_TRACE=1 comes from EngineFlags (Development) so the macros
# are live; also set it explicitly here so the test is correct even if run standalone.
add_executable(trace_tests TraceTests.cpp)
target_link_libraries(trace_tests PRIVATE lur::trace)
target_compile_features(trace_tests PRIVATE cxx_std_20)
target_compile_definitions(trace_tests PRIVATE LUR_TRACE_ENABLED=1)

add_test(NAME trace_tests COMMAND trace_tests)
