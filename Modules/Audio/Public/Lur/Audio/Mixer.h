#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "Lur/Audio/Sound.h"

namespace Lur::Audio {

using SoundId = int;
constexpr SoundId InvalidSound = -1;

// Real-time SFX mixer — the shared, pure-C++ heart of the audio module (no OS code, so it
// unit-tests on the host). It bridges two threads without a lock:
//
//   Add()    — main thread, at load: register a decoded Sound, get a SoundId. Do this
//              before Start(); Sounds are not touched concurrently with Render().
//   Play()   — GAME thread, per move: trigger a sound. Wait-free — it only pushes a small
//              record into an SPSC ring; it never allocates, blocks, or touches voices.
//   Render() — AUDIO thread, in the device callback: drains pending triggers into free
//              voices and sums active voices into the output buffer. Wait-free.
//
// Output is mono 16-bit at OutputRate() (pinned to 48 kHz — our cook rate and the
// low-latency rate on both phones). A Sound added at a different rate is resampled ONCE in
// Add(), so a voice at Pitch 1.0 reads its samples back verbatim.
//
// Each voice reads at its own PITCH (issue #78): its position is fixed-point and it steps
// by a fractional amount, linearly interpolating between samples. Pitch is the strongest
// variety knob there is for percussive SFX — it is what stops the hundredth repeat of a
// frequent event from being audibly the same click as the first. At Pitch == 1.0 the step is exactly one
// and the fraction is exactly zero, so the interpolation reproduces the PCM bit-for-bit;
// varying it costs one multiply-add per frame and no allocation, so Render() stays
// wait-free.
class Mixer {
public:
    static constexpr uint32_t DefaultRate = 48000;

    // Playback-rate bounds. Wide enough for any sane cosmetic jitter, narrow enough that
    // a bad caller cannot make a voice crawl through a clip for minutes.
    static constexpr float MinPitch = 0.25f;
    static constexpr float MaxPitch = 4.0f;

    void Init(uint32_t OutputRate = DefaultRate);

    // Register a sound (main thread, before Start). Resamples to OutputRate if needed.
    // Returns InvalidSound if empty or the mixer is full.
    SoundId Add(Sound Snd);

    // Trigger playback (game thread). Wait-free; silently drops if all trigger slots or
    // voices are momentarily full (inaudible for SFX, and never blocks the game).
    //
    // Pitch is a playback-rate multiplier: 1.0 verbatim, 1.06 about a semitone up (and
    // ~6% shorter), 0.5 an octave down. Clamped to [MinPitch, MaxPitch]. The float->
    // fixed-point conversion happens HERE, on the game thread, so the audio thread only
    // ever adds integers.
    void Play(SoundId Id, float Gain = 1.0f, float Pitch = 1.0f);

    // Fill Frames mono 16-bit samples (audio thread). Wait-free. Frames must be <= MaxBlock.
    void Render(int16_t* Out, uint32_t Frames);

    uint32_t OutputRate() const { return Rate; }
    int SoundCount() const { return static_cast<int>(Sounds.size()); }

private:
    static constexpr int MaxVoices = 16;          // simultaneous sounds
    static constexpr int TriggerRingSize = 64;    // power of two; pending Play() slots
    static constexpr uint32_t MaxBlock = 4096;    // largest device callback we accept

    // Read position and step are Q32.32 fixed point: the high 32 bits index the sample,
    // the low 32 are the fraction between it and the next. Fixed point (not a float
    // accumulator) so a long clip cannot drift as the position grows.
    static constexpr uint64_t PitchOne = 1ull << 32;

    struct Voice {
        SoundId  Id = InvalidSound;
        uint64_t Pos = 0;
        uint64_t Step = PitchOne;
        float    Gain = 1.0f;
    };
    struct Trigger { SoundId Id = InvalidSound; uint64_t Step = PitchOne; float Gain = 1.0f; };

    void DrainTriggers();

    uint32_t Rate = DefaultRate;
    std::vector<Sound> Sounds;                    // stable indices; SoundId is the index
    Voice Voices[MaxVoices];
    Trigger Ring[TriggerRingSize];
    int32_t Acc[MaxBlock];                        // audio-thread scratch accumulator

    std::atomic<uint32_t> Head{0};                // producer (game thread) writes triggers
    std::atomic<uint32_t> Tail{0};                // consumer (audio thread) reads triggers
};

} // namespace Lur::Audio
