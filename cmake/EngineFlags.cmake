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
#   LUR_TRACE      1 = Lur::Trace CPU scopes compiled in (LUR_TRACE_SCOPE/_LATENCY);
#                      on in Development/Debugging, off in Shipping. A pure observer
#                      (never touches sim state) — issue #101. The off-ladder
#                      "profiling = shipping + stats" build is -DLUR_CONFIG=Shipping
#                      -DLUR_TRACE=1.
#   LUR_AGENT      1 = AGENT-ONLY instrumentation: remote control and automatic
#                      capture that exist so an assistant can drive and observe the
#                      app without hands. OFF IN EVERY CONFIG, INCLUDING DEVELOPMENT
#                      — opt in with -DLUR_AGENT=ON, and never in a build handed to a
#                      player.
#
#                      Why this exists as its own axis (playtest feedback 2026-07-25):
#                      LUR_INTERNAL is NOT a sufficient gate for it, because the build
#                      a player actually plays IS Development — that is where the dev
#                      console and the tunable CVars live, and those are FOR the player.
#                      An affordance that lets the harness open the console, or that
#                      writes capture files during someone's session, is a different
#                      category: it can override real input and make the game feel like
#                      it is being driven from outside. So it gets a switch that is off
#                      unless explicitly asked for.
#
#                      Rule: if it was built for the ASSISTANT rather than for the
#                      player or the developer at the keyboard, it is #if LUR_AGENT.
#
# These are defined for the whole engine (all modules + chess) via
# add_compile_definitions below. App targets that live OUTSIDE this tree's scope
# (the Android/iOS mains, defined by the app's own top-level project) can't be
# reached from here — they read the LUR_* cache vars this file sets and apply
# them to their own target. See Games/Chess/Android/app/src/main/cpp/CMakeLists.txt.

set(LUR_CONFIG "Development" CACHE STRING "Build configuration: Shipping | Development | Debugging")
set_property(CACHE LUR_CONFIG PROPERTY STRINGS Shipping Development Debugging)

# LUR_FAST forces an unoptimized (-O0) build regardless of LUR_CONFIG — for the
# host unit-test loop (build.ps1), which wants fast COMPILES, not fast code
# (correctness, not speed). See the optimization-axis block below.
option(LUR_FAST "Force -O0 for a fast compile loop, independent of LUR_CONFIG" OFF)

# Agent-only instrumentation (see the header comment). Deliberately NOT derived from
# LUR_CONFIG: it must stay off in Development, which is the configuration a player plays.
# A build handed over for play is built without this, so the code is absent, not merely idle.
option(LUR_AGENT "Compile in assistant-only remote control / capture (never for a player)" OFF)

if(LUR_CONFIG STREQUAL "Shipping")
    set(_lur_shipping 1)
    set(_lur_internal 0)
    set(_lur_asserts 0)
    set(_lur_slow 0)
    set(_lur_trace 0)
elseif(LUR_CONFIG STREQUAL "Debugging")
    set(_lur_shipping 0)
    set(_lur_internal 1)
    set(_lur_asserts 1)
    set(_lur_slow 1)
    set(_lur_trace 1)
else()  # Development (default)
    if(NOT LUR_CONFIG STREQUAL "Development")
        message(WARNING "Unknown LUR_CONFIG='${LUR_CONFIG}', falling back to Development")
    endif()
    set(_lur_shipping 0)
    set(_lur_internal 1)
    set(_lur_asserts 1)
    set(_lur_slow 0)
    set(_lur_trace 1)
endif()

# A shipping binary can opt into tracing (the "profiling = shipping + stats" build)
# without any other tooling: -DLUR_CONFIG=Shipping -DLUR_TRACE=1.
if(DEFINED LUR_TRACE)
    set(_lur_trace ${LUR_TRACE})
endif()

# Agent instrumentation is a hard 0 unless explicitly requested, and a Shipping build can never
# carry it whatever was asked for — belt and braces, since the whole point is that it cannot leak.
if(LUR_AGENT AND NOT _lur_shipping)
    set(_lur_agent 1)
else()
    set(_lur_agent 0)
endif()

# Publish the derived values as cache vars so out-of-tree app targets (Android/iOS
# mains) can apply the same macros to themselves after add_subdirectory(root).
set(LUR_SHIPPING ${_lur_shipping} CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_INTERNAL ${_lur_internal} CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_ASSERTS  ${_lur_asserts}  CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_SLOW     ${_lur_slow}     CACHE INTERNAL "derived from LUR_CONFIG")
set(LUR_TRACE    ${_lur_trace}    CACHE INTERNAL "derived from LUR_CONFIG (issue #101)")
set(LUR_AGENT_ON ${_lur_agent}    CACHE INTERNAL "assistant-only instrumentation (opt-in, never for a player)")

