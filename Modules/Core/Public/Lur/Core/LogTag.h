#pragma once

// The app's log tag, supplied by the build.
//
// Every platform logging call needs one: Android's __android_log_print takes it as the tag
// that `logcat -s <tag>` filters on, and iOS's os_log lines are conventionally prefixed with
// it so a syslog capture can be grepped. It is the app's name, not the engine's, so the engine
// takes it as a build parameter rather than knowing it.
//
// REQUIRED, with no default — the same discipline as LUR_BLE_SERVICE_UUID and for the same
// reason. Four copies of the Vulkan surface seam existed whose ONLY difference was this string
// hardcoded; a default here would simply move that mistake one level up, and the app that
// forgot to set it would log under another app's tag, which is invisible until someone greps a
// device log and quietly reads the wrong app's lines.
#ifndef LUR_LOG_TAG
#error "LUR_LOG_TAG is not defined. Each app must define its own log tag (a per-app \
target_compile_definitions) — inheriting another app's would file its device logs under that \
app's name."
#endif

namespace Lur::Core {

// The tag as a plain string literal, for the per-platform logging seams.
inline constexpr const char* LogTag = LUR_LOG_TAG;

}  // namespace Lur::Core
