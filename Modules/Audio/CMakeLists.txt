# modules/audio — the audio engine: a pure-C++ wait-free SFX mixer + our own lossless PCM
# codec (LSF1). Both are hardware-free, so this is a plain compiled library that builds and
# unit-tests on the host.
#
# The OS device backends (AAudio / RemoteIO) live under Platform/, ONE COPY PER PLATFORM,
# compiled in only on the platform that has them — the same shape as the renderer's surface
# seam. They were per-app files, which was survivable while exactly one game had sound; moving
# them the moment a second game wants audio prevents a duplication instead of fixing one.
if(ANDROID)
    set(_lur_audio_platform Platform/Android/AudioDevice.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_lur_audio_platform Platform/Ios/AudioDevice.mm)
else()
    # Host/desktop: no backend, and nothing there calls CreateAudioDevice — so it stays
    # UNDEFINED rather than gaining a null implementation nobody asked for. The day a desktop
    # build wants sound, that is when the desktop backend gets written.
    set(_lur_audio_platform "")
endif()
add_library(lur_audio
    Private/PcmCodec.cpp
    Private/Mixer.cpp
    ${_lur_audio_platform})
target_include_directories(lur_audio PUBLIC Public PRIVATE Private)
target_compile_features(lur_audio PUBLIC cxx_std_20)
target_link_libraries(lur_audio PUBLIC lur::core)   # LUR_ASSERT
# The .mm seam is Objective-C++ under ARC (it uses __bridge casts). CMake's Xcode generator
# does not enable ARC by default, and it used to be switched on by the APP target that
# compiled this file — so the flag has to move with the file or the bridge casts fail.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_source_files_properties(Platform/Ios/AudioDevice.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()
add_library(lur::audio ALIAS lur_audio)