# Apply to this scope + every target added under it (all engine modules + chess,
# and the Desktop app — all root subdirs added after this include).
add_compile_definitions(
    LUR_SHIPPING=${_lur_shipping}
    LUR_INTERNAL=${_lur_internal}
    LUR_ASSERTS=${_lur_asserts}
    LUR_SLOW=${_lur_slow}
    LUR_TRACE=${_lur_trace}
    LUR_AGENT=${_lur_agent})

message(STATUS "LurMotorn config: ${LUR_CONFIG} "
        "(SHIPPING=${_lur_shipping} INTERNAL=${_lur_internal} ASSERTS=${_lur_asserts} "
        "SLOW=${_lur_slow} TRACE=${_lur_trace} AGENT=${_lur_agent})")
if(_lur_agent)
    message(WARNING "LUR_AGENT=1: assistant-only remote control/capture is COMPILED IN. "
            "This build must not be handed to a player.")
endif()

# ── Build fingerprint (issue #112, Addendum C.3) ─────────────────────────────
# git commit + dirty flag, baked in so two peers can refuse to connect on a build
# mismatch (the proactive form of the reactive anchor-hash desync alarm). Snapshotted at
# configure time; a source edit without reconfigure is still caught by the anchor hash.
find_package(Git QUIET)
set(_lur_fp "no-git")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _lur_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _lur_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_lur_sha STREQUAL "")
        set(_lur_fp "no-git")
    elseif(_lur_dirty STREQUAL "")
        set(_lur_fp "${_lur_sha}")
    else()
        set(_lur_fp "${_lur_sha}-dirty")
    endif()
endif()
# Include the config in the fingerprint: same commit, different LUR_CONFIG => different sim.
set(LUR_BUILD_FP "${_lur_fp}+${LUR_CONFIG}" CACHE INTERNAL "build fingerprint (#112)")
add_compile_definitions(LUR_BUILD_FP=\"${LUR_BUILD_FP}\")
message(STATUS "LurMotorn build fingerprint: ${LUR_BUILD_FP}")

# ── Optimization axis — the ladder's "Opt" column, coupled to LUR_CONFIG ──────
# The macros above are only HALF the ladder. The other half — real optimization
# (-O0 vs -O2) — lives in CMAKE_BUILD_TYPE, a SEPARATE dial. Historically every
# build driver hardcoded CMAKE_BUILD_TYPE=Debug and none set LUR_CONFIG, so the
# documented "Development = optimized" rung was never actually produced: the phone
# AND the desktop ran -O0 (issue #89). Bind the two here so LUR_CONFIG is the one
# dial a human turns; a driver that wants -O0 for compile speed passes LUR_FAST.
#
# Single-config generators only (Ninja: host + Android, where CMAKE_BUILD_TYPE
# governs -O). Multi-config generators (Xcode/iOS, Visual Studio) ignore it and
# choose optimization per-build — there the app project owns the Opt column and
# must be kept in sync with LUR_CONFIG by hand.
get_property(_lur_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _lur_multi_config)
    if(LUR_FAST OR LUR_CONFIG STREQUAL "Debugging")
        set(_lur_build_type Debug)            # -O0 -g
    else()                                    # Development, Shipping
        set(_lur_build_type RelWithDebInfo)   # -O2 -g (asserts stay on via LUR_ASSERTS)
    endif()
    set(CMAKE_BUILD_TYPE "${_lur_build_type}" CACHE STRING
        "Native optimization, derived from LUR_CONFIG by EngineFlags (LUR_FAST=ON forces -O0)" FORCE)
    message(STATUS "LurMotorn opt: CMAKE_BUILD_TYPE=${_lur_build_type} "
            "(from LUR_CONFIG=${LUR_CONFIG}, LUR_FAST=${LUR_FAST})")
endif()

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

# Static libstdc++/libgcc on MinGW (issue #152). Our host binaries are built by the WinLibs
# g++, but a Windows PATH usually carries OTHER mingw runtimes — Git for Windows ships its own
# libstdc++-6.dll — and the loader takes the first one it finds. Loading a foreign libstdc++
# into our binary is an ABI mismatch that CRASHES: the headless AI harness segfaulted inside
# std::ifstream's constructor when launched from Git Bash, printing nothing, which read for a
# while as "the harness's logging is broken" (it wasn't; it died before the first line).
# Linking the runtimes statically makes every host binary shell-agnostic and un-hijackable.
if(PROJECT_IS_TOP_LEVEL AND MINGW)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()
