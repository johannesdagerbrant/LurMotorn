# Host unit tests for the audio mixer + LSF1 lossless codec. Dependency-free: a plain
# executable that returns non-zero on failure, registered with CTest.
add_executable(audio_tests AudioTests.cpp)
target_link_libraries(audio_tests PRIVATE lur::audio)
target_compile_features(audio_tests PRIVATE cxx_std_20)
add_test(NAME audio_tests COMMAND audio_tests)
