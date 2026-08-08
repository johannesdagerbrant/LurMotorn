#include "Lur/Audio/Mixer.h"

#include "Lur/Core/Assert.h"

namespace Lur::Audio {

void Mixer::Init(uint32_t OutputRate) {
    Rate = OutputRate ? OutputRate : DefaultRate;
    Sounds.clear();
    for (Voice& V : Voices) V = Voice{};
    Head.store(0, std::memory_order_relaxed);
    Tail.store(0, std::memory_order_relaxed);
}

SoundId Mixer::Add(Sound Snd) {
    if (Snd.Empty()) return InvalidSound;
    if (static_cast<int>(Sounds.size()) >= (1 << 20)) return InvalidSound;  // sanity bound

    // Resample to the output rate ONCE (linear). Our cooked SFX are already 48 kHz, so this
    // is a no-op in practice; it just keeps Render() correct if a device forces another
    // rate. Doing it here rather than per-voice means a voice's Step carries only the
    // caller's PITCH, never a device-rate correction.
    if (Snd.Rate != Rate && Snd.Rate != 0) {
        const uint32_t SrcN = Snd.Frames();
        const uint64_t DstN = (static_cast<uint64_t>(SrcN) * Rate + Snd.Rate / 2) / Snd.Rate;
        std::vector<int16_t> Dst(static_cast<size_t>(DstN));
        for (uint64_t i = 0; i < DstN; ++i) {
            const double SrcPos = static_cast<double>(i) * Snd.Rate / Rate;
            const uint32_t i0 = static_cast<uint32_t>(SrcPos);
            const uint32_t i1 = (i0 + 1 < SrcN) ? i0 + 1 : i0;
            const double Frac = SrcPos - i0;
            Dst[static_cast<size_t>(i)] =
                static_cast<int16_t>(Snd.Pcm[i0] * (1.0 - Frac) + Snd.Pcm[i1] * Frac);
        }
        Snd.Pcm = std::move(Dst);
        Snd.Rate = Rate;
    }

    Sounds.push_back(std::move(Snd));
    return static_cast<SoundId>(Sounds.size() - 1);
}

void Mixer::Play(SoundId Id, float Gain, float Pitch) {
    if (Id < 0 || Id >= static_cast<SoundId>(Sounds.size())) return;
    // Clamp here, on the game thread: the audio thread must never have to reason about a
    // caller's bad number, and a NaN would otherwise convert to garbage below.
    if (!(Pitch >= MinPitch)) Pitch = MinPitch;              // also catches NaN
    if (Pitch > MaxPitch) Pitch = MaxPitch;
    const uint64_t Step = static_cast<uint64_t>(static_cast<double>(Pitch) *
                                                static_cast<double>(PitchOne));

    const uint32_t Head_ = Head.load(std::memory_order_relaxed);
    const uint32_t Tail_ = Tail.load(std::memory_order_acquire);
    if (Head_ - Tail_ >= TriggerRingSize) return;           // ring full — drop
    Ring[Head_ & (TriggerRingSize - 1)] = Trigger{Id, Step, Gain};
    Head.store(Head_ + 1, std::memory_order_release);        // publish after the write
}

void Mixer::DrainTriggers() {
    uint32_t Tail_ = Tail.load(std::memory_order_relaxed);
    const uint32_t Head_ = Head.load(std::memory_order_acquire);
    for (; Tail_ != Head_; ++Tail_) {
        const Trigger T = Ring[Tail_ & (TriggerRingSize - 1)];
        for (Voice& V : Voices) {                            // assign to the first free voice
            if (V.Id == InvalidSound) {
                V.Id = T.Id; V.Pos = 0; V.Step = T.Step; V.Gain = T.Gain;
                break;
            }
        }
        // If every voice is busy, the trigger is dropped (16 voices is plenty for SFX).
    }
    Tail.store(Tail_, std::memory_order_release);
}

void Mixer::Render(int16_t* Out, uint32_t Frames) {
    LUR_ASSERT(Frames <= MaxBlock);
    if (Frames > MaxBlock) Frames = MaxBlock;                // quiet guard in shipping

    DrainTriggers();

    for (uint32_t f = 0; f < Frames; ++f) Acc[f] = 0;

    for (Voice& V : Voices) {
        if (V.Id == InvalidSound) continue;
        const Sound& S = Sounds[static_cast<size_t>(V.Id)];
        const uint32_t N = S.Frames();
        const int16_t* P = S.Pcm.data();
        for (uint32_t f = 0; f < Frames; ++f) {
            const uint32_t i0 = static_cast<uint32_t>(V.Pos >> 32);
            if (i0 >= N) break;                              // ran off the end mid-block
            // Hold the last sample rather than wrapping to P[0] — reading the head of the
            // clip as the tail's neighbour would splice a click onto every voice's end.
            const uint32_t i1 = (i0 + 1 < N) ? i0 + 1 : i0;
            const float Frac = static_cast<float>(static_cast<uint32_t>(V.Pos)) *
                               (1.0f / 4294967296.0f);
            const float A = static_cast<float>(P[i0]);
            const float Smp = A + (static_cast<float>(P[i1]) - A) * Frac;
            Acc[f] += static_cast<int32_t>(Smp * V.Gain);
            V.Pos += V.Step;
        }
        if (static_cast<uint32_t>(V.Pos >> 32) >= N) V.Id = InvalidSound;   // voice finished
    }

    for (uint32_t f = 0; f < Frames; ++f) {
        int32_t s = Acc[f];
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        Out[f] = static_cast<int16_t>(s);
    }
}

} // namespace Lur::Audio
