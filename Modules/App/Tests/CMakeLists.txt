# Host unit tests for Lur::App (issue #43). Links NO game: GameHost's choreography is engine
# behaviour, and proving that on the host is the whole reason it moved out of the two mains.
add_executable(app_tests AppTests.cpp)
target_link_libraries(app_tests PRIVATE lur::app lur::transport)
target_compile_features(app_tests PRIVATE cxx_std_20)

add_test(NAME app_tests COMMAND app_tests)

# The platform<->renderer handshake, and the one decision that differs between the two thread
# topologies (issue #43, section C). Its own binary because it links no transport and no disk: it is
# sixteen atomics out of RpsMain.mm with the rule that governs them made explicit. Threads::Threads
# for the one test that runs a real frame loop — the protocol's whole purpose is a cross-thread
# handoff, which a single-threaded test cannot demonstrate terminates.
find_package(Threads REQUIRED)
add_executable(render_handshake_tests RenderHandshakeTests.cpp)
target_link_libraries(render_handshake_tests PRIVATE lur::app Threads::Threads)
target_compile_features(render_handshake_tests PRIVATE cxx_std_20)
add_test(NAME render_handshake_tests COMMAND render_handshake_tests)
