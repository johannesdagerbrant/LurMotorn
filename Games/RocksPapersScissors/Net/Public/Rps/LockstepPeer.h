#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Lur/Net/Session.h"  // EMsgType (the generic game slots)
#include "Lur/Sim/Tick.h"
#include "Rps/Sim.h"
#include "Rps/SnapshotRing.h"  // rollback snapshot ring + peer predictor
#include "Rps/Tunables.h"

namespace Rps {

// RTS message-type aliases over the engine's generic game slots (#44). The engine
// names no game concept; the RTS gives 3..5 meaning here.
constexpr Lur::Net::EMsgType MsgInput       = Lur::Net::EMsgType::Game0;
constexpr Lur::Net::EMsgType MsgAnchor      = Lur::Net::EMsgType::Game1;
constexpr Lur::Net::EMsgType MsgResyncChunk = Lur::Net::EMsgType::Game2;
// #160: the pre-match opening camp (and its #149 re-sends) — a channel of its own, NOT a MsgInput
// batch. Splitting it is the whole fix: the two used to share MsgInput and be told apart by reading
// the payload, and a produced tick can be byte-identical to a re-send (a player re-placing a camp
// where theirs already stands). Now MsgInput means "a produced tick" with no exceptions, so the
// receiver never has to guess, and MsgCamp is idempotent by construction — it is never buffered as
// a tick, so any number of re-sends cannot shift the timeline.
constexpr Lur::Net::EMsgType MsgCamp        = Lur::Net::EMsgType::Game6;
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
    // per-Sim Cv — a deterministic mid-match balance tweak. Dev-only; the console is the
    // only caller. EditWallClockMs is the last-writer-wins resolver key.
    void SetGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t EditWallClockMs);

    // Match-start sync (Addendum C.3): seed this peer's pre-match overrides (typically the
    // persisted cvars.cfg set), then SendCvarSync() before tick 0. Both peers exchange
    // their full sets, merge with the last-writer-wall-clock resolver (timestamp collision
    // -> compile-time default), and apply the identical merged set before simulating — so
    // one designer's tuning propagates to the peer, deterministically.
    //
    // #169: a main calls SendCvarSync ONCE, next to Lp.Init — and that is not enough on its own,
    // so BeginResync re-offers the set too. One offer per peer only converges when both peers
    // Init while the link is up; when one app restarts, the incumbent's Init has long passed and
    // it never re-sends, leaving the rejoiner on compile-time defaults for the rest of the
    // session. Offers are only honoured BEFORE tick 0 (a mid-match set has no agreed apply tick —
    // use SetGameplayCvar, which stamps one).
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

    // ---- #161: a desync RECOVERS the match; it does not end it ----
    // e6d6abf declared a draw when the anchor cross-check tripped. That was a stopgap for a worse bug
    // (both phones froze forever with no message) and explicitly not the wanted behaviour: it throws
    // the match away, and a draw is a lie about what happened.
    //
    // Who is right, with no referee and no host: the peer whose device GUID is lower keeps its
    // timeline; the other rebuilds from it. MyTeam already IS that comparison — every path derives
    // Team = MyGuid < PeerGuid ? 0 : 1 — so both peers reach the same verdict locally with nothing
    // negotiated, which is what a tie-break has to be when the two sides disagree about reality.
    //
    // How: replay the winner's INPUT HISTORY, not a state transfer. The history is already chunked for
    // the reconnect path and is kilobytes; a Sim is hundreds of KB, which over BLE is a ~100 s stall.
    // The trade is that replay converges when the cause was a LOST INPUT (which #159 showed it is, at
    // least sometimes) and cannot when the cause is genuine nondeterminism — it faithfully reproduces
    // that. Hence the bound below. Note it converges regardless of WHICH peer lost data, because both
    // end up replaying one identical history; the discarded timeline may have been the more complete
    // one. That is deliberate — consistency, not fairness (CLAUDE.md), and the players share a room.
    static constexpr int MaxDesyncRecoveries = 3;
    // How long to wait before starting the NEXT recovery round once a budget is spent. Doubles per
    // round from 1 s and caps at 15 s.
    //
    // The cap is the point, not the growth. #210 records the alternative: a reconnect/resync cycle
    // firing every ~11 s for eight minutes without ever widening, which is a livelock rather than a
    // retry. A pair that cannot converge should settle into a slow, quiet re-attempt that costs
    // nothing and still recovers the instant the peer becomes able to answer.
    static constexpr uint64_t RecoveryRetryBaseNs = 1'000'000'000ull;
    static constexpr uint64_t RecoveryRetryMaxNs  = 15'000'000'000ull;
    static uint64_t RecoveryRetryBackoffNs(int Round) {
        uint64_t Ns = RecoveryRetryBaseNs;
        for (int I = 1; I < Round && Ns < RecoveryRetryMaxNs; ++I) Ns *= 2;
        return Ns > RecoveryRetryMaxNs ? RecoveryRetryMaxNs : Ns;
    }
    // #167: the LOST-FRAME path gets its OWN, more generous bound instead of sharing the one above.
    // The two are not the same kind of event. An anchor mismatch means the sims already disagree and
    // replay may not converge — three attempts and then a draw is right. A lost frame is repaired
    // BEFORE the hole executes, demonstrably converges (395 ms on hardware), and is caused by the
    // radio rather than by the sim; charging it to the desync budget means an ordinary restart or a
    // flaky link leaves the match one attempt from a draw with nothing actually wrong.
    //
    // 2026-08-01 made the case unarguable: a duplicate-delivery fault (#163) reported a false gap on
    // every tick and spent all three attempts within the first three ticks of the match. After that
    // RequestRecovery was a silent no-op for twenty minutes — so the ONE mechanism that repairs a real
    // lost frame was disabled by noise, in exactly the match it existed to protect.
    static constexpr int MaxGapRecoveries = 16;
    // #162: how long execution may sit at the ceiling waiting for peer frames before the match is
    // given up. This was the ONE hold in the netcode with no bound, and that is how a load collapse
    // became terminal: under ~1600 units the sim breached the tick budget, the radio timed out, the
    // reconnect/resync loop never converged, and the two peers ended up in DIFFERENT MATCHES — one
    // frozen at the ceiling at tick 5195 "waiting for peer input that will never come", the other
    // restarted and sitting pre-match. Nothing reconciled that; both phones were dead to the players.
    //
    // Ending the match instead sends both sides back to the camp handshake, the one state that always
    // re-converges. Generous on purpose: a peer this far behind is already unplayable, and the
    // transport's own 5 s link timeout fires long before this does — so reaching it means the peer is
    // not merely slow. Re-armed by any progress, so a phone that briefly falls behind is not punished.
    static constexpr uint64_t CeilingStallTimeoutNs = 20'000'000'000ull;
    // A recovery that never completes is the freeze again by another name, so the wait is bounded too
    // (the winner's history may never arrive — a dead peer, a lost chunk).
    static constexpr uint64_t DesyncRecoveryTimeoutNs = 4'000'000'000ull;

    // A recovery is in flight: state is not trusted and nothing is executed. The view shows this as
    // "resyncing" — a recovery that silently rewinds several seconds of play reads as a glitch or as
    // cheating, so the player has to be told something is being repaired.
    bool Recovering() const { return Recovering_; }

    // Rollback diagnostics (Docs/Journal/2026-08-03). Rollbacks() counts how many times a delivered
    // peer frame contradicted the "peer idle" prediction and forced a restore+re-simulate; ResimTicks()
    // is the total ticks re-simulated across all of them. These ARE the plan's §"correction frequency"
    // and worst-case resim-cost measurements — sparse human input should keep both low. Per match
    // (reset in BeginMatch).
    int Rollbacks() const { return Rollbacks_; }
    uint32_t ResimTicks() const { return ResimTicks_; }
    // Recoveries attempted THIS match (reset by BeginMatch). Reaching MaxDesyncRecoveries is what
    // finally declares the draw — that is the only place a draw legitimately lives.
    int RecoveryAttempts() const { return RecoveryAttempts_; }
    // How many times a recovery BUDGET has been spent. Effort is bounded per round; rounds are
    // not, because the match must always be able to recover (owner ruling, 2026-08-16).
    int RecoveryRounds() const { return RecoveryRounds_; }
    // #167: gap repairs attempted THIS match, bounded separately by MaxGapRecoveries. Split out so a
    // lost frame can never push the match toward the draw that MaxDesyncRecoveries declares.
    int GapRecoveries() const { return GapRecoveries_; }
    // #163: produced frames discarded as duplicates/reorders — a frame whose sequence sits BEHIND the
    // one we expect. Exposed because it is the difference between "the link lost data" (InputGaps) and
    // "the link delivered data twice", which are opposite faults that looked identical before.
    int DuplicateFrames() const { return DuplicateFrames_; }

    // #148: how long a peer may hold production/execution waiting for the other side's frontier
    // marker before resuming on its own state. A restarted app cannot send that marker (Session
    // fires its resync handler only on a reconnect EDGE, which a fresh launch never takes), and
    // without this bound the survivor was wedged permanently.
    static constexpr uint64_t ResyncStallTimeoutNs = 3'000'000'000ull;

    // #163: how long pre-match "we are ready, the peer is not" may last before it is called out. The
    // half-open link produced exactly that state and it read as a frozen app: one peer at started=1
    // and advancing, the other at started=0 with untouched starting gold because the peer's camp never
    // arrived. Generous on purpose — the other player may simply be thinking about where to build —
    // so this is the point past which SILENCE is the more likely explanation than deliberation.
    static constexpr uint64_t PreMatchStallWarnNs = 8'000'000'000ull;

    // #149: pre-match, the local camp is RE-SENT on this period until the peer's camp comes back.
    // A single send was enough only while both peers entered the match together. Across a
    // post-match restart they re-init a few ms to seconds apart, so peer A's camp could arrive
    // while B was still on its win screen — B buffered it as the OLD match's input and B's own
    // restart then cleared it, leaving both sides waiting on a camp neither would send again
    // (the arrival-before-reinit family of #147/#148). Re-sending makes the exchange
    // self-healing regardless of who restarts first.
    static constexpr uint64_t PreMatchCampResendNs = 500'000'000ull;

