# modules/audio — the audio engine: a pure-C++ wait-free SFX mixer + our own lossless PCM
# codec (LSF1). Both are hardware-free, so this is a plain compiled library that builds and
# unit-tests on the host. The OS device backends (AAudio / RemoteIO) are NOT here — they are
# app-build seam files (Games/Chess/{Android,iOS}) that implement CreateAudioDevice(), the
# same split the transport and renderer use.
add_library(lur_audio
    Private/PcmCodec.cpp
    Private/Mixer.cpp)
target_include_directories(lur_audio PUBLIC Public PRIVATE Private)
target_compile_features(lur_audio PUBLIC cxx_std_20)
target_link_libraries(lur_audio PUBLIC lur::core)   # LUR_ASSERT
add_library(lur::audio ALIAS lur_audio)
