# Host unit tests for Modules/Core (the Lur::Log seam; LUR_ASSERT is header-only).
add_executable(core_tests CoreTests.cpp)
# lur::sim is for the TEST only (Fixed): CVar<T>::Nudge picks its step from T, and Fixed is
# the type the balance knobs actually use, so leaving it untested would leave the branch that
# matters uncovered. Modules/Core itself does not depend on Sim.
target_link_libraries(core_tests PRIVATE lur::core lur::sim lur::render)
target_compile_features(core_tests PRIVATE cxx_std_20)
add_test(NAME core_tests COMMAND core_tests)

# #201: the assistant remote-control command line promoted out of Rps::AgentControl. NOT behind
# LUR_AGENT on purpose — the parser cannot drive anything, and compiling it always is what keeps the
# grammar (and the sequence-number idempotence rule) in the ordinary host suite.
add_executable(agent_parser_tests AgentCommandParserTests.cpp)
target_link_libraries(agent_parser_tests PRIVATE lur::core)
target_compile_features(agent_parser_tests PRIVATE cxx_std_20)
add_test(NAME agent_parser_tests COMMAND agent_parser_tests)

# CVar SHIPPING zero-overhead proof (spec §1/§7, issue #111): reading a CVar must emit
# machine code identical to reading the old constexpr. The script compiles a probe TU
# twice at -O2 -DLUR_SHIPPING=1 and diffs Hot()'s disassembly. Host/Windows-only (needs
# g++ + objdump on PATH, as the host build already requires).
if(WIN32)
    add_test(NAME cvar_zero_overhead
             COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                     -File ${CMAKE_SOURCE_DIR}/scripts/check-cvar-zero-overhead.ps1)

    # Build fingerprint (#164): it must track the tree at BUILD time, which a cached configure-time
    # value could not. Drives cmake/BuildFingerprint.cmake against a throwaway git repo — the only
    # way to assert the commit / dirty / config transitions without three full builds.
    add_test(NAME build_fingerprint
             COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                     -File ${CMAKE_SOURCE_DIR}/scripts/check-build-fingerprint.ps1)
endif()