#if LUR_AGENT
    // ---- Assistant-only fault injection (CLAUDE.md's LUR_AGENT axis) ----
    // Absent from every config including Development, force-zeroed in Shipping. These exist because
    // the two failure modes this netcode was rebuilt around cannot be produced on demand from the
    // outside: a BLE link does not drop a single frame when asked, and a deterministic sim does not
    // diverge when asked. Without them the recovery paths could only be tested on the host, and the
    // hardware bug they were written for (#159, once, after 13.5 minutes) is not reproducible by
    // waiting. Injecting the fault is the only way to prove the repair works on a real pair.

    // Silently drop the next N PRODUCED input frames — the datagram is never handed to Send. This is
    // the #163 half-open link in miniature: the sender believes it sent, the receiver never sees it,
    // and nothing in the transport reports an error. The receiver's sequence check should then name
    // the missing tick and #161 should repair the timeline before the hole is executed.
    void AgentDropOutgoing(int Frames) { AgentDropTx_ = Frames > 0 ? Frames : 0; }
    int  AgentDroppedRemaining() const { return AgentDropTx_; }

    // Diverge this peer's state on purpose, so the anchor cross-check has something real to catch.
    // Gold because it is hashed, trivially observable on the LOCKSTEP line, and cannot crash anything.
    // NOTE this is NOT in the input history, so replaying the survivor's history cannot undo it — that
    // makes it the NONDETERMINISM shape rather than the lost-input shape, and therefore the case that
    // must exhaust the recovery budget and end in the bounded draw.
    void AgentCorruptState(int32_t GoldDelta) { TheSim.Teams[MyTeam].Gold += GoldDelta; }
