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
// Toggle: define LUR_ASSERTS_ENABLED=1 to force on, =0 to force off. Otherwise it
// follows NDEBUG (on in Debug, off in Release).
#include <cstdio>
#include <cstdlib>

#if !defined(LUR_ASSERTS_ENABLED)
    #if defined(NDEBUG)
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
    // LUR_ASSERT_MSG(cond, fmt, ...) — printf-style context on failure.
    #define LUR_ASSERT_MSG(Cond, ...)                                              \
        do {                                                                       \
            if (!(Cond)) {                                                         \
                std::fprintf(stderr, "LUR_ASSERT failed: %s\n  at %s:%d\n  ",      \
                             #Cond, __FILE__, __LINE__);                           \
                std::fprintf(stderr, __VA_ARGS__);                                 \
                std::fprintf(stderr, "\n");                                        \
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
