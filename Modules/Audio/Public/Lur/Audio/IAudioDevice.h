#pragma once
#include <cstdint>

namespace Lur::Audio {

// Fills `Frames` mono 16-bit samples into `Out`. Invoked on the OS audio device's realtime
// callback thread, so it MUST be wait-free: no locks, no allocation, no syscalls, no I/O.
// In practice it forwards straight to Mixer::Render. `User` is the opaque pointer passed to
// Start (the Mixer).
using RenderCallback = void (*)(void* User, int16_t* Out, uint32_t Frames);

// The OS audio output. Owns a low-latency callback stream that pulls mono 16-bit frames at
// 48 kHz. Two backends, selected purely by which seam file each app build links (no ifdef
// factory): AAudio on Android, a RemoteIO Audio Unit on iOS. The host has none.
//
// Lifecycle mirrors the transport/renderer seams: create via CreateAudioDevice(), Start on
// app foreground, Stop on background/teardown.
struct IAudioDevice {
    virtual ~IAudioDevice() = default;

    // Open the stream and begin calling Cb on the audio thread. Returns false if the OS
    // could not start a stream. Idempotent-safe: calling Start twice without Stop is a no-op.
    virtual bool Start(RenderCallback Cb, void* User) = 0;

    // Stop and close the stream. After this the callback is guaranteed not to run.
    virtual void Stop() = 0;

    // Device-native output rate in Hz (48000). Valid after a successful Start.
    virtual uint32_t OutputRate() const = 0;
};

} // namespace Lur::Audio
