#pragma once
#include <string>

// Per-platform app-startup verbs, written ONCE (issue #43, Phase 3 section B).
//
// These are the pieces of a main() that are pure OS ceremony: no game decides them, every game needs
// them, and each one was being re-derived per (game x platform). Section B moves the whole entry point
// here eventually; this header is the seam it grows into, starting with the two absorptions whose
// duplication was doing active harm rather than merely costing lines.
//
// The tag is NOT a parameter. `Lur::Core::LogTag` comes from LUR_LOG_TAG, a required build definition
// with no default (#42/#200) — so a sink written here names the right app automatically, and a game
// that forgets to define it fails to BUILD instead of logging under someone else's name. The two
// hand-written sinks this replaces both hardcoded "OnlyRps", which is the duplication-maintained-by-
// hope shape the batch keeps finding.
namespace Lur::App::Platform {

// Install the engine's log sink so Lur::Log::* from inside the engine reaches the platform log.
//
// WHY IT MATTERS, and it is not cosmetic: chess never installed one on EITHER phone, so every
// engine-side message on device went to a stdout nobody reads. That is not hypothetical harm — the
// same absence cost a real diagnosis on RPS (2026-07-30): #112's build-fingerprint gate fired,
// reported the mismatch through Lur::Log::Error, and the line went nowhere; the pair then played 13
// minutes and desynced with "were the builds even the same?" unanswerable from the log. RPS grew a
// sink in response, in two copies. Chess never got one at all.
//
// Call this FIRST in a main, before anything can try to report a problem.
void InstallLogSink();

#if defined(__APPLE__)
// A BLOCKING WRITE TO STDERR CAN KILL THE APP. Diagnosed on OnlyRps 2026-08-01 from a crash report:
// the main thread stopped in
//     __write_nocancel <- fprintf <- MVKBaseObject::reportMessage <- MVKInstance::logVersions
//                      <- vkCreateInstance <- VulkanRendererImpl::Init
// and FrontBoard killed the process with 0x8BADF00D — "scene-update watchdog transgression,
// exhausted real (wall clock) time allowance of 10.00 seconds".
//
// MoltenVK announces its version through plain fprintf, and nothing drains the app's stdio after a
// `dvt launch`, so once that pipe's buffer fills write(2) blocks — forever, on the thread that also
// runs the render loop. The symptom is a BLACK SCREEN with NO log output (os_log never gets a turn)
// while the process is still alive in proclist, so every "is it running?" probe says yes.
//
// Two guards: the env var stops the chatty lines (errors still get through), and O_NONBLOCK makes a
// would-block write fail with EAGAIN and DROP bytes instead. Losing a diagnostic line always beats
// wedging the app — logging here is os_log's job and stdio has no reader by construction.
//
// Every MoltenVK app has this exposure, so it belongs here rather than in a game: it was duplicated
// verbatim in both iOS mains, INCLUDING all sixteen lines of the postmortem above it. Call before
// creating the renderer.
void UnblockStdio();

// Where this app may write: Application Support, created by the Store on first save.
//
// It was computed independently in THREE places on iOS — both mains and the CoreBluetooth
// driver's own IosSaveDir() — and they must agree, because the device GUID is written through
// one and read back through another. Two paths that drift here do not fail loudly; they fail as
// a phone that has silently forgotten who it is, which then re-rolls its identity and strands
// its peer's cached BLE role (#79's deaf-pair shape).
//
// Apple-only on purpose. The Android answer needs the activity handle, so it stays
// AndroidApp::SaveDir(); declaring a uniform one here would only be a link error waiting to
// happen. The #ifdef makes a wrong-platform call a COMPILE error instead.
std::string SaveDir();
#endif

} // namespace Lur::App::Platform
