#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Audio/Mixer.h"
#include "Lur/Audio/PcmCodec.h"
#include "Lur/Audio/Sound.h"

using namespace Lur::Audio;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Deterministic pseudo-random (no <random>, no Math.random) so tests are reproducible.
static uint32_t Lcg(uint32_t& S) { S = S * 1664525u + 1013904223u; return S; }

// Encode then decode a PCM buffer and assert it comes back BIT-EXACT — the whole point of
// a lossless codec. Also returns the compressed size for the compression-ratio check.
static size_t RoundTrip(const std::vector<int16_t>& Pcm, uint32_t Rate) {
    const std::vector<uint8_t> Blob =
        EncodeLossless(Pcm.data(), static_cast<uint32_t>(Pcm.size()), Rate);
    Sound Out;
    const bool Ok = DecodeLossless(Blob.data(), Blob.size(), Out);
    CHECK(Ok);
    CHECK(Out.Rate == Rate);
    CHECK(Out.Frames() == Pcm.size());
    bool Exact = Out.Pcm.size() == Pcm.size();
    for (size_t i = 0; Exact && i < Pcm.size(); ++i) Exact = (Out.Pcm[i] == Pcm[i]);
    CHECK(Exact);
    return Blob.size();
}

// Lossless across the signals the codec must survive: silence, extremes, a smooth tone, and
// full-range noise — including a size that spans several blocks and an odd tail.
static void TestCodecRoundTrip() {
    uint32_t S = 0x1234567u;

    std::vector<int16_t> Silence(1000, 0);
    RoundTrip(Silence, 48000);

    std::vector<int16_t> Extremes;
    for (int i = 0; i < 200; ++i) Extremes.push_back((i & 1) ? 32767 : -32768);
    RoundTrip(Extremes, 48000);

    // A smooth, compressible tone spanning >2 blocks with an odd-length tail.
    std::vector<int16_t> Tone(9000 + 37);
    for (size_t i = 0; i < Tone.size(); ++i) {
        // integer triangle wave — smooth, so the fixed predictors shrink it a lot
        const int t = static_cast<int>(i % 200);
        Tone[i] = static_cast<int16_t>((t < 100 ? t : 200 - t) * 300 - 15000);
    }
    const size_t ToneBytes = RoundTrip(Tone, 48000);
    CHECK(ToneBytes < Tone.size() * sizeof(int16_t));   // must actually compress a tone

    // Full-range noise — worst case; must stay lossless (order 0 + escape handles it).
    std::vector<int16_t> Noise(5000);
    for (auto& v : Noise) v = static_cast<int16_t>(Lcg(S) & 0xFFFF);
    RoundTrip(Noise, 44100);

    // Empty and single-sample edge cases.
    std::vector<int16_t> One(1, -12345);
    RoundTrip(One, 48000);
}

// A bad or truncated blob must be REJECTED, not crash (shipped games embed trusted data, but
// the decoder still guards).
static void TestCodecRejectsGarbage() {
    Sound Out;
    const uint8_t Bad[12] = {'X', 'X', 'X', 'X', 0, 0, 0, 0, 10, 0, 0, 0};
    CHECK(!DecodeLossless(Bad, sizeof(Bad), Out));         // wrong magic
    CHECK(!DecodeLossless(Bad, 4, Out));                   // too short for a header

    // Valid header claiming 10000 frames but no bitstream -> overrun -> false.
    const uint8_t Starved[12] = {'L', 'S', 'F', '1', 0x80, 0xBB, 0, 0, 0x10, 0x27, 0, 0};
    CHECK(!DecodeLossless(Starved, sizeof(Starved), Out));
}

static Sound MakeSound(uint32_t frames, int16_t value, uint32_t rate) {
    Sound s;
    s.Pcm.assign(frames, value);
    s.Rate = rate;
    return s;
}

// The mixer sums active voices, advances and retires them, ignores bad ids, and never blocks.
static void TestMixerMixesAndRetires() {
    Mixer M;
    M.Init(48000);
    CHECK(M.OutputRate() == 48000);

    const SoundId A = M.Add(MakeSound(4, 1000, 48000));    // 4-frame tone
    const SoundId B = M.Add(MakeSound(2, 2000, 48000));    // 2-frame tone
    CHECK(A == 0 && B == 1);
    CHECK(M.Add(Sound{}) == InvalidSound);                 // empty rejected

    M.Play(A);
    M.Play(B);
    M.Play(999);                                           // out-of-range: ignored

    int16_t Buf[8] = {0};
    M.Render(Buf, 8);
    // frame 0,1: A+B = 3000 ; frame 2,3: A only = 1000 ; frame 4..: silence
    CHECK(Buf[0] == 3000 && Buf[1] == 3000);
    CHECK(Buf[2] == 1000 && Buf[3] == 1000);
    CHECK(Buf[4] == 0 && Buf[7] == 0);

    // Both voices have retired; a fresh render is pure silence.
    int16_t Buf2[4] = {7, 7, 7, 7};
    M.Render(Buf2, 4);
    CHECK(Buf2[0] == 0 && Buf2[3] == 0);
}

// Clipping: summed voices beyond full scale saturate rather than wrap.
static void TestMixerClips() {
    Mixer M;
    M.Init(48000);
    const SoundId A = M.Add(MakeSound(4, 30000, 48000));
    for (int i = 0; i < 4; ++i) M.Play(A);                 // 4 * 30000 = 120000 -> clamp
    int16_t Buf[4] = {0};
    M.Render(Buf, 4);
    CHECK(Buf[0] == 32767);
}

// Flooding Play() far past the ring/voice capacity must not crash or block (wait-free drop).
static void TestMixerPlayFloodIsSafe() {
    Mixer M;
    M.Init(48000);
    const SoundId A = M.Add(MakeSound(100, 500, 48000));
    for (int i = 0; i < 1000; ++i) M.Play(A);              // way past ring + voices
    int16_t Buf[64] = {0};
    M.Render(Buf, 64);
    CHECK(Buf[0] > 0);                                     // at least some voices played
}

// A sound added at a different rate is resampled to the output rate in Add().
static void TestMixerResamplesOnAdd() {
    Mixer M;
    M.Init(48000);
    const SoundId A = M.Add(MakeSound(100, 1000, 24000));  // half rate -> ~200 frames
    CHECK(A == 0);
    M.Play(A);
    int16_t Buf[256] = {0};
    M.Render(Buf, 256);
    // ~200 frames of ~1000 then silence; sample well inside the clip should be ~1000.
    CHECK(Buf[50] > 800 && Buf[50] < 1200);
    CHECK(Buf[255] == 0);
}

int main() {
    TestCodecRoundTrip();
    TestCodecRejectsGarbage();
    TestMixerMixesAndRetires();
    TestMixerClips();
    TestMixerPlayFloodIsSafe();
    TestMixerResamplesOnAdd();

    if (GFailures == 0) {
        std::printf("All audio tests passed.\n");
        return 0;
    }
    std::printf("%d audio test(s) failed.\n", GFailures);
    return 1;
}
