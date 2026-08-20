#pragma once
// The two-peer lockstep test harness, shared by every host suite that needs a pair of
// LockstepPeers on one process.
//
// This lived inside NetTests.cpp until the two-peer soak (#211) needed the same pieces. Rather than
// grow a second copy — which is how RPS ended up with four touch dispatches and 299 lines of drift
// between the two BLE shims — it moved here on the SECOND consumer, per the promotion rule. Both
// suites now build the pair the same way, so a fault helper fixed in one is fixed in both.
//
// Deliberately NOT a Modules/ facility: it knows Rps::InputEvent, the RPS map rules and the RPS
// camp handshake. It is test scaffolding for one game, and the engine wall stays where it is.
#include <cstdint>
#include <utility>
#include <vector>

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Rps/EventCodec.h"
#include "Rps/LockstepPeer.h"

namespace Rps::Harness {

using Lur::Serialization::BitReader;
using Lur::Serialization::BitWriter;

// One wall tick at TickRateHz (10). Tick(OneTickNs) therefore advances exactly one tick, which is
// what lets a test index faults by tick number.
inline constexpr uint64_t OneTickNs = 100'000'000ull;

// ONE definition of the tests' opening-camp spot, because it has to satisfy several map rules at
// once and a scattered magic coordinate silently rots when any of them moves (#157 did exactly
// that — the old (17, 14) fell inside the widened mine clearance and every match-start test failed
// at once, looking like a lockstep bug rather than a stale coordinate). It must be:
//   * >= Cv.MineClearance (7) from every live mine. X=17 sits midway between the mine columns at
//     14 and 20, so dx=3 and the end mine row at Y=9 forces dy >= sqrt(49-9) = 6.33 -> Y >= 15.4.
//   * >= 2 x footprint from the HQ, which auto-places at InitialFrontier x 3/4 = 26.25.
//   * inside the team's opening frontier (<= 35 from its own end).
// Y=16 clears all three with margin; mirrored for team 1.
inline const Fixed CampTestX = F(17);
inline Fixed CampTestY(uint8_t Team) { return Team == 0 ? F(16) : F(WorldHeight.ToInt() - 16); }

// ---- A QUEUED link (models the real deferred delivery / Pump, and avoids the synchronous
// re-entrancy hazard a naive loopback has). ----
struct Outbox {
    std::vector<std::pair<Lur::Net::EMsgType, std::vector<uint8_t>>> Q;
};
inline void Enqueue(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N) {
    static_cast<Outbox*>(Ctx)->Q.emplace_back(Type, std::vector<uint8_t>(Data, Data + N));
}
inline void Deliver(Outbox& From, LockstepPeer& To) {
    for (auto& M : From.Q) To.OnMessage(M.first, M.second.data(), M.second.size());
    From.Q.clear();
}

// Deliver everything EXCEPT the Nth MsgInput frame — a lost produced tick inside a link that is
// otherwise working perfectly, which is what #163 observed on hardware (iPhone->Galaxy notifications
// stopped while writes kept landing, and separately an input the iPhone executed at tick 4528 was
// simply absent from the Galaxy's stream with no transport complaint at all).
inline void DeliverDroppingNthInput(Outbox& From, LockstepPeer& To, int NthInput) {
    int Seen = 0;
    for (auto& M : From.Q) {
        if (M.first == MsgInput && Seen++ == NthInput) continue;  // the frame that never arrives
        To.OnMessage(M.first, M.second.data(), M.second.size());
    }
    From.Q.clear();
}

// Deliver every MsgInput frame TWICE — the opposite fault to the one above, and the one actually
// caught on hardware 2026-08-01: the Android central had established TWO GATT connections to the same
// iPhone peripheral and subscribed to notifications on both, so every produced frame the peripheral
// notified arrived at the app twice. Writes (central->peripheral) go out from one place and were
// clean, which is why the damage was one-directional in exactly the direction #163 documents.
inline void DeliverDuplicatingInputs(Outbox& From, LockstepPeer& To) {
    for (auto& M : From.Q) {
        To.OnMessage(M.first, M.second.data(), M.second.size());
        if (M.first == MsgInput) To.OnMessage(M.first, M.second.data(), M.second.size());
    }
    From.Q.clear();
}

// Re-encode ONE of the sender's produced frames with an extra event injected, keeping the sequence
// byte and appending rather than replacing — so nothing looks lost, no gap is reported, and the two
// sims genuinely diverge. That is the shape #159 captured on hardware: a long clean match, then one
// peer's executed stream containing an event the other's does not, with the link reporting nothing.
// Injecting a real divergence matters — a test that only forges a bad ANCHOR proves the detector
// fires but cannot prove two different states are brought back together.
//
// SlotOverride: which building slot the forged Queue event targets. The default (-1) uses the
// opening camp of ForgedTeam, which is slot 2/3 — Init auto-places the two home bases at slots 0/1
// (#146) and tick 0's two camps land next. Queueing at the camp really does move state (gold down,
// queue up); queueing at the HOME BASE is rejected, so it would inject nothing and the caller would
// prove nothing. A soak that has built past the opening camp can name a slot it knows is live.
inline bool TamperOneInput(Outbox& From, LockstepPeer& To, uint8_t ForgedTeam, int SlotOverride = -1) {
    const uint8_t Slot = SlotOverride >= 0 ? static_cast<uint8_t>(SlotOverride)
                                           : static_cast<uint8_t>(ForgedTeam == 0 ? 2 : 3);
    bool Did = false;
    for (auto& M : From.Q) {
        if (!Did && M.first == MsgInput) {
            BitReader R(M.second.data(), M.second.size());
            const uint32_t Seq = R.ReadBits(8);
            InputEvent Buf[MaxEventsPerTick];
            const int Cnt = DecodeEventBatch(R, Buf, MaxEventsPerTick);
            if (Cnt >= 0 && Cnt + 1 < MaxEventsPerTick) {
                Buf[Cnt] = InputEvent::Queue(ForgedTeam, Slot, 4);
                BitWriter W;
                W.WriteBits(Seq, 8);
                EncodeEventBatch(W, Buf, Cnt + 1);
                const std::vector<uint8_t>& B = W.Finish();
                To.OnMessage(MsgInput, B.data(), B.size());
                Did = true;
                continue;
            }
        }
        To.OnMessage(M.first, M.second.data(), M.second.size());
    }
    From.Q.clear();
    return Did;
}

// Drive both peers to the SAME head so raw head-state hashes are comparable. Under rollback the two
// run on independent wall clocks and their speculative heads can sit a tick apart (most visibly after
// a staggered recovery reseed) even though their CONFIRMED timelines already agree — lockstep gave
// equal heads for free, rollback needs this. Ticks whichever peer is behind (speculating the idle
// peer forward) until the heads meet, delivering both ways each step.
inline void SettleUntilEqual(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb) {
    for (int I = 0; I < 64 && A.ExecTick() != B.ExecTick(); ++I) {
        if (A.ExecTick() <= B.ExecTick()) A.Tick(OneTickNs);
        if (B.ExecTick() <= A.ExecTick()) B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
}

// #139: drive both peers through the pre-match placement handshake — each places its mining camp
// (its "ready"), the camps are exchanged, and the match starts from tick 0 with both camps in.
// Tests that don't otherwise place a camp call this so the clock actually starts.
inline void PlaceCampsAndStart(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb) {
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 4 && !(A.MatchStarted() && B.MatchStarted()); ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
}

}  // namespace Rps::Harness
