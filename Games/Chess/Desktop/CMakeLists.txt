# Desktop chess shell (Workbench, roadmap Phase 0.5). A copy-pasted third main that
# drives the shared BoardView on Windows — deliberately NOT extracted into a GameHost
# yet (that's Phase 4). Built only for the desktop Workbench configuration.
if(LUR_DESKTOP AND WIN32)
    add_executable(onlychess_desktop DesktopMain.cpp)
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