#endif

    const Sim& GetSim() const { return TheSim; }
    uint32_t ExecTick() const { return TheSim.Tick; }

    // Rollback scaffolding (Docs/Journal/2026-08-03, Phase 1): the CONFIRMED tick — the highest tick
    // whose combined input BOTH peers have really produced, so both sims agree on it beyond any doubt.
    // This is the frontier rollback treats as immutable (never rolled back past; the snapshot ring
    // retires below it). Defined in terms of the two input timelines:
    //   * local:  LocalEvents holds a real (produced, or the by-convention empty delay pre-seed) entry
    //             for every index in [0, LocalEvents.size()) — so the last known local tick is size-1.
    //   * peer:   PeerTickNext_ is the exec tick of the next REAL peer frame still owed, so the last
    //             known peer tick is PeerTickNext_ - 1. Using PeerTickNext_ rather than
    //             PeerEvents.size() is deliberate and forward-looking: when Phase 2 speculates the peer
    //             with PredictPeerBatch, PeerEvents will grow with PREDICTED entries that must NOT count
    //             as confirmed, while PeerTickNext_ still advances only on a delivered frame.
    // The confirmed tick is the lesser of the two. Returns -1 before anything is confirmed (an
    // int64_t, not a uint32_t, so "nothing yet" is unambiguous rather than colliding with tick 0).
    // Read-only and behaviour-neutral in Phase 1 — the execution model still gates on the ceiling.
    int64_t ConfirmedTick() const {
        const uint32_t Known = ConfirmedFrontier();  // min(local produced, peer received): both real
        return Known == 0 ? -1 : static_cast<int64_t>(Known) - 1;
    }
    // A hash mismatch was seen at an anchor. NOT a terminal state any more: CrossCheck declares the
    // match a DRAW when it trips, so the normal post-match hold + restart runs and clears this. Read
    // it for diagnostics ("that draw was a desync, not a real draw"), not as "the session is over".
    // The draw is a STOPGAP — **#161** replaces it with actually recovering the match.
    bool Desynced() const { return Desync; }
    // #204: how many anchor mismatches this match has DETECTED — sticky for the life of the match,
    // where Desynced() above is a live gate that clears as soon as the repair is under way.
    //
    // These answer different questions and the difference bit us. `Desynced()` is what every
    // diagnostic printed as `desync=`, and a human reads that as "did this match diverge" — but
    // BeginRecovery clears it IMMEDIATELY on the survivor (its timeline stands, so there is nothing to
    // gate), while the adopting peer holds it until the rebuild converges. So one genuine divergence
    // was reported by the two phones as `desync=1` and `desync=0`: the survivor's screen said the match
    // was clean. Under the consistency rule a contested outcome must read the same on both screens, and
    // this is the counter that does, because BOTH peers cross-check the same anchor tick and so both
    // pass through CrossCheck before either decides who survives.
    uint32_t DesyncsSeen() const { return DesyncsSeen_; }
    bool Stalled() const { return TheSim.Tick < WallTicks; }  // behind wallclock = waiting on peer

    // #135/#139: match-start ready gate. The match clock does NOT start until BOTH teams have
    // placed their mining camp — the placement of camp #1 IS each peer's "ready" (the seed/ready
    // handshake re-skinned). Pre-match, Tick() holds (the clock never accumulates); the local
    // camp is exchanged over MsgInput and both camps are applied as tick 0's input on both peers,
    // so play begins from tick 0 with both camps already in the identical sim state. The view
    // reads this for the pre-match camera / "waiting for opponent" state.
    bool MatchStarted() const { return MatchStarted_; }

    // ---- #163: link diagnostics. A lost frame and a half-open link both used to be INVISIBLE ----
    // How many produced peer frames went missing this match, detected by the per-frame sequence byte
    // (see ProduceAndSend). Non-zero means the transport dropped input while the link looked healthy —
    // the #163 shape, and the leading candidate for #159's unexplained divergence. Counted once per
    // gap rather than once per later frame, so it answers "how many did we lose".
    int InputGaps() const { return InputGaps_; }
    // The exec tick of the most recent missing frame. This is the field the log could not produce:
    // frames were logged as `recv msg type=N size=M` with nothing tying one to a tick, so locating a
    // lost input needed two flight recordings and a diff.
    uint32_t LastInputGapTick() const { return LastGapTick_; }
    // Pre-match, WE are ready and the peer is not, for longer than PreMatchStallWarnNs. Derived
    // against MatchStarted_ rather than cleared by whoever starts the match, so it cannot outlive its
    // fault by a path that forgot to reset it — and a latch that stays lit after the fault is one
    // people learn to ignore (the mistake #112's build-mismatch flag made).
    bool PreMatchStalled() const { return PreMatchStalled_ && !MatchStarted_; }
    // #149: which match this is, 0-based, bumped by each post-match restart. The mains latch their
    // "already tallied this match" flags per index — the only way to score exactly once per match
    // now that one Lp lives across many of them.
    uint32_t MatchIndex() const { return MatchIndex_; }
    // The local camp the player has placed but that isn't in the sim yet (pre-match, waiting for
    // the opponent to ready). Lets the view show your camp the instant you drop it. HasLocalCamp
    // stays true after the match starts, so gate the preview on !MatchStarted().
    bool HasLocalCamp() const { return LocalReady_; }
    const InputEvent& LocalCamp() const { return LocalCamp_; }

    // Flight recording (opt-in, off by default so it costs nothing): capture the
    // executed (mask0, mask1) per tick so a fresh Sim can replay the whole match to a
    // hash-identical state — the replay law (design §1), and the post-mortem dump on a
    // desync. Both peers execute the SAME stream, so either peer's recording replays both.
    void SetRecording(bool On) { Recording = On; }
    uint64_t Seed() const { return TheSim.Seed; }

