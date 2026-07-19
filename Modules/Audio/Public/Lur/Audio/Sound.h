#pragma once
#include <cstdint>
#include <vector>

namespace Lur::Audio {

// A decoded, ready-to-mix sound: mono 16-bit PCM at a known sample rate. SFX are tiny, so
// the whole clip lives in RAM — decoded ONCE at load (see PcmCodec), never per-play. This
// is what the mixer sums; keeping it decoded is what makes triggering a move sound a
// wait-free "copy some samples", with no codec on the audio thread.
struct Sound {
    std::vector<int16_t> Pcm;   // mono samples, one int16 per frame
    uint32_t Rate = 0;          // sample rate in Hz (48000 for our cooked SFX)

    uint32_t Frames() const { return static_cast<uint32_t>(Pcm.size()); }
    bool Empty() const { return Pcm.empty(); }
};

} // namespace Lur::Audio
