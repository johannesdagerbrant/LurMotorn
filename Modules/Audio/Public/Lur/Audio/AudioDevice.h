#pragma once
#include "Lur/Audio/IAudioDevice.h"

namespace Lur::Audio {

// Create the platform audio output. Defined once per PLATFORM, in Modules/Audio/Platform/
// (AAudio on Android, RemoteIO on iOS) — a link-time seam, because there is one
// implementation per platform and never two at runtime.
//
// DECLARED everywhere, DEFINED only where a backend exists. On host and desktop builds there
// is none, so calling this fails to LINK rather than returning nullptr — deliberate: a build
// that wants sound and has no backend should say so at build time, not go quietly silent.
// Callers must still null-check the result (a device can fail to open).
// The returned device is owned by the caller; Stop() then delete to tear down.
IAudioDevice* CreateAudioDevice();

} // namespace Lur::Audio