#if LUR_INTERNAL
    // ---- Per-tick sink: what puts a LINKED match on disk (#159) ----
    // Called immediately after each tick's COMBINED batch is applied, with the tick it was applied
    // ON and the resulting state hash. A main hands this to a MatchRecorder; the netcode itself
    // never touches the filesystem, exactly as the solo path keeps that policy in the main.
    //
    // Why a sink instead of letting the caller walk RecordedEvents(): the index-to-tick mapping is
    // an assumption, and a resync re-bases the timeline underneath it. The sink carries the tick
    // from the sim that just stepped it, so it cannot drift — and it delivers the hash at the same
    // moment, which is the whole point of recording two peers. Identical event streams with
    // diverging hashes is nondeterminism; differing streams is a lost or duplicated frame. Those
    // are different bugs and the pair of files tells them apart at a glance.
    using TickSink = void (*)(void* Ctx, uint32_t Tick, const InputEvent* Batch, int Count,
                              uint64_t StateHash);
    // Survives a match restart (like Send/Ctx): it is app wiring, not per-match state.
    void SetTickSink(TickSink S, void* SinkCtx) { Sink_ = S; SinkCtx_ = SinkCtx; }

    // #180: fired the instant a match becomes live — from inside TryStartMatch, after the merged CVar
    // set is in place and tick 0's camps are seeded, and BEFORE any tick has executed. A main opens
    // its recording here.
    //
    // Why this is an EVENT and not something a main polls. The mains used to watch MatchStarted()
    // from their own loop, and that is a race: TryStartMatch runs whenever the gate closes, which
    // includes while DELIVERING the peer's camp — so a peer could start and execute tick 0 before its
    // loop next looked. Tick 0 is the one tick guaranteed to carry input (it is where BOTH camps are
    // applied), so the loser wrote a file with an empty tick 0 and `--recdiff` then reported
    // "EVENTS differ at tick 0 ... look at the transport" for a match whose transport was fine.
    // Observed on hardware 2026-08-01: the Galaxy armed 77 ms AFTER its match started and lost tick 0
    // while the iPhone armed 3 s before and kept it, so a 23-minute desync-free run diffed as a
    // transport fault — a false lead pointing at the subsystem #163 had just made everyone suspicious
    // of. Arming at Lp.Init instead is NOT the fix: the header would then snapshot this peer's own
    // pre-merge CVar set (the reason the arm was moved to match-start in the first place).
    using MatchStartSink = void (*)(void* Ctx);
    // Survives a match restart, like the tick sink: it is app wiring, not per-match state. Fires once
    // per match — TryStartMatch returns early once MatchStarted_ is set — including on the #149
    // post-match restart, so each match gets its own file.
    void SetMatchStartSink(MatchStartSink S, void* SinkCtx) {
        StartSink_ = S;
        StartSinkCtx_ = SinkCtx;
    }
