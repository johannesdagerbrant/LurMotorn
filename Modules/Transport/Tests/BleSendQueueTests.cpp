// Host tests for Lur::Transport::BleSendQueue — the send flow control that used to be ~60 lines
// of hand-rolled netcode inside BleShim.kt, where no test could reach it (and where it drifted
// between the two games).
//
// The policy under test, restated from the Kotlin it replaces: a BLE stack allows exactly ONE
// outstanding operation, and a second write issued before the first completes is SILENTLY
// dropped. Under load that dropped nearly every datagram and state only propagated via the
// slower resync (#72). So: enqueue, issue one, issue the next only on completion — plus a
// bounded watchdog, because a link that dies between issue and completion would otherwise stall
// the queue forever.
//
// Every one of those sentences is a DECISION, which is why it belongs in engine C++ rather than
// in a platform file. The fake radio below is the whole point: it can refuse a write, lose a
// completion, or vanish, on demand — none of which a real radio does when you ask it to.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Transport/BleSendQueue.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// A radio that does exactly what the test tells it to. `Accept` models the stack momentarily
// refusing a write (the real one returns false when busy); `Sent` records what actually left,
// in order, so ordering is asserted on the wire and not on the queue's internals.
class FakeBleRadio final : public IBleRadio {
public:
    bool Accept = true;
    std::vector<std::vector<uint8_t>> Sent;

    bool Write(const uint8_t* Data, std::size_t Size) override {
        ++WriteAttempts;
        if (!Accept) return false;
        Sent.emplace_back(Data, Data + Size);
        return true;
    }

    int WriteAttempts = 0;
};

static void Push(BleSendQueue& Q, uint8_t Tag) {
    const uint8_t Bytes[2] = {Tag, 0xAA};
    CHECK(Q.Enqueue(Bytes, sizeof(Bytes)));
}

static void PushFast(BleSendQueue& Q, uint8_t Tag) {
    const uint8_t Bytes[2] = {Tag, 0xAA};
    CHECK(Q.Enqueue(Bytes, sizeof(Bytes), EBleSendPriority::Expedited));
}

static uint8_t TagOf(const std::vector<uint8_t>& V) { return V.empty() ? 0 : V[0]; }

// ONE outstanding operation. The second datagram must not reach the radio until the first
// completes — this is the whole reason the queue exists.
static void TestOneInFlightAtATime() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);
    Push(Q, 2);
    CHECK(Radio.Sent.size() == 1);          // only the first went out
    CHECK(TagOf(Radio.Sent[0]) == 1);
    CHECK(Q.InFlight());
    CHECK(Q.Queued() == 1);

    Q.OnSendComplete();
    CHECK(Radio.Sent.size() == 2);
    CHECK(TagOf(Radio.Sent[1]) == 2);
    CHECK(Q.InFlight());                    // ...and now the SECOND one is the outstanding one
    CHECK(Q.Queued() == 0);

    Q.OnSendComplete();
    CHECK(!Q.InFlight());                   // only now is the radio idle
}

// FIFO. A datagram stream is order-sensitive, so the queue may never reorder.
static void TestFifoOrder() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    for (uint8_t i = 1; i <= 5; ++i) Push(Q, i);
    for (int i = 0; i < 5; ++i) Q.OnSendComplete();

    CHECK(Radio.Sent.size() == 5);
    for (uint8_t i = 0; i < 5; ++i) CHECK(TagOf(Radio.Sent[i]) == i + 1);
}

// A refused write is NOT a lost datagram: the radio said "busy", so it stays queued and goes
// out on the next opportunity, still in order.
static void TestRefusedWriteStaysQueued() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Radio.Accept = false;
    Push(Q, 1);
    CHECK(Radio.Sent.empty());
    CHECK(!Q.InFlight());                   // nothing is outstanding — the radio took nothing
    CHECK(Q.Queued() == 1);                 // ...and nothing was dropped

    Radio.Accept = true;
    Q.Tick(1'000'000ull);                   // a tick re-pumps
    CHECK(Radio.Sent.size() == 1);
    CHECK(TagOf(Radio.Sent[0]) == 1);
}

