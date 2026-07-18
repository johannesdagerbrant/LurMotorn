# EngineFlags — shared compiler discipline + build-configuration macros
# (Review #2 §3.5 / §5; issue #65).
#
# ── Build configurations (Unreal-style, the human-facing dial) ───────────────
# One ordinal ladder, each rung a strict SUPERSET of the one below:
#
#   SHIPPING     retail binary            no tooling, no asserts,      optimized
#   DEVELOPMENT  fast dev + tooling       tooling + asserts,           optimized
#   DEBUGGING    everything on            tooling + asserts + slow,    -O0 -g
#
# Select with -DLUR_CONFIG=Shipping|Development|Debugging (default Development —
# the everyday dev loop; a release pipeline selects Shipping explicitly).
#
# ── Capability macros (what CODE tests — never the config name) ──────────────
# Each config DERIVES these; gate features on the capability so a future build
# that doesn't fit the ladder (e.g. profiling = shipping + stats) never forces a
# call-site rewrite. This mirrors Unreal (named configs, but code checks
# WITH_EDITOR / DO_CHECK / !UE_BUILD_SHIPPING) and Casey's HANDMADE_INTERNAL /
# HANDMADE_SLOW split.
#
#   LUR_SHIPPING   1 = the retail binary (quiet guards, nothing dev-only)
#   LUR_INTERNAL   1 = developer-only code compiled in (tooling, bots, PlayMove,
#                      the soak/autoplayer) — MUST be 0 in a shipped build
#   LUR_ASSERTS    1 = LUR_ASSERT is deafening (log + trap); 0 = quiet guard.
#                      DECOUPLED from NDEBUG on purpose, so an *optimized*
#                      DEVELOPMENT build (the overnight soak) still traps.
#   LUR_SLOW       1 = expensive validation welcome (full-board re-checks, audits)
#
# These are defined for the whole engine (all modules + chess) via
# add_compile_definitions below. App targets that live OUTSIDE this tree's scope
# (the Android/iOS mains, defined by the app's own top-level project) can't be
# reached from here — they read the LUR_* cache vars this file sets and apply
# them to their own target. See Games/Chess/Android/app/src/main/cpp/CMakeLists.txt.

set(LUR_CONFIG "Development" CACHE STRING "Build configuration: Shipping | Development | Debugging")
set_property(CACHE LUR_CONFIG PROPERTY STRINGS Shipping Development Debugging)

if(LUR_CONFIG STREQUAL "Shipping")
    set(_lur_shipping 1)
    set(_lur_internal 0)
    set(_lur_asserts 0)
    set(_lur_slow 0)
elseif(LUR_CONFIG STREQUAL "Debugging")
    set(_lur_shipping 0)
    set(_lur_internal 1)
    set(_lur_asserts 1)
    set(_lur_slow 1)
else()  # Development (default)
    if(NOT LUR_CONFIG STREQUAL "Development")
        message(WARNING "Unknown LUR_CONFIG='${LUR_CONFIG}', falling back to Development")
    endif()
    set(_lur_shipping 0)
    set(_lur_internal 1)
    set(_lur_asserts 1)
    set(_lur_slow 0)
endif()

# Publish the derived values as cache vars so out-of-tree app targets (Android/iOS
# mains) can apply the same macros to themselves after add_subdirectory(root).
set(LUR_SHIPPING ${_lur_shipping} CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_INTERNAL ${_lur_internal} CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_ASSERTS  ${_lur_asserts}  CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_SLOW     ${_lur_slow}     CACHE INTERNAL "derived from LUR_CONFIG")

# Apply to this scope + every target added under it (all engine modules + chess,
# and the Desktop app — all root subdirs added after this include).
add_compile_definitions(
    LUR_SHIPPING=${_lur_shipping}
    LUR_INTERNAL=${_lur_internal}
    LUR_ASSERTS=${_lur_asserts}
    LUR_SLOW=${_lur_slow})

message(STATUS "LurMotorn config: ${LUR_CONFIG} "
        "(SHIPPING=${_lur_shipping} INTERNAL=${_lur_internal} ASSERTS=${_lur_asserts} SLOW=${_lur_slow})")

# ── Compiler discipline ──────────────────────────────────────────────────────
# Applied only when LurMotorn is the TOP-LEVEL project — the host correctness loop
# driven by build.ps1, which compiles every shared module + the chess core + the
# unit tests. That build is the gate that keeps the shared code warning-clean.
#
# The Android/iOS apps pull this tree in via add_subdirectory, so they do NOT
# inherit -Werror here: their platform translation units (Vulkan/BLE backends) are
# not built on the host and can't be verified from here, and a warning there must
# not break an unrelated core change. Extending strict flags to the app builds is a
# deliberate later step that needs on-device verification.
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
