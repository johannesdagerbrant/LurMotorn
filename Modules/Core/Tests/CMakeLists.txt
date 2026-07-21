# Host unit tests for Modules/Core (the Lur::Log seam; LUR_ASSERT is header-only).
add_executable(core_tests CoreTests.cpp)
target_link_libraries(core_tests PRIVATE lur::core)
target_compile_features(core_tests PRIVATE cxx_std_20)
add_test(NAME core_tests COMMAND core_tests)

# CVar SHIPPING zero-overhead proof (spec §1/§7, issue #111): reading a CVar must emit
# machine code identical to reading the old constexpr. The script compiles a probe TU
# twice at -O2 -DLUR_SHIPPING=1 and diffs Hot()'s disassembly. Host/Windows-only (needs
# g++ + objdump on PATH, as the host build already requires).
if(WIN32)
    add_test(NAME cvar_zero_overhead
             COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                     -File ${CMAKE_SOURCE_DIR}/scripts/check-cvar-zero-overhead.ps1)
endif()