// The completion callback can simply never arrive — the link died between issue and ack. Without
// a bound the queue stalls forever, which on a phone reads as "the game froze". After the
// timeout the queue gives up on that datagram and moves on.
static void TestWatchdogResumesAfterALostCompletion() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);
    Push(Q, 2);
    CHECK(Radio.Sent.size() == 1);

    Q.Tick(BleSendQueue::SendTimeoutNs / 2);
    CHECK(Radio.Sent.size() == 1);          // not yet — the timeout is a bound, not a hurry

    Q.Tick(BleSendQueue::SendTimeoutNs);     // now past it
    CHECK(Radio.Sent.size() == 2);
    CHECK(TagOf(Radio.Sent[1]) == 2);
}

// The token bug shape, made explicit. A completion that arrives AFTER the watchdog already gave
// up must not pump a second time — that would put two datagrams in flight, which is exactly the
// state the whole queue exists to prevent.
static void TestLateCompletionAfterWatchdogDoesNotDoublePump() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);
    Push(Q, 2);
    Push(Q, 3);
    CHECK(Radio.Sent.size() == 1);

    Q.Tick(BleSendQueue::SendTimeoutNs + 1);   // watchdog fires, issues #2
    CHECK(Radio.Sent.size() == 2);

    Q.OnSendComplete();                        // the LATE completion for #1
    CHECK(Radio.Sent.size() == 2);             // must NOT also release #3
    CHECK(Q.InFlight());                       // #2 is still the one outstanding

    Q.OnSendComplete();                        // #2's own completion
    CHECK(Radio.Sent.size() == 3);
}

// No link means the backlog is meaningless: a datagram stream only makes sense against a peer
// that received the earlier ones. Keeping it would deliver a burst of stale state on reconnect.
static void TestLinkLossDropsTheBacklog() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);
    Push(Q, 2);
    Push(Q, 3);
    CHECK(Q.Queued() == 2);

    Q.OnLinkLost();
    CHECK(Q.Queued() == 0);
    CHECK(!Q.InFlight());

    // ...and the queue is usable again immediately, without a reset call.
    Push(Q, 9);
    CHECK(Radio.Sent.size() == 2);
    CHECK(TagOf(Radio.Sent[1]) == 9);
}

// #190: a latency-critical datagram JUMPS the queue. The queue is otherwise FIFO with no notion
// of urgency, so a move could sit behind a keepalive or — much worse — behind a multi-datagram
// resync payload, which is exactly when the queue is deepest and latency is felt most. Each wait
// costs a whole connection interval.
//
// Priority is passed EXPLICITLY. Both platform backends used to infer it from the datagram's
// LENGTH ("1 byte == a move"), which coupled a transport to one game's wire format and then died
// silently when that format changed — see the class comment in BleSendQueue.h.
static void TestExpeditedJumpsTheQueue() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);                             // goes out immediately (nothing in flight)
    Push(Q, 2);                             // queued
    Push(Q, 3);                             // queued behind 2
    PushFast(Q, 9);                         // must overtake BOTH
    CHECK(Radio.Sent.size() == 1);

    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[1]) == 9);       // the urgent one, ahead of 2 and 3
    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[2]) == 2);       // ...and the normal ones keep their own order
    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[3]) == 3);
}

// Expedited datagrams do not overtake EACH OTHER: within a priority class the queue stays FIFO.
// Chess relies on this indirectly (it is turn-alternating, so it never has two moves in flight),
// but a game that does send two urgent datagrams back to back must not have them reordered.
static void TestExpeditedKeepsFifoAmongItself() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);                             // in flight
    Push(Q, 2);                             // normal, queued
    PushFast(Q, 7);
    PushFast(Q, 8);

    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[1]) == 7);
    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[2]) == 8);       // 8 after 7, not before it
    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[3]) == 2);       // the normal one last
}

// An expedited datagram cannot pull back the one already handed to the radio — that one is gone.
static void TestExpeditedDoesNotPreemptTheInFlightDatagram() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);
    CHECK(Radio.Sent.size() == 1);
    CHECK(TagOf(Radio.Sent[0]) == 1);

    PushFast(Q, 9);
    CHECK(Radio.Sent.size() == 1);          // still exactly one outstanding
    Q.OnSendComplete();
    CHECK(TagOf(Radio.Sent[1]) == 9);
}

