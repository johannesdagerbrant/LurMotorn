# modules/platform — desktop window + input seam for the Workbench build (Phase 0.5).
# Compiled only for the desktop (LUR_DESKTOP); the phones get their window from
# NativeActivity / UIKit, so this module has no place there.
if(LUR_DESKTOP AND WIN32)
    add_library(lur_platform STATIC Private/Windows/Win32Window.cpp)
    target_include_directories(lur_platform PUBLIC Public)
    target_compile_features(lur_platform PUBLIC cxx_std_20)
    target_link_libraries(lur_platform PUBLIC lur_input PRIVATE user32 gdi32)
    add_library(lur::platform ALIAS lur_platform)
endif()
