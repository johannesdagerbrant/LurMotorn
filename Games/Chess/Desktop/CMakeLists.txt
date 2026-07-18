# Desktop chess shell (Workbench, roadmap Phase 0.5). A copy-pasted third main that
# drives the shared BoardView on Windows — deliberately NOT extracted into a GameHost
# yet (that's Phase 4). Built only for the desktop Workbench configuration.
if(LUR_DESKTOP AND WIN32)
    # WindowsBleTransport is the dev-rig's third radio (issue #58): a real BLE central
    # on Windows, bridged to a C# WinRT subprocess. It lives in the app build (not the
    # cross-platform Modules/Transport) because it's a dev instrument, Windows-only, and
    # spawns a subprocess — exactly like AndroidBleTransport lives in the Android app.
    add_executable(onlychess_desktop DesktopMain.cpp WindowsBleTransport.cpp)
    target_link_libraries(onlychess_desktop PRIVATE
        chess::view
        chess::core
        lur::render
        lur::net
        lur::transport
        lur::save
        lur::sim
        lur::platform
        lur::input)
    target_compile_features(onlychess_desktop PRIVATE cxx_std_20)
endif()
