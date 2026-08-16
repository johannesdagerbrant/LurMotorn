# Desktop RPS shell (Phase 1, slice 0). A copy-pasted second main (like chess's) that
# drives the shared SimRunner + GameView on Windows with a real Win32 window + Vulkan
# surface — the fast iteration loop, and the first thing that runs on screen. Built
# only for the desktop Workbench configuration.
if(LUR_DESKTOP AND WIN32)
    # WindowsBleTransport makes the PC a real BLE opponent to the phone so TraceAndroid can
    # capture a live match. Shared from Modules/Transport/Platform/Windows since #42 — this was
    # the copy that carried the per-game service UUID, and chess's copy did not.
    add_executable(onlyrps_desktop DesktopMain.cpp
        ${CMAKE_SOURCE_DIR}/Modules/Transport/Platform/Windows/WindowsBleTransport.cpp)
    target_include_directories(onlyrps_desktop PRIVATE
        ${CMAKE_SOURCE_DIR}/Modules/Transport/Platform/Windows)
    # The BLE identity is per-app and REQUIRED (BleProtocol.h #errors without it), the same
    # rule the Android and iOS targets follow. Stating it here is what lets the shared
    # Windows radio read BleServiceUuid instead of taking a literal at its call site.
    target_compile_definitions(onlyrps_desktop PRIVATE
        "LUR_BLE_SERVICE_UUID=\"4C55524D-4F54-4F52-4E00-5472616E7371\"")
    target_link_libraries(onlyrps_desktop PRIVATE
        rps::view
        rps::runtime
        rps::net
        rps::core
        lur::render
        lur::net
        lur::transport
        lur::sim
        lur::platform
        lur::input)
    target_compile_features(onlyrps_desktop PRIVATE cxx_std_20)
endif()