#endif  // LUR_INTERNAL — the recorder it feeds is dev tooling, so the seam goes with it
    // #137: the executed COMBINED per-tick event batch (team0's events then team1's) — one
    // stream now (StepEvents takes one batch), replacing the two mask vectors. Replay feeds
    // each tick's batch back through StepEvents on a fresh Sim to a hash-identical state.
    const std::vector<std::vector<InputEvent>>& RecordedEvents() const { return RecEvents; }

private:
    void SendInputFrame(uint32_t Tick, const std::vector<InputEvent>& Batch);  // #163-stamped wire send
    // Rollback execution (Docs/Journal/2026-08-03), replacing the old ceiling-gated Execute():
    //   * Speculate advances the sim toward the wall tick, using real peer input where known and the
    //     "peer idle" prediction beyond it, snapshotting each tick and capped at the rollback horizon.
    //   * StepTickRange runs [TheSim.Tick, Target) — at most MaxTicks — snapshotting before each tick.
    //   * RollbackTo restores the snapshot at Tick and re-speculates to the head (a delivered frame
    //     contradicted the prediction there).
    //   * AdvanceConfirmed processes each newly-CONFIRMED tick exactly once — flight recorder, tick
    //     sink, and the 10-tick anchor — hashing the confirmed state held in the snapshot ring, never a
    //     speculative state a rollback could still change (which would false-desync the peer).
    //   * ConfirmedFrontier = the count of ticks both peers have really produced input for
    //     (= ConfirmedTick + 1) = min(local produced, peer received). The resync/history paths use it
    //     so they never hand over speculative ticks.
    void Speculate();
    void StepTickRange(uint32_t Target, uint32_t MaxTicks);
    void StepOneTick(uint32_t T);  // combine + apply tick T's batch (shared by speculate/rollback/replay)
    void RollbackTo(uint32_t Tick);
    void AdvanceConfirmed();
    uint32_t ConfirmedFrontier() const {
        const uint32_t L = static_cast<uint32_t>(LocalEvents.size());
        const uint32_t P = static_cast<uint32_t>(PeerEvents.size());
        return L < P ? L : P;
    }
    void PreMatchTick(uint64_t ElapsedNs);  // #139: hold the clock, exchange the start camp, start on both-ready
    void TryStartMatch();   // #139: both camps in -> seed tick 0 with them + begin the clock
    // #149: everything Init does to build a match, WITHOUT the peer identity (MyTeam/Send/Ctx) or
    // the merged cvar set — so the post-match restart reuses one code path with Init and cannot
    // forget a field. Init calls it too; only Init resets MatchIndex_.
    void BeginMatch(uint64_t Seed);
    void SendLocalCamp();   // #149: our camp on MsgCamp (first send + re-sends)
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
    void ApplyActiveCvars();  // rebuild the pre-tick-0 sim from the merged set (or move Cv mid-match)
    CvSnapshot MergedCvs() const;  // compile-time defaults overlaid with ActiveCvars
    // Set once this peer has merged ANYTHING (seeded or received). From then on the merged set —
    // not the local globals — is what a fresh sim must be built from, so #147 can't come back via
    // a path that calls Sim::Init directly.
    bool HaveMergedCvs_ = false;

    // #169: how many MsgCvarSync datagrams this peer has RECEIVED. Not a statistic — zero at match
    // start is the signature of the bug that made every match on the last-launched phone desync at
    // the first anchor (the incumbent never re-offered its set to a rejoiner). Deliberately never
    // reset: it answers "has this peer ever heard the other one's tunables", which spans matches.
    int CvarSyncsSeen_ = 0;

    bool BuildMismatch_ = false;  // peer reported a different LUR_BUILD_FP at connect
    // #166: the last fingerprint the peer ACTUALLY sent ("" = none heard yet). Kept as evidence
    // so Init can re-derive the verdict instead of discarding a fingerprint that arrived early.
    std::string PeerFingerprint_;
    bool        BadBuildLogged_ = false;  // refuse once per Init, not once per TryStartMatch

    std::mutex               CvQueueMutex_;  // UI thread -> sim thread edit inbox
    std::vector<PendingCvar> CvQueue_;
    void DrainCvarQueue();  // sim thread: apply queued UI edits via SetGameplayCvar
