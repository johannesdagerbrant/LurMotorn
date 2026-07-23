#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
constexpr Lur::Net::EMsgType MsgCvarSync    = Lur::Net::EMsgType::Game4;
constexpr Lur::Net::EMsgType MsgFingerprint = Lur::Net::EMsgType::Game5;
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

    // #137: queue a local input EVENT (place/queue) for the next produced wall tick — replaces
    // the 4-bit SetLocalMask. Thread-safe (the Android INPUT thread queues while the SIM thread
    // drains it in Tick, #91): a mutex-guarded inbox, fine for the human tap rate. The Team on
    // the event is overwritten with MyTeam here, so the UI can't spoof the peer's team.
    void QueueLocalEvent(InputEvent E);

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

    // Match-start sync (Addendum C.3): seed this peer's pre-match overrides (typically the
    // persisted cvars.cfg set), then SendCvarSync() before tick 0. Both peers exchange
    // their full sets, merge with the last-writer-wall-clock resolver (timestamp collision
    // -> compile-time default), and apply the identical merged set before simulating — so
    // one designer's tuning propagates to the peer, deterministically.
    void SeedGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t EditWallClockMs);
    void SendCvarSync();

    // Thread-safe: enqueue a gameplay-CVar edit from the UI/glue thread. Drained on the sim
    // thread at the top of Tick() into SetGameplayCvar (send + stamp). Rare (human taps), so
    // a mutex is fine. This is the ONLY Lp method — besides SetLocalMask — safe off the sim
    // thread; the numpad/console commit routes through here.
    void QueueGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t EditWallClockMs);

    // Build-fingerprint gate (Addendum C.3): exchange a compile-time fingerprint (git
    // commit + dirty + config, LUR_BUILD_FP) at connect and refuse the match on mismatch,
    // BEFORE tick 0 — the proactive form of the reactive anchor-hash desync alarm, and what
    // makes the 1-byte GameplayId agreement safe (identical builds => identical CVar list).
    void SendFingerprint();
    bool BuildMismatch() const { return BuildMismatch_; }
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
    // #137: the executed COMBINED per-tick event batch (team0's events then team1's) — one
    // stream now (StepEvents takes one batch), replacing the two mask vectors. Replay feeds
    // each tick's batch back through StepEvents on a fresh Sim to a hash-identical state.
    const std::vector<std::vector<InputEvent>>& RecordedEvents() const { return RecEvents; }

private:
    void ProduceAndSend(const std::vector<InputEvent>& Batch);
    void Execute();
#if LUR_INTERNAL
    // Gameplay-CVar overrides waiting to be applied, keyed by the exec tick they land on
    // (both peers hold the SAME tick->overrides once the MsgCvar is delivered). Applied to
    // TheSim.Cv just before Step(tick), so the value is in place for that whole tick.
    struct PendingCvar { uint8_t Id; int32_t Raw; uint64_t WallMs; };
    std::unordered_map<uint32_t, std::vector<PendingCvar>> PendingCvars;
    void StorePendingCvar(uint32_t Tick, uint8_t Id, int32_t Raw, uint64_t WallMs);
    void ApplyCvarsForTick(uint32_t T);

    // This peer's current override set (id -> value + edit wall-clock), relative to the
    // compile-time defaults. Seeded pre-match and updated by live tweaks; exchanged +
    // merged at match start (MsgCvarSync). Reverting an id to default = erasing it here.
    struct CvarVal { int32_t Raw; uint64_t WallMs; };
    std::unordered_map<uint8_t, CvarVal> ActiveCvars;
    void MergeCvar(uint8_t Id, int32_t Raw, uint64_t WallMs);  // resolver: last-writer; tie -> default
    void ApplyActiveCvars();  // TheSim.Cv = defaults, then overlay ActiveCvars (pre-tick-0)

    bool BuildMismatch_ = false;  // peer reported a different LUR_BUILD_FP at connect

    std::mutex               CvQueueMutex_;  // UI thread -> sim thread edit inbox
    std::vector<PendingCvar> CvQueue_;
    void DrainCvarQueue();  // sim thread: apply queued UI edits via SetGameplayCvar
#endif
    void EmitAnchor();
    void CrossCheck(uint32_t Tick);
    void RebuildFromHistory(uint32_t Frontier);  // Incoming[0/1] -> fresh sim + timeline at Frontier
    void ReseedFrom(uint32_t Frontier);          // truncate to Frontier + a fresh Delay pre-seed

    Sim TheSim;
    Lur::Sim::TickClock Clock{TickRateHz};
    uint32_t Delay = InputDelayTicks;
    uint8_t MyTeam = 0;
    std::mutex EventQueueMutex_;              // input thread -> sim thread inbox (#91)
    std::vector<InputEvent> PendingLocalEvents;

    // index = exec tick; each entry is that tick's event batch for the team (pre-seeded empty
    // for ticks 0..Delay-1, the by-convention empty delay window on both peers).
    std::vector<std::vector<InputEvent>> LocalEvents;
    std::vector<std::vector<InputEvent>> PeerEvents;
    uint32_t WallTicks = 0;           // local wall ticks elapsed = the execution target

    std::unordered_map<uint32_t, uint32_t> MyHash, PeerHash;  // exec tick -> truncated StateHash
    bool Desync = false;

    bool Recording = false;
    std::vector<std::vector<InputEvent>> RecEvents;  // executed combined batch per tick (while Recording)

    bool Awaiting = false;                            // in a resync exchange: don't produce/execute yet
    std::vector<std::vector<InputEvent>> IncomingHistory;  // reassembled peer combined-batch history

    SendFn Send = nullptr;
    void* Ctx = nullptr;
};

}  // namespace Rps
