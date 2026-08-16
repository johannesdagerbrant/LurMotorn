// Host tests for Lur::Transport::GattReadLength — how many bytes one ATT read may return.
//
// This arithmetic has broken the cross-platform link TWICE.
//
//   #17  the Android GATT server ignored the read OFFSET and returned the whole 32-byte device id
//        for every request, corrupting the central's reassembly of a long read.
//   #206 the offset was honoured but the LENGTH never was: the server returned every remaining
//        byte regardless of the connection's ATT MTU. Caught on the pair 2026-08-16 — every
//        outgoing iPhone attempt stalled at exactly the same step:
//
//            central attempt -> CharsFound
//            central: connect watchdog - stalled at CharsFound, tearing it down
//
//        i.e. readValueForCharacteristic was issued and didUpdateValueForCharacteristic never
//        fired, with neither a value nor an error, while Android logged `serve device id: offset=0
//        -> 32B` and no continuation at any non-zero offset. A 32-byte value cannot fit a
//        default-MTU response, so claiming to have sent it all means the central never blob-reads
//        the tail and the read never completes.
//
// Both were off-by-something in five lines of platform code that no test could reach. It is engine
// C++ now, with the ATT rules written down, because "it broke the link twice" is the strongest
// possible argument that this is a DECISION and not ceremony.
//
// The ATT rules being encoded:
//   * A Read Response and a Read Blob Response each carry at most MTU-1 bytes (one opcode byte).
//   * The default ATT MTU is 23, and no connection may be below it.
//   * Offset == size is legal and returns zero bytes — that is how a long read terminates.
//   * Offset > size is an error (ATT INVALID_OFFSET), never a silent empty read.
#include <cstdio>

#include "Lur/Transport/GattLongRead.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// #206 exactly: 32 bytes, default MTU. The first read must NOT claim all 32.
static void TestDeviceIdAtDefaultMtuIsChunked() {
    const int First = GattReadLength(32, 0, AttDefaultMtu);
    CHECK(First == 22);                       // 23 - 1, not 32
    const int Second = GattReadLength(32, 22, AttDefaultMtu);
    CHECK(Second == 10);                      // the tail the central blob-reads
    CHECK(GattReadLength(32, 32, AttDefaultMtu) == 0);   // ...and then it is done
}

// With a negotiated MTU big enough, the whole value goes in one response and there is no blob.
static void TestLargeMtuSendsItAllAtOnce() {
    CHECK(GattReadLength(32, 0, 247) == 32);
    CHECK(GattReadLength(32, 32, 247) == 0);
}

// The boundary that decides whether a blob happens at all: MTU-1 == size.
static void TestExactFitNeedsNoSecondRead() {
    CHECK(GattReadLength(32, 0, 33) == 32);   // 33 - 1 == 32, fits exactly
    CHECK(GattReadLength(32, 0, 32) == 31);   // one short, so the tail remains
    CHECK(GattReadLength(32, 31, 32) == 1);
}

// Offset == size terminates a long read with an empty response. Offset > size is an ATT error, and
// must be distinguishable from it — returning 0 for both would make the central spin.
static void TestOffsetAtEndVersusPastEnd() {
    CHECK(GattReadLength(32, 32, AttDefaultMtu) == 0);
    CHECK(GattReadLength(32, 33, AttDefaultMtu) == GattReadInvalidOffset);
    CHECK(GattReadInvalidOffset < 0);         // a length can never be negative, so this is unambiguous
}

// No connection is below the ATT default. A stack reporting something smaller is reporting nonsense,
// and honouring it would produce responses too short to make progress.
static void TestMtuBelowTheDefaultIsClamped() {
    CHECK(GattReadLength(32, 0, 0) == 22);
    CHECK(GattReadLength(32, 0, 5) == 22);
}

// An empty value is legal — a characteristic we do not serve. One empty response, no blob.
static void TestEmptyValue() {
    CHECK(GattReadLength(0, 0, AttDefaultMtu) == 0);
    CHECK(GattReadLength(0, 1, AttDefaultMtu) == GattReadInvalidOffset);
}

// A value that needs three responses: the middle one must be full, not short. A short middle chunk
// still "works" but triples the round trips on the path whose latency IS the product.
static void TestMultiChunkMiddleIsFull() {
    CHECK(GattReadLength(50, 0, AttDefaultMtu) == 22);
    CHECK(GattReadLength(50, 22, AttDefaultMtu) == 22);
    CHECK(GattReadLength(50, 44, AttDefaultMtu) == 6);
    CHECK(GattReadLength(50, 50, AttDefaultMtu) == 0);
}

int main() {
    TestDeviceIdAtDefaultMtuIsChunked();
    TestLargeMtuSendsItAllAtOnce();
    TestExactFitNeedsNoSecondRead();
    TestOffsetAtEndVersusPastEnd();
    TestMtuBelowTheDefaultIsClamped();
    TestEmptyValue();
    TestMultiChunkMiddleIsFull();

    if (GFailures == 0) std::printf("GattLongRead tests: all passed\n");
    return GFailures == 0 ? 0 : 1;
}