#endif
    // #147: the ONE way this class (re)creates its Sim — from the merged cvar set once any sync has
    // happened, from the globals before that. Match start, the pre-tick-0 sync apply, and the resync
    // rebuild all route through it; calling Sim::Init directly is what let the peers diverge.
    void ResetSim(uint64_t Seed);
    void EmitAnchor(uint32_t Tick, uint32_t Hash);  // emit ONE anchor for a confirmed tick+hash
    void CrossCheck(uint32_t Tick);
    void RebuildFromHistory(uint32_t Frontier);  // Incoming[0/1] -> fresh sim + timeline at Frontier
    void ReseedFrom(uint32_t Frontier);          // truncate to Frontier + a fresh Delay pre-seed

    // #161 recovery. Two entry points, one mechanism:
    //   * BeginRecovery — the anchor cross-check found two different states. Both peers reach this on
    //     the same anchor, so the GUID tie-break (MyTeam) decides who hands over and who rebuilds
    //     with nothing negotiated.
    //   * RequestRecovery — WE know we are the incomplete one (#163's sequence gap named a frame that
    //     never arrived). The peer has seen nothing wrong, so it must be asked; and because the gap is
    //     caught before the hole's tick executes, this path repairs the timeline BEFORE any divergence
    //     exists. No bad state, no anchor alarm, nothing lost.
    void BeginRecovery(const char* Why);
    void RequestRecovery(uint32_t MissingTick);
    void OfferHistoryToLoser();   // hand over our timeline and re-base to the same frontier
    void FinishRecovery();        // converged: resume play
    // Budget or patience spent. This USED to end the match as a draw; it now starts a fresh
    // recovery round after a backoff instead, and never touches TheSim.Result. See the ruling note
    // on RecoveryRounds_.
    void FailRecovery(const char* Why);
    bool IsRecoverySurvivor() const { return MyTeam == 0; }  // lower device GUID keeps its timeline

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
    uint32_t WallTicks = 0;           // ticks PRODUCED so far (may briefly lead the wall clock — see below)
    // Send-on-tap: ticks the WALL CLOCK has actually elapsed. WallTicks normally equals this, but the
    // wire-only send-early SENDS a frame ahead of it (see NextSendTick_/SentBatches_) but never
    // produces/executes ahead — WallTicks only ever catches up TO WallClockTicks_, never leads it.
    uint32_t WallClockTicks_ = 0;
    // Wire-only send-early. A pending input's frame is SENT immediately (up to SendLeadTicks ahead of
    // the wall clock) so the peer gets it sooner, but the batch is parked here — sent, not yet locally
    // produced/executed — and only moved into LocalEvents (and simulated) when its wall tick arrives, so
    // the local head stays on the 10 Hz grid (no render hitch). NextSendTick_ is the next tick number to
    // send; SentBatches_ holds the batches for ticks [WallTicks, NextSendTick_) awaiting local production.
    uint32_t NextSendTick_ = 0;
    std::deque<std::vector<InputEvent>> SentBatches_;

    std::unordered_map<uint32_t, uint32_t> MyHash, PeerHash;  // exec tick -> truncated StateHash
    bool Desync = false;
    uint32_t DesyncsSeen_ = 0;   // #204: anchor mismatches detected THIS match; see DesyncsSeen()

    // #163 diagnostics. The sequence is the LOW BYTE of the produced exec tick, not a separate
    // counter: one byte per frame (10 B/s at the tick rate) catches every gap of 1..255 frames, which
    // at 10 Hz is any outage up to 25 s — far longer than the link timeout that would fire first. A
    // full varint tick would cost 2-3 bytes per frame for a distinction the transport can't produce.
    int      InputGaps_ = 0;
    uint32_t LastGapTick_ = 0;
    bool     PreMatchStalled_ = false;
    uint64_t PreMatchWaitNs_ = 0;   // time spent ready-but-unpaired (only accrues while LocalReady_)
    uint32_t PeerTickNext_ = 0;     // exec tick of the next produced frame the peer owes us
