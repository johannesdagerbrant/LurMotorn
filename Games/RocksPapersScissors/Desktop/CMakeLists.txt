# Desktop RPS shell (Phase 1, slice 0). A copy-pasted second main (like chess's) that
# drives the shared SimRunner + GameView on Windows with a real Win32 window + Vulkan
# surface — the fast iteration loop, and the first thing that runs on screen. Built
# only for the desktop Workbench configuration.
if(LUR_DESKTOP AND WIN32)
    add_executable(onlyrps_desktop DesktopMain.cpp)
    target_link_libraries(onlyrps_desktop PRIVATE
        rps::view
        rps::runtime
        rps::core
        lur::render
        lur::sim
        lur::platform
        lur::input)
    target_compile_features(onlyrps_desktop PRIVATE cxx_std_20)
endif()
