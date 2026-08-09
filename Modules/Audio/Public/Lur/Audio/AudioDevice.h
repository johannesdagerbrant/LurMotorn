#pragma once
#include "Lur/Audio/IAudioDevice.h"

namespace Lur::Audio {

// Create the platform audio output. Defined once per PLATFORM in the app build (AAudio on
// Android, RemoteIO on iOS), exactly like CreateBleTransport — a link-time seam, because
// there is one implementation per platform and never two at runtime. Returns nullptr where
// no backend exists (e.g. host unit tests), so callers must null-check.
// The returned device is owned by the caller; Stop() then delete to tear down.
IAudioDevice* CreateAudioDevice();

} // namespace Lur::Audio