#if LUR_AGENT
    int AgentDropTx_ = 0;           // produced frames still to be swallowed (agent fault injection)
#endif

    // #161 recovery state. Separate from Awaiting (the reconnect exchange) because the two differ in
    // what they mean and in how they end: a reconnect resumes on OUR state when it times out, while a
    // recovery that times out must NOT — our state is the one known to be suspect.
    bool     Recovering_ = false;
    bool     RecoveryAdopting_ = false;  // we are the one rebuilding (waiting for the survivor's history)
    int      RecoveryAttempts_ = 0;
    int      GapRecoveries_ = 0;     // #167: lost-frame repairs, bounded by MaxGapRecoveries
    int      DuplicateFrames_ = 0;   // #163: frames arriving BEHIND the expected sequence
    uint64_t RecoveryNs_ = 0;
    uint64_t RecoveryCarryNs_ = 0;  // wall time held during a repair, given back so no tick is lost
    // A DRAW IS NOT AN ACCEPTABLE OUTCOME OF RPS (owner ruling, 2026-08-16). A match must always be
    // able to recover and continue until one team wins, so exhausting the budget can no longer end
    // it. These two carry the retry instead: rounds is how many times the budget has been spent (it
    // only escalates the log), and the timer spaces the retries so a pair that cannot converge waits
    // quietly rather than thrashing — the failure shape #210 documents.
    int      RecoveryRounds_ = 0;
    uint64_t RecoveryRetryNs_ = 0;   // >0 = counting down to the next forced recovery round

    // #162: how long we have been unable to advance while live (not resyncing, not recovering). Reset
    // by any executed tick, so it measures a CONTINUOUS starvation rather than accumulated slowness.
    uint64_t StallNs_ = 0;
    uint32_t LastExecTick_ = 0;

    // Rollback (Docs/Journal/2026-08-03). The snapshot ring restored from on a misprediction, the
    // count of rollbacks + ticks re-simulated (diagnostics), and the highest CONFIRMED tick anchored
    // so far (anchors emit only for confirmed state — never a speculative hash that a rollback could
    // invalidate). Sized RollbackHorizon + 2 so the confirmed frontier's snapshot survives alongside a
    // full horizon of speculative snapshots to restore from.
    SnapshotRing SnapRing_{RollbackHorizon + 2};
    int      Rollbacks_ = 0;
    uint32_t ResimTicks_ = 0;
    // A tick can be executed several times (speculatively, then re-simulated on a correction), but it
    // is RECORDED, SUNK, and ANCHORED exactly once — when it becomes confirmed. LastConfirmed_ tracks
    // how far that confirmed-side processing has run (re-based by a resync). Kept close behind the
    // confirmed frontier by calling AdvanceConfirmed promptly, so its ring lookups stay in-window.
    uint32_t LastConfirmed_ = 0;

    bool Recording = false;
    std::vector<std::vector<InputEvent>> RecEvents;  // executed combined batch per tick (while Recording)
