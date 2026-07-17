// Dependency-free unit tests for the bit codec. No framework: each CHECK records
// a failure and the process exits non-zero if any failed, which CTest reports.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Serialization/Varint.h"

using namespace Lur::Serialization;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// A value written with N bits reads back identically.
static void TestRoundtripFixedWidths() {
    for (int Width = 1; Width <= 32; ++Width) {
        const uint32_t Value = (Width == 32) ? 0xDEADBEEFu
                                             : (0xDEADBEEFu & ((1u << Width) - 1u));
        BitWriter W;
        W.WriteBits(Value, Width);
        CHECK(W.GetBitCount() == static_cast<std::size_t>(Width));
        const auto& Bytes = W.Finish();

        BitReader R(Bytes.data(), Bytes.size());
        CHECK(R.ReadBits(Width) == Value);
        CHECK(R.IsOk());
    }
}

// A heterogeneous sequence of fields packs and unpacks in order.
static void TestMixedSequence() {
    BitWriter W;
    W.WriteBit(true);
    W.WriteBits(5, 3);     // 101
    W.WriteBits(0, 1);     // 0
    W.WriteBits(300, 9);   // 100101100
    const auto& Bytes = W.Finish();

    // 1 + 3 + 1 + 9 = 14 bits -> 2 bytes after padding.
    CHECK(Bytes.size() == 2);

    BitReader R(Bytes.data(), Bytes.size());
    CHECK(R.ReadBit() == true);
    CHECK(R.ReadBits(3) == 5u);
    CHECK(R.ReadBits(1) == 0u);
    CHECK(R.ReadBits(9) == 300u);
    CHECK(R.IsOk());
}

// Reading past the end is safe and flips IsOk() to false.
static void TestOverreadIsSafe() {
    BitWriter W;
    W.WriteBits(0b101, 3);
    const auto& Bytes = W.Finish();

    BitReader R(Bytes.data(), Bytes.size());
    CHECK(R.ReadBits(3) == 0b101u);
    CHECK(R.IsOk());
    (void)R.ReadBits(8);   // only padding/zeros remain, then past-end
    CHECK(!R.IsOk());
}

// BitsForIndex models the move-codec cost: 0 bits for a forced choice.
static void TestBitsForIndex() {
    CHECK(BitsForIndex(0) == 0);
    CHECK(BitsForIndex(1) == 0);   // forced move -> zero bits
    CHECK(BitsForIndex(2) == 1);
    CHECK(BitsForIndex(3) == 2);
    CHECK(BitsForIndex(4) == 2);
    CHECK(BitsForIndex(5) == 3);
    CHECK(BitsForIndex(20) == 5);  // typical midgame branching: 5 bits
    CHECK(BitsForIndex(32) == 5);
    CHECK(BitsForIndex(33) == 6);
}

// Varint round-trips across magnitudes.
static void TestVarintRoundtrip() {
    const uint64_t Samples[] = {0, 1, 15, 16, 255, 256, 1u << 20, 0xFFFFFFFFull};
    for (uint64_t V : Samples) {
        BitWriter W;
        WriteVarUint(W, V);
        const auto& Bytes = W.Finish();
        BitReader R(Bytes.data(), Bytes.size());
        CHECK(ReadVarUint(R) == V);
        CHECK(R.IsOk());
    }
}

// A hostile/corrupt varint (continuation bit stuck on) must not shift by >= 64
// (UB) or loop forever — it terminates when the reader runs dry. Feeding all-ones
// bytes keeps More=1 group after group; the guard drops bits past bit 63 and the
// loop ends at end-of-buffer with IsOk() false.
static void TestVarintHostileStreamTerminates() {
    std::vector<uint8_t> AllOnes(32, 0xFF);   // way more continuation groups than 64/4=16
    BitReader R(AllOnes.data(), AllOnes.size());
    const uint64_t V = ReadVarUint(R);        // must return (no UB / no hang)
    (void)V;
    CHECK(!R.IsOk());                          // consumed past the end -> not ok
}

int main() {
    TestRoundtripFixedWidths();
    TestMixedSequence();
    TestOverreadIsSafe();
    TestBitsForIndex();
    TestVarintRoundtrip();
    TestVarintHostileStreamTerminates();

    if (GFailures == 0) {
        std::printf("All bitstream tests passed.\n");
        return 0;
    }
    std::printf("%d bitstream test(s) failed.\n", GFailures);
    return 1;
}
