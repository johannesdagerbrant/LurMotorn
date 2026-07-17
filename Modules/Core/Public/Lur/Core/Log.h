#pragma once
#include <cstdarg>
#include <cstdio>

// Lur::Log — one engine-wide logging seam (Review #1 §3.5). The app installs ONE sink
// once (Init); everything else calls Info/Error with a printf-style format. Replaces
// the scattered ad-hoc paths (per-object SetLogger lambdas, LOGI macros, Vk::PlatformLog).
//
// The sink is a plain function pointer + void* user (Handmade lens: no std::function
// heap/indirection for a single boot-time subscriber). With NO sink installed it falls
// back to stdout/stderr prefixed with the tag — so the desktop build logs out of the
// box, and a phone shell installs a logcat/os_log sink at startup.
namespace Lur::Log {

using Sink = void (*)(bool Error, const char* Line, void* User);

namespace Detail {
inline Sink        GSink = nullptr;
inline void*       GUser = nullptr;
inline const char* GTag  = "Lur";

inline void Emit(bool Error, const char* Fmt, va_list Args) {
    char Buf[512];
    std::vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    if (GSink != nullptr) GSink(Error, Buf, GUser);
    else std::fprintf(Error ? stderr : stdout, "[%s] %s\n", GTag, Buf);
}
}  // namespace Detail

// Install the sink + tag once at startup. Sink may be null to use the built-in
// stdout/stderr writer (handy for the desktop / host).
inline void Init(Sink S, const char* Tag, void* User = nullptr) {
    Detail::GSink = S;
    Detail::GUser = User;
    Detail::GTag  = (Tag != nullptr) ? Tag : "Lur";
}

inline void Info(const char* Fmt, ...) {
    va_list A;
    va_start(A, Fmt);
    Detail::Emit(false, Fmt, A);
    va_end(A);
}

inline void Error(const char* Fmt, ...) {
    va_list A;
    va_start(A, Fmt);
    Detail::Emit(true, Fmt, A);
    va_end(A);
}

}  // namespace Lur::Log