// A link that dies while a completion is owed must not carry that debt into the NEXT link. If
// it did, the fresh link's first real completion would be swallowed as if it were the ghost of
// the old one, and the new queue would stall for a full watchdog period on its very first
// datagram — the worst possible moment, right when the peers are resyncing.
static void TestAbandonedDebtDoesNotSurviveLinkLoss() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    Push(Q, 1);
    Q.Tick(BleSendQueue::SendTimeoutNs + 1);   // watchdog abandons #1 -> one completion owed
    Q.OnLinkLost();                            // ...and the link dies before it lands

    Push(Q, 2);
    CHECK(Radio.Sent.size() == 2);
    Q.OnSendComplete();                        // a REAL completion for #2, not a ghost
    CHECK(!Q.InFlight());                      // so it must be honoured, not swallowed

    Push(Q, 3);
    CHECK(Radio.Sent.size() == 3);             // and the queue keeps moving
}

// Fixed capacity, and a full queue REFUSES rather than dropping. Dropping the oldest would
// silently break the stream's ordering guarantee; refusing tells the caller, which is the same
// choice Session::Send makes for an over-long payload.
static void TestFullQueueRefusesRatherThanDropping() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);
    Radio.Accept = false;                      // nothing drains, so the queue fills

    const uint8_t Byte = 0x7F;
    int Accepted = 0;
    for (int i = 0; i < BleSendQueue::MaxQueued + 8; ++i)
        if (Q.Enqueue(&Byte, 1)) ++Accepted;

    CHECK(Accepted == BleSendQueue::MaxQueued);
    CHECK(Q.Queued() == BleSendQueue::MaxQueued);
    CHECK(Q.Dropped() == 8);                   // counted, so a full queue is diagnosable
}

// An over-long datagram is refused outright — it cannot fit one BLE write, and truncating the
// wire is never acceptable.
static void TestOversizedDatagramRefused() {
    FakeBleRadio Radio;
    BleSendQueue Q;
    Q.SetRadio(&Radio);

    std::vector<uint8_t> Big(BleSendQueue::MaxDatagram + 1, 0xEE);
    CHECK(!Q.Enqueue(Big.data(), Big.size()));
    CHECK(Q.Queued() == 0);
    CHECK(Radio.Sent.empty());

    std::vector<uint8_t> Exact(BleSendQueue::MaxDatagram, 0xEE);
    CHECK(Q.Enqueue(Exact.data(), Exact.size()));
    CHECK(Radio.Sent.size() == 1);
    CHECK(Radio.Sent[0].size() == BleSendQueue::MaxDatagram);
}

// With no radio attached the queue must not crash or silently pretend to send.
static void TestNoRadioIsSafe() {
    BleSendQueue Q;
    const uint8_t Byte = 1;
    CHECK(Q.Enqueue(&Byte, 1));
    CHECK(Q.Queued() == 1);
    CHECK(!Q.InFlight());
    Q.Tick(BleSendQueue::SendTimeoutNs * 4);
    CHECK(Q.Queued() == 1);                    // still held, nothing invented
}

int main() {
    TestOneInFlightAtATime();
    TestFifoOrder();
    TestRefusedWriteStaysQueued();
    TestWatchdogResumesAfterALostCompletion();
    TestLateCompletionAfterWatchdogDoesNotDoublePump();
    TestExpeditedJumpsTheQueue();
    TestExpeditedKeepsFifoAmongItself();
    TestExpeditedDoesNotPreemptTheInFlightDatagram();
    TestLinkLossDropsTheBacklog();
    TestAbandonedDebtDoesNotSurviveLinkLoss();
    TestFullQueueRefusesRatherThanDropping();
    TestOversizedDatagramRefused();
    TestNoRadioIsSafe();

    if (GFailures == 0) {
        std::printf("ble_send_queue_tests: all checks passed\n");
        return 0;
    }
    std::printf("ble_send_queue_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
