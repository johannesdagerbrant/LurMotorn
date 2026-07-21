#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Lur/Net/Session.h"  // EMsgType (the generic game slots)
#include "Lur/Sim/Tick.h"
#include "Rps/Sim.h"
#include "Rps/Tunables.h"

namespace Rps {

// RTS message-type aliases over the engine's generic game slots (#44). The engine
// names no game concept; the RTS gives 3..5 meaning here.
constexpr Lur::Net::EMsgType MsgInput       = Lur::Net::EMsgType::Game0;
constexpr Lur::Net::EMsgType MsgAnchor      = Lur::Net::EMsgType::Game1;
constexpr Lur::Net::EMsgType MsgResyncChunk = Lur::Net::EMsgType::Game2;
#if LUR_INTERNAL
// Dev-only gameplay-CVar sync (#112, Addendum C): a balance knob tweaked in the console/
// panel is stamped a few exec ticks ahead, sent, and applied on BOTH peers at that tick,
// so the tweak reaches the peer deterministically mid-match. Never compiled into shipping
// (the opcode is neither sent nor accepted there; the sim's CVars are constexpr).
constexpr Lur::Net::EMsgType MsgCvar        = Lur::Net::EMsgType::Game3;
#endif

// Lockstep coordinator for ONE peer (design doc §1-§4). Drives a Sim in lockstep with
// the other peer over a reliable, ordered datagram link — which is what BLE GATT and
// the loopback both are, so this is lockstep without UDP's hard parts (no resend, no
// loss recovery; the only failure is link death).
//
// Single-threaded by design: the two-window loopback pumps BOTH peers on the main loop
// so every net-flow bug is reproducible in a debugger with both peers visible (the
// workbench point). The threaded SimRunner is the separate single-instance path.
//
// The model (§3):
//   * Each peer owns ONE team. Local input sampled at wall tick W is scheduled to
//     EXECUTE at tick W+Delay and sent immediately; the first Delay ticks are empty by
//     convention on both sides (nothing can be scheduled earlier).
//   * Execution tracks wallclock (one exec tick per wall tick) but is GATED: tick T
//     runs only once BOTH masks for T are known. If the peer's input for T hasn't
//     arrived, the sim stalls at the ceiling (T <= peer's watermark = their tick+Delay);
//     the fast peer waits, nobody sprints past a peer.
//   * Every 10 exec ticks an Anchor re-anchors the implicit tick count AND carries a
//     truncated StateHash; a mismatch (impossible under reliable transport + a
//     deterministic sim unless there's a bug) trips Desync -> declare a draw.
class LockstepPeer {
public:
    using SendFn = void (*)(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N);

    void Init(uint64_t Seed, uint8_t MyTeam, SendFn Send, void* Ctx);

    // OR in this frame's presses; consumed (once) by the next produced wall tick.
    // Atomic: on Android the INPUT thread calls this while the SIM thread consumes it in
    // Tick (#91). Relaxed is fine — it's a lossy-OR mailbox, not an ordering barrier.
    void SetLocalMask(uint8_t Mask) { PendingLocalMask.fetch_or(Mask & 0xFu, std::memory_order_relaxed); }

    // Advance wallclock: produce + send local input for the new ticks, then execute as
    // far as the ceiling allows.
    void Tick(uint64_t ElapsedNs);

    // A datagram arrived (Input / Anchor / ResyncChunk / Cvar), dispatched by type.
    void OnMessage(Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N);

#if LUR_INTERNAL
    // Override an AffectsGameplay CVar (by its 1-byte wire id, raw value): stamps it a few
    // exec ticks ahead, sends MsgCvar, and applies it on BOTH peers at that tick to the
    // per-Sim Cv — a deterministic mid-match balance tweak. Dev-only (the console/panel/
    // desktop --tune caller). EditWallClockMs is the last-writer-wins resolver key.
    void SetGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t EditWallClockMs);
#endif

    // Reconnect (cold rejoin or blip): send our executed input history as chunks +
    // a frontier marker, and re-base our own timeline to that frontier with a fresh
    // delay pre-seed. Whichever peer is behind rebuilds from the longer history; the
    // one ahead ignores the shorter. Both end at the same frontier, bit-identical.
    // (Consistency over fairness: the survivor drops its <=Delay in-flight presses so
    // both sides agree on the delay window that spans the outage.)
    void BeginResync();
    bool AwaitingResync() const { return Awaiting; }

    const Sim& GetSim() const { return TheSim; }
    uint32_t ExecTick() const { return TheSim.Tick; }
    bool Desynced() const { return Desync; }
    bool Stalled() const { return TheSim.Tick < WallTicks; }  // behind wallclock = waiting on peer

    // Flight recording (opt-in, off by default so it costs nothing): capture the
    // executed (mask0, mask1) per tick so a fresh Sim can replay the whole match to a
    // hash-identical state — the replay law (design §1), and the post-mortem dump on a
    // desync. Both peers execute the SAME stream, so either peer's recording replays both.
    void SetRecording(bool On) { Recording = On; }
    uint64_t Seed() const { return TheSim.Seed; }
    const std::vector<uint8_t>& RecordedTeam0() const { return RecM0; }
    const std::vector<uint8_t>& RecordedTeam1() const { return RecM1; }

private:
    void ProduceAndSend(uint8_t Mask);
    void Execute();
#if LUR_INTERNAL
    // Gameplay-CVar overrides waiting to be applied, keyed by the exec tick they land on
    // (both peers hold the SAME tick->overrides once the MsgCvar is delivered). Applied to
    // TheSim.Cv just before Step(tick), so the value is in place for that whole tick.
    struct PendingCvar { uint8_t Id; int32_t Raw; uint64_t WallMs; };
    std::unordered_map<uint32_t, std::vector<PendingCvar>> PendingCvars;
    void StorePendingCvar(uint32_t Tick, uint8_t Id, int32_t Raw, uint64_t WallMs);
    void ApplyCvarsForTick(uint32_t T);
#endif
    void EmitAnchor();
    void CrossCheck(uint32_t Tick);
    void RebuildFromHistory(uint32_t Frontier);  // Incoming[0/1] -> fresh sim + timeline at Frontier
    void ReseedFrom(uint32_t Frontier);          // truncate to Frontier + a fresh Delay pre-seed

    Sim TheSim;
    Lur::Sim::TickClock Clock{TickRateHz};
    uint32_t Delay = InputDelayTicks;
    uint8_t MyTeam = 0;
    std::atomic<uint8_t> PendingLocalMask{0};  // input thread -> sim thread (#91)

    std::vector<uint8_t> LocalMasks;  // index = exec tick (pre-seeded empty for 0..Delay-1)
    std::vector<uint8_t> PeerMasks;
    uint32_t WallTicks = 0;           // local wall ticks elapsed = the execution target

    std::unordered_map<uint32_t, uint32_t> MyHash, PeerHash;  // exec tick -> truncated StateHash
    bool Desync = false;

    bool Recording = false;
    std::vector<uint8_t> RecM0, RecM1;  // executed masks per tick (only while Recording)

    bool Awaiting = false;              // in a resync exchange: don't produce/execute yet
    std::vector<uint8_t> Incoming[2];   // reassembled peer history streams (team0, team1)

    SendFn Send = nullptr;
    void* Ctx = nullptr;
};

}  // namespace Rps
