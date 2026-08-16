# Desktop chess shell (Workbench, roadmap Phase 0.5). Drives the shared BoardView on
# Windows; the session + persistence choreography is lur::app's GameHost since #43, so
# this is platform plus the workbench's own two-peer arrangement. Built only for the
# desktop Workbench configuration.
if(LUR_DESKTOP AND WIN32)
    # WindowsBleTransport is the dev-rig's third radio (issue #58): a real BLE central on
    # Windows, bridged to a C# WinRT subprocess. It lives in Modules/Transport/Platform/Windows
    # now (#42), beside the Android and iOS radios — the argument that it belonged in the app
    # because it is a dev instrument did not survive the second copy: chess and RPS each had
    # one, and they had already drifted.
    add_executable(onlychess_desktop DesktopMain.cpp
        ${CMAKE_SOURCE_DIR}/Modules/Transport/Platform/Windows/WindowsBleTransport.cpp)
    target_include_directories(onlychess_desktop PRIVATE
        ${CMAKE_SOURCE_DIR}/Modules/Transport/Platform/Windows)
    # The BLE identity is per-app and REQUIRED (BleProtocol.h #errors without it), the same
    # rule the Android and iOS targets follow. Stating it here is what lets the shared
    # Windows radio read BleServiceUuid instead of taking a literal at its call site.
    target_compile_definitions(onlychess_desktop PRIVATE
        "LUR_BLE_SERVICE_UUID=\"4C55524D-4F54-4F52-4E00-5472616E7370\"")
    target_link_libraries(onlychess_desktop PRIVATE
        chess::view
        chess::core
        lur::render
        lur::app    # #43: GameHost — Store/device id/SyncManager/Session, one copy
        lur::net
        lur::transport
        lur::save
        lur::sim
        lur::platform
        lur::input)
    target_compile_features(onlychess_desktop PRIVATE cxx_std_20)
endif()
