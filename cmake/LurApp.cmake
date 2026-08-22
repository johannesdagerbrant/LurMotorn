# LurApp — everything an out-of-tree APP target needs from the engine, in one call.
#
# ── Why this exists ──────────────────────────────────────────────────────────
# The Android and iOS mains are defined by the app's OWN top-level project and pull the engine in
# with add_subdirectory. That puts them outside the reach of the engine tree's
# add_compile_definitions, so each one re-applied the derived LUR_* capability macros by hand — four
# copies of the same block, hand-maintained.
#
# It failed the way hand-copied things fail: LUR_AGENT was simply MISSING from
# Games/RocksPapersScissors/iOS/CMakeLists.txt, so the iPhone app compiled the agent harness out even
# when the tree was configured with -DLUR_AGENT=ON — on the one platform with no touch injection at
# all, i.e. the half that most needs remote control. Nothing failed to build; the feature was just
# absent. A new LUR_* macro today means editing four files and noticing all four.
#
# So: ONE function. Adding a capability macro is a one-line change here, and a game that forgets to
# call it does not build.
#
# ── The two per-app values with NO default ───────────────────────────────────
# LOG_TAG and BLE_UUID are REQUIRED, and deliberately have no fallback (CLAUDE.md). The service UUID
# once defaulted to chess's, so a forgetful game inherited chess's IDENTITY — which does not fail
# loudly, it fails as two phones that never see each other. This function hard-errors instead.

include_guard(GLOBAL)

# lur_configure_app_target(<target> LOG_TAG <tag> BLE_UUID <uuid>)
#
# Applies the derived capability macros, the app's log tag and its BLE service UUID, and — on a
# multi-config generator — pins optimization to what LUR_CONFIG asked for.
function(lur_configure_app_target TARGET)
    cmake_parse_arguments(LAPP "" "LOG_TAG;BLE_UUID" "" ${ARGN})

    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "lur_configure_app_target: '${TARGET}' is not a target")
    endif()
    if(NOT LAPP_LOG_TAG)
        message(FATAL_ERROR
            "lur_configure_app_target(${TARGET}): LOG_TAG is required and has no default. "
            "Lur/Core/LogTag.h #errors without it, so that a game which forgets does not file its "
            "logs under another app's name.")
    endif()
    if(NOT LAPP_BLE_UUID)
        message(FATAL_ERROR
            "lur_configure_app_target(${TARGET}): BLE_UUID is required and has no default. "
            "It is the advertise/scan discriminator; inheriting another game's value fails "
            "SILENTLY, as two phones that never discover each other.")
    endif()

    # The tag is stated twice per app, and it has to be: the cache var set BEFORE
    # add_subdirectory(engine) makes it a compile definition for every ENGINE target in the tree,
    # while the argument here reaches the APP target, which that definition cannot see. If the two
    # ever disagreed the app's own sources would log under a different tag from the engine sources
    # compiled into the same binary — half the lines vanishing from `logcat -s <tag>`, which reads as
    # a dead subsystem rather than a build mistake. So check.
    if(LUR_LOG_TAG AND NOT LUR_LOG_TAG STREQUAL LAPP_LOG_TAG)
        message(FATAL_ERROR
            "lur_configure_app_target(${TARGET}): LOG_TAG '${LAPP_LOG_TAG}' disagrees with the "
            "LUR_LOG_TAG cache var '${LUR_LOG_TAG}' set before the engine was added. The engine's "
            "sources would log under one tag and this target's under the other.")
    endif()

    target_compile_definitions(${TARGET} PRIVATE
        # The capability ladder, derived from LUR_CONFIG by EngineFlags.cmake and published as cache
        # vars precisely so this scope can read them.
        LUR_SHIPPING=${LUR_SHIPPING}
        LUR_INTERNAL=${LUR_INTERNAL}
        LUR_ASSERTS=${LUR_ASSERTS}
        LUR_SLOW=${LUR_SLOW}
        # Drives Lur::Trace's macros: on in Development/Debugging, compiled out of Shipping. Without
        # it the touch->present latency lines expand to nothing.
        LUR_TRACE=${LUR_TRACE}
        # Assistant-only remote control. LUR_AGENT_ON is the DERIVED value (a Shipping build force-
        # zeroes it), so an ordinary build — the one a player installs — has none of it compiled in.
        LUR_AGENT=${LUR_AGENT_ON}
        # For the engine platform sources compiled into THIS target.
        LUR_LOG_TAG="${LAPP_LOG_TAG}"
        "LUR_BLE_SERVICE_UUID=\"${LAPP_BLE_UUID}\"")

    # ── The multi-config -O0 trap (#198, and the 20x measurement that found it) ──
    # EngineFlags couples LUR_CONFIG to optimization through CMAKE_BUILD_TYPE, which ONLY binds
    # single-config generators. Ninja (host, Android) gets it; the Xcode generator ignores it
    # entirely and `xcodebuild -configuration Debug` hard-forces -O0.
    #
    # On 2026-08-03 that made the iPhone's sim.step read ~2.5-3 ms against the Galaxy's ~0.15 ms —
    # a 20x gap that was PURELY the build, and it cost a full performance investigation before
    # anyone noticed. It has been surviving as a manual line on the two-phone pre-flight checklist.
    #
    # Pin the Xcode optimization attribute from LUR_CONFIG so the answer no longer depends on which
    # -configuration the caller happens to pass. LUR_XCODE_OPT is derived in EngineFlags next to
    # CMAKE_BUILD_TYPE, so both halves of the Opt column are decided in one place.
    get_property(_lur_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_lur_multi AND DEFINED LUR_XCODE_OPT)
        set_target_properties(${TARGET} PROPERTIES
            XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL "${LUR_XCODE_OPT}")
        message(STATUS "LurMotorn opt (${TARGET}): GCC_OPTIMIZATION_LEVEL=${LUR_XCODE_OPT} "
                "(from LUR_CONFIG=${LUR_CONFIG}) — overrides the -configuration passed to xcodebuild")
    endif()
endfunction()
