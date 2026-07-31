# Build fingerprint generator (#112's gate, #164's fix) — run in script mode, ONCE PER BUILD.
#
#   cmake -DLUR_FP_REPO=<repo root> -DLUR_FP_CONFIG=<LUR_CONFIG> -DLUR_FP_OUT=<file.cpp>
#         -P cmake/BuildFingerprint.cmake
#
# Why a build-time script instead of a CMake variable: the fingerprint answers "were these two
# binaries built from the same source?", and a value computed at CONFIGURE time cannot. Ninja and
# Gradle reconfigure only when a CMakeLists.txt changes, so the ordinary loop (commit, install)
# reused a cached fingerprint from the previous commit — two phones built from identical source
# reported badbuild=1 (twice on 2026-07-31), and an uncommitted edit was invisible in the other
# direction. Recomputing here, as a build step, makes the value exact for the binary it lands in.
#
# It emits a tiny TU rather than a compile definition for two reasons: a compile definition is fixed
# at configure time by construction, and a one-line TU means a fingerprint change relinks one object
# instead of recompiling every consumer of the macro.
if(NOT DEFINED LUR_FP_OUT)
    message(FATAL_ERROR "BuildFingerprint.cmake: -DLUR_FP_OUT=<file> is required")
endif()
if(NOT DEFINED LUR_FP_REPO)
    set(LUR_FP_REPO "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT DEFINED LUR_FP_CONFIG)
    set(LUR_FP_CONFIG "Unknown")
endif()

find_package(Git QUIET)
set(_fp "no-git")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${LUR_FP_REPO}
        OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    # --untracked-files=no on purpose: a new file that nothing includes yet does not change the
    # binary, and counting it would make the fingerprint flap while someone is merely adding notes.
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${LUR_FP_REPO}
        OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_sha STREQUAL "")
        set(_fp "no-git")
    elseif(_dirty STREQUAL "")
        set(_fp "${_sha}")
    else()
        set(_fp "${_sha}-dirty")
    endif()
endif()
# Same commit, different LUR_CONFIG => different sim (asserts, slow checks, -O), so the config is
# part of the build's identity, not metadata about it.
set(_full "${_fp}+${LUR_FP_CONFIG}")

set(_body
"// GENERATED per build by cmake/BuildFingerprint.cmake (#164). Do not edit; do not commit.\n\
#include \"Lur/Core/BuildFingerprint.h\"\n\
namespace Lur {\n\
const char* BuildFingerprint() { return \"${_full}\"; }\n\
}  // namespace Lur\n")

# Write only on a CHANGE. This script runs every build, so an unconditional write would relink (and
# on some generators re-run downstream steps) on every single build for no reason.
set(_prev "")
if(EXISTS "${LUR_FP_OUT}")
    file(READ "${LUR_FP_OUT}" _prev)
endif()
if(NOT _prev STREQUAL _body)
    file(WRITE "${LUR_FP_OUT}" "${_body}")
    message(STATUS "LurMotorn build fingerprint: ${_full}")
endif()