#if LUR_INTERNAL
    TickSink Sink_ = nullptr;                        // #159: per-tick recording sink (a main's recorder)
    void*    SinkCtx_ = nullptr;
    MatchStartSink StartSink_ = nullptr;             // #180: match-became-live edge (opens the file)
    // #208: has Init() run for a real match? A resync can reach this object BEFORE the main has
    // called Init (measured on the pair: the rebuild ran 245 ms before "lockstep started"), and a
    // match-live edge announced from a still-default peer makes the main write a recording header
    // off an empty sim — `seed 0 / human 0`, which recdiff refuses outright.
    bool Inited_ = false;
    void*          StartSinkCtx_ = nullptr;
#endif

    bool Awaiting = false;                            // in a resync exchange: don't produce/execute yet
    uint64_t AwaitingNs = 0;                          // #148: how long we've been holding (stall bound)
    int  ReoffersLeft = 0;                            // #148: re-sends of our history left this round
    std::vector<std::vector<InputEvent>> IncomingHistory;  // reassembled peer combined-batch history
    void SendResyncOffer();                           // #148: our history + frontier marker

    // #135/#139 match-start ready gate. Pre-match the clock holds; each peer's first miner-camp
    // placement is its "ready", exchanged over MsgInput. Both camps become tick 0's input on both
    // peers (LocalEvents[0]/PeerEvents[0]) so play begins from tick 0 with both camps in.
    bool MatchStarted_ = false;
    bool LocalReady_ = false;      // our camp placed (and captured as the tick-0 local input)
    bool PeerReady_ = false;       // peer's camp received
    bool LocalCampSent_ = false;   // we've sent our camp to the peer at least once
    uint64_t CampResendNs_ = 0;    // #149 time since that send (pre-match re-send period)
    uint64_t PostMatchNs_ = 0;     // #149 wall time held on the win/lose screen
    uint32_t MatchIndex_ = 0;      // #149 0-based match counter (restart bumps it)
    InputEvent LocalCamp_{};
    InputEvent PeerCamp_{};

    SendFn Send = nullptr;
    void* Ctx = nullptr;
};

}  // namespace Rps
