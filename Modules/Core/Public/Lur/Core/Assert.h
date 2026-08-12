#pragma once
// LUR_ASSERT — crash loudly in dev, compile out in release.
//
// Philosophy (Review #2 §5, "crash loudly, guard quietly"): in DEVELOPMENT builds a
// contradiction should be DEAFENING. A LUR_ASSERT marks a "can't happen" invariant —
// a programmer error, not hostile input. If it fails we print the expression, the
// file:line, and a message, then trap into the debugger. In RELEASE builds the macro
// compiles to nothing, and the surrounding code keeps its own quiet runtime guard for
// peer-supplied / untrusted data (a shipped game must not crash on a bad packet).
//
// So: LUR_ASSERT for our own invariants; ordinary if-guards + logging for wire input.
//
// Renderer exception (#93, decision #4): the Vulkan backend does NOT LUR_ASSERT on
// swapchain / VK_ERROR_DEVICE_LOST. Mobile GPUs lose the device on thermal spikes /
// rotation, so those paths HEAL + loud-log in Development (the #73 self-healer), and
// only the dedicated Debugging build (LUR_SLOW) traps on a genuine device-loss for
// maximum signal. See LUR_ON_DEVICE_LOST in VulkanBackend.cpp.
//
// Toggle: define LUR_ASSERTS_ENABLED=1 to force on, =0 to force off. Otherwise it
// follows the build-configuration macro LUR_ASSERTS (set by cmake/EngineFlags.cmake
// from LUR_CONFIG): on in DEVELOPMENT/DEBUGGING, off in SHIPPING. This is DECOUPLED
// from NDEBUG on purpose — an optimized DEVELOPMENT build (NDEBUG defined, e.g. the
// overnight soak) must still trap. Only when neither is defined (a bare compile with
// no EngineFlags) do we fall back to NDEBUG so a stray host tool still behaves.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Lur/Core/Log.h"

#if !defined(LUR_ASSERTS_ENABLED)
    #if defined(LUR_ASSERTS)
        #define LUR_ASSERTS_ENABLED LUR_ASSERTS
    #elif defined(NDEBUG)
        #define LUR_ASSERTS_ENABLED 0
    #else
        #define LUR_ASSERTS_ENABLED 1
    #endif
#endif

#if defined(_MSC_VER)
    #define LUR_DEBUG_TRAP() __debugbreak()
#elif defined(__has_builtin) && __has_builtin(__builtin_trap)
    #define LUR_DEBUG_TRAP() __builtin_trap()
#else
    #define LUR_DEBUG_TRAP() std::abort()
#endif

#if LUR_ASSERTS_ENABLED
namespace Lur::Assert::Detail {

// Report a failed assertion through Lur::Log rather than straight to stderr.
//
// WHY: on a phone, stderr IS NOWHERE. Nothing drains a sideloaded app's stdio (see
// Lur::App::Platform::UnblockStdio, which makes those writes drop rather than wedge the
// app), so "deafening in Development" was true only on the desktop — on device a failed
// assert trapped SILENTLY, leaving just a backtrace to work back from. Lur::Log has a
// platform sink installed by every main now (os_log / logcat, #43 section B), and its own
// no-sink fallback is still stderr — so the host keeps behaving exactly as before.
//
// One line, not three: os_log and logcat are line-oriented, and a single line survives the
// grep-through-a-filtered-capture that reading device logs actually requires.
inline void Report(const char* Expr, const char* File, int Line, const char* Fmt, ...) {
    char Msg[256];
    va_list A;
    va_start(A, Fmt);
    std::vsnprintf(Msg, sizeof(Msg), Fmt, A);
    va_end(A);
    Lur::Log::Error("LUR_ASSERT failed: %s | at %s:%d | %s", Expr, File, Line, Msg);
}

}  // namespace Lur::Assert::Detail

    // LUR_ASSERT_MSG(cond, fmt, ...) — printf-style context on failure.
    #define LUR_ASSERT_MSG(Cond, ...)                                              \
        do {                                                                       \
            if (!(Cond)) {                                                         \
                ::Lur::Assert::Detail::Report(#Cond, __FILE__, __LINE__,           \
                                              __VA_ARGS__);                        \
                LUR_DEBUG_TRAP();                                                  \
            }                                                                      \
        } while (0)
    #define LUR_ASSERT(Cond) LUR_ASSERT_MSG(Cond, "%s", "(invariant violated)")
#else
    // Evaluate nothing, but reference Cond via sizeof so it stays "used" (no warnings)
    // and any names it mentions don't become dead in release builds.
    #define LUR_ASSERT_MSG(Cond, ...) do { (void)sizeof(Cond); } while (0)
    #define LUR_ASSERT(Cond)          do { (void)sizeof(Cond); } while (0)
#endif
