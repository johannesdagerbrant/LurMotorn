// Host tests for the RPS lockstep netcode. Starts with the event codec: round-trip,
// the byte budget (a press/watermark is 1 byte before framing), and fuzz-safety
// (hostile bytes never crash the decoder). Chess's fuzz_tests as the pattern.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Sim/Random.h"
#include "Rps/EventCodec.h"

using namespace Rps;
using Lur::Serialization::BitReader;
using Lur::Serialization::BitWriter;
using Lur::Sim::SplitMix64;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// ---- Round-trip: every (delta, mask) survives encode -> decode ----
static void TestRoundTrip() {
    const uint32_t Deltas[] = {1, 2, 7, 14, 15, 16, 100, 9000, 1u << 20};
    for (uint32_t D : Deltas)
        for (uint8_t M = 0; M < 16; ++M) {
            BitWriter W;
            EncodeEvent(W, D, M);
            const std::vector<uint8_t>& Bytes = W.Finish();
            BitReader R(Bytes.data(), Bytes.size());
            uint32_t Dd = 0;
            uint8_t Mm = 0;
            CHECK(DecodeEvent(R, Dd, Mm));
            CHECK(Dd == D && Mm == M);
        }
}

// ---- Byte budget: a live-wire event (delta 1..14) is exactly ONE byte ----
static void TestByteBudget() {
    for (uint32_t D = 1; D < EventDeltaInline; ++D) {
        BitWriter W;
        EncodeEvent(W, D, 0xB);
        CHECK(W.Finish().size() == 1);  // 1 byte; framed (+type) = the 2-byte budget
    }
    // A watermark (mask 0, delta 1) is also one byte — the idle case is not special.
    BitWriter Wm;
    EncodeEvent(Wm, 1, 0x0);
    CHECK(Wm.Finish().size() == 1);
    // The escape (delta >= 15) costs the varint but stays small.
    BitWriter We;
    EncodeEvent(We, 9000, 0x5);
    CHECK(We.Finish().size() <= 4);
}

// ---- A stream of events (resync/recorder use): decode reconstructs absolute ticks ----
static void TestStreamAbsoluteTicks() {
    struct EV { uint32_t Delta; uint8_t Mask; };
    const EV In[] = {{1, 0x2}, {3, 0x4}, {1, 0x0}, {20, 0x8}, {1, 0x1}};
    BitWriter W;
    for (const EV& E : In) EncodeEvent(W, E.Delta, E.Mask);
    const std::vector<uint8_t>& Bytes = W.Finish();

    BitReader R(Bytes.data(), Bytes.size());
    uint32_t Tick = 0;  // events are delta-coded; accumulate to absolute ticks
    int I = 0;
    for (;;) {
        uint32_t D = 0;
        uint8_t M = 0;
        if (!DecodeEvent(R, D, M)) break;
        Tick += D;
        CHECK(I < 5 && M == In[I].Mask);
        ++I;
        if (I == 5) break;  // consumed all five (trailing padding bits may look like a 0,0 event)
    }
    CHECK(I == 5);
    CHECK(Tick == 1 + 3 + 1 + 20 + 1);
}

// ---- Fuzz: hostile bytes never crash; the decoder stays total ----
static void TestFuzzDecode() {
    SplitMix64 Rng(0xF0FF);
    for (int Iter = 0; Iter < 20000; ++Iter) {
        uint8_t Buf[8];
        const int N = 1 + static_cast<int>(Rng.NextBounded(8));
        for (int I = 0; I < N; ++I) Buf[I] = static_cast<uint8_t>(Rng.NextBounded(256));
        BitReader R(Buf, static_cast<size_t>(N));
        // Drain events until the reader runs dry — must always terminate, never trap.
        for (int Guard = 0; Guard < 64; ++Guard) {
            uint32_t D = 0;
            uint8_t M = 0;
            if (!DecodeEvent(R, D, M)) break;
        }
    }
    CHECK(true);  // reaching here without a crash/hang is the assertion
}

int main() {
    TestRoundTrip();
    TestByteBudget();
    TestStreamAbsoluteTicks();
    TestFuzzDecode();

    if (GFailures == 0) std::printf("rps_net_tests: ALL PASS\n");
    else std::printf("rps_net_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
