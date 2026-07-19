#pragma once
#include "Lur/Audio/IAudioDevice.h"

namespace Lur::Audio {

// Create the platform audio output. Implemented separately in each app build — Games/Chess/
// Android (AAudio) and Games/Chess/iOS (RemoteIO) — exactly like CreateBleTransport. Returns
// nullptr on platforms without a backend (e.g. host unit tests), so callers must null-check.
// The returned device is owned by the caller; Stop() then delete to tear down.
IAudioDevice* CreateAudioDevice();

} // namespace Lur::Audio
