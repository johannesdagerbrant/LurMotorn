# EngineFlags — shared compiler discipline (Review #2 §3.5 / §5).
#
# Applied only when LurMotorn is the TOP-LEVEL project — i.e. the host correctness
# loop driven by build.ps1, which compiles every shared module + the chess core +
# the unit tests. That build is the gate that keeps the shared code warning-clean.
#
# The Android/iOS apps pull this tree in via add_subdirectory, so they do NOT inherit
# -Werror here: their platform translation units (the Vulkan backend, the BLE
# backends) are not built on the host and can't be verified from here, and a warning
# there must not break an unrelated core change. Extending strict flags to the app
# builds is a deliberate later step that needs on-device verification.
#
# Exceptions / RTTI decision (recorded here on purpose):
#   * RTTI is OFF now — nothing uses dynamic_cast / typeid (verified).
#   * Exceptions stay ON for now: Lur::Save leans on std::filesystem's throwing API.
#     -fno-exceptions is deferred to the std::filesystem replacement (Review #2 §3.1,
#     roadmap Phase 5); exceptions go off in the same change.
if(PROJECT_IS_TOP_LEVEL)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Werror -fno-rtti)
    elseif(MSVC)
        add_compile_options(/W4 /WX /GR-)
    endif()
endif()
