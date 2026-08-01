#include "Rps/LockstepPeer.h"

#include <cstring>

#include "Lur/Core/Assert.h"
#include "Lur/Core/BuildFingerprint.h"
#include "Lur/Core/Log.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Rps/EventCodec.h"

namespace Rps {

// Resync chunk tags: 0 = the combined history stream; 0xFF = the frontier marker; 0xFE = #161's
// "I have lost input, send me your history" request. Declared up here because the recovery paths
// below both build and consume them.
namespace {
constexpr uint8_t ResyncTagMarker  = 0xFF;
constexpr uint8_t ResyncTagRequest = 0xFE;
}  // namespace

// #147: the ONE way this class (re)creates its Sim. Every fresh-sim path — match start, the
// pre-tick-0 cvar sync, and the resync rebuild — must derive the Init-dependent hashed state
// (frontier high-water, opening gold, home-base Y) from the MERGED cvar set. Latching this peer's
// LOCAL globals instead is exactly how two phones ended up with different initial state and
// desynced, and it was wrong in three separate places; routing them all through here means a
// future fresh-sim path cannot reintroduce it. Before any sync has happened there is no merged
// set yet, so the globals ARE the answer (and in Shipping they're the only thing there is).
void LockstepPeer::ResetSim(uint64_t Seed) {
#if LUR_INTERNAL
    if (HaveMergedCvs_) { TheSim.InitWithCvs(Seed, MergedCvs()); return; }
#endif
    TheSim.Init(Seed);
}

#if LUR_INTERNAL
// The merged override set as a full Cv. Baseline = the COMPILE-TIME defaults, never LatchCvs():
// the merged set is expressed relative to the defaults ("absent" and "wall-clock tie" both mean
// default), so overlaying it onto locally-overridden globals silently keeps this peer's own value
// for every id the merge didn't carry.
CvSnapshot LockstepPeer::MergedCvs() const {
    CvSnapshot Merged = DefaultCvs();
    for (const auto& [Id, V] : ActiveCvars) ApplyCvOverride(Merged, Id, V.Raw);
    return Merged;
}
#endif

// #149: build a match. Everything Init does EXCEPT the peer identity (MyTeam/Send/Ctx) and the
// merged cvar set — those survive a restart by construction, since they aren't touched here. Init
// and the post-match restart share this so a new field can't be reset in one path and forgotten in
// the other. Note ResetSim, never Sim::Init: a fresh sim must come from the MERGED cvars (#147).
void LockstepPeer::BeginMatch(uint64_t Seed) {
    ResetSim(Seed);
    Delay = InputDelayTicks;
    LocalEvents.assign(Delay, {});  // ticks 0..Delay-1 are empty by convention on BOTH peers
    PeerEvents.assign(Delay, {});
    WallTicks = 0;
    { std::lock_guard<std::mutex> Lock(EventQueueMutex_); PendingLocalEvents.clear(); }
    Desync = false;
    MyHash.clear();
    PeerHash.clear();
    RecEvents.clear();
    Awaiting = false;
    IncomingHistory.clear();
    MatchStarted_ = false;   // #139: hold the clock until both camps are placed
    LocalReady_ = false;
    PeerReady_ = false;
    LocalCampSent_ = false;
    CampResendNs_ = 0;
    PostMatchNs_ = 0;
    // #163: per-MATCH diagnostics. Reset here (not in Init) so each match's gap count answers "did
    // THIS match lose input", which is the question a desync investigation asks; a session-lifetime
    // total would blur a clean match after a bad one.
    InputGaps_ = 0;
    LastGapTick_ = 0;
    PreMatchStalled_ = false;
    PreMatchWaitNs_ = 0;
    PeerTickNext_ = Delay;  // both peers pre-seed ticks 0..Delay-1, so the first produced frame is Delay
    LocalCamp_ = InputEvent{};
    PeerCamp_ = InputEvent{};
    AwaitingNs = 0;
    ReoffersLeft = 0;   // no resync round is in flight in a fresh match (BeginResync arms it)
    // #161: the recovery budget is PER MATCH. A match that needed two repairs should not start the
    // next one already one attempt from a draw — and a fresh match cannot be mid-recovery.
    Recovering_ = false;
    RecoveryAdopting_ = false;
    RecoveryAttempts_ = 0;
    RecoveryNs_ = 0;
    RecoveryCarryNs_ = 0;
    StallNs_ = 0;       // #162: a fresh match is not starved
    LastExecTick_ = 0;
#if LUR_INTERNAL
    // #147: ResetSim above already honours a merged set that arrived BEFORE this Init — on iOS that
    // is the normal order, not a race: one renderFrame pumps the session inbox (delivering the
    // peer's MsgCvarSync) and only afterwards reaches the "session ready -> Lp.Init" branch.
    // Per-tick stamps are dropped: they were computed as ExecTick+N on the OLD timeline and mean
    // nothing against a fresh match's tick numbering.
    PendingCvars.clear();
#endif
}

void LockstepPeer::Init(uint64_t Seed, uint8_t InMyTeam, SendFn InSend, void* InCtx) {
    MyTeam = InMyTeam & 1u;
    Send = InSend;
    Ctx = InCtx;
    MatchIndex_ = 0;      // a fresh session; the restart path is the only other bump
#if LUR_INTERNAL
    // Clear the build-fingerprint verdict HERE and nowhere else, because Init is exactly where the
    // mains re-exchange fingerprints (SendFingerprint sits next to their Lp.Init call). Pairing the
    // two means the flag always describes the CURRENT peer's build.
    //
    // It used to persist for the life of the object, and that outlived the condition: after the peer
    // was reinstalled from a matching commit, the phone that had not itself restarted still reported
    // badbuild=1 against a build that no longer existed (seen 2026-07-30, one peer reading 1 and the
    // other 0 for the same pair). A diagnostic that stays lit after the fault is one people learn to
    // ignore. NOT cleared in BeginMatch: a post-match restart does not re-exchange, so clearing
    // there would blank a REAL mismatch after the first match and never set it again.
    BuildMismatch_ = false;
#endif
    BeginMatch(Seed);
}

void LockstepPeer::QueueLocalEvent(InputEvent E) {
    E.Team = MyTeam;  // authoritative — the UI can only ever act for its own team
    std::lock_guard<std::mutex> Lock(EventQueueMutex_);
    PendingLocalEvents.push_back(E);
}

void LockstepPeer::ProduceAndSend(const std::vector<InputEvent>& Batch) {
    // #163: stamp the frame with the LOW BYTE of the exec tick it is for, BEFORE the batch. The
    // receiver knows the index it expects (PeerEvents.size()), so a lost frame is named on arrival of
    // the next one instead of surfacing minutes later as an unexplained divergence — on hardware an
    // input executed at tick 4528 was simply absent from the other peer's stream, with no transport
    // complaint, and locating it needed two flight recordings and a diff.
    //
    // Deliberately NOT inside EncodeEventBatch: that encoder is also the resync-history and
    // flight-recorder format (one codec, three uses), and a sequence number is meaningful only on the
    // live wire. Putting it here keeps the other two formats untouched.
    const uint32_t ForTick = static_cast<uint32_t>(LocalEvents.size());
    LocalEvents.push_back(Batch);  // lands at exec tick Delay + WallTicks
    Lur::Serialization::BitWriter W;
    W.WriteBits(ForTick & 0xFFu, 8);
    EncodeEventBatch(W, Batch.data(), static_cast<int>(Batch.size()));  // one framed batch per tick
    const std::vector<uint8_t>& B = W.Finish();
#if LUR_AGENT
    // Assistant-only fault injection: swallow this frame. The local timeline is UNTOUCHED (the batch is
    // already in LocalEvents above), so this reproduces exactly the #163 shape — the sender's state is
    // correct and complete, the receiver is missing one produced tick, and no transport error is
    // raised anywhere. Placed after the encode so the frame is fully built and only the handoff is
    // skipped; a drop that also skipped the encode would not exercise the same code.
    if (AgentDropTx_ > 0) {
        --AgentDropTx_;
        Lur::Log::Error("RPS/agent: DROPPING produced frame for tick %u (%d more to drop) — simulating "
                        "the #163 half-open link", ForTick, AgentDropTx_);
        return;
    }
#endif
    if (Send) Send(Ctx, MsgInput, B.data(), B.size());
}

void LockstepPeer::Tick(uint64_t ElapsedNs) {
    // A DECIDED MATCH OUTRANKS EVERY OTHER HOLD, and it is FIRST for that reason (#161). This block
    // used to sit below the resync/recovery early-returns, and that ordering reproduced the very freeze
    // this issue exists to remove: a peer that drew and then entered a recovery (a stale input gap is
    // enough) returned early from every subsequent Tick, so the post-match hold — the only thing that
    // reaches BeginMatch and clears the latches — never ran again. Once the result is in there is
    // nothing left to repair, so no in-flight exchange may outlive it.
    //
    // #149: hold the win/lose screen, then begin a FRESH match that waits for both camps again. The two
    // peers detect the result on the same TICK but time this hold on their own clocks, so they rebuild a
    // few ms apart. That is safe — and ONLY because the new match holds until both camps are in (#139).
    // Do not try to make the restart bit-synchronous. Seed+1 keeps the peers agreeing (both hold the
    // same seed) while giving each match its own variety.
    if (MatchStarted_ && TheSim.Result != ResultOngoing) {
        PostMatchNs_ += ElapsedNs;
        if (PostMatchNs_ >= PostMatchHoldNs) {
            const uint64_t NextSeed = TheSim.Seed + 1;
            BeginMatch(NextSeed);
            ++MatchIndex_;
            Lur::Log::Info("RPS: match %u begins (seed 0x%llx) — awaiting both camps", MatchIndex_,
                           static_cast<unsigned long long>(NextSeed));
        }
        return;
    }
    // #148: a resync exchange holds production/execution — but it must NEVER hold forever. The
    // survivor of an app restart waited on a marker the newcomer could not send (Session fires its
    // resync handler only on a reconnect EDGE, which a freshly launched app never takes), and with
    // no timeout here that phone was wedged permanently: the reported "still paused on the phone
    // that kept running". Consistency over completeness — on giving up we keep OUR state, and the
    // peer rebuilds from it when it next offers/asks.
    // #161: a recovery in flight is bounded independently of the reconnect exchange, and it must NOT
    // end the way that one does. A stalled reconnect resumes on our own state, which is right when our
    // state is merely stale; here our state is the one known to be suspect, so resuming on it would
    // re-establish the divergence and the anchor would trip again immediately. Out of patience means
    // out of options: take the draw (the last resort), which at least resolves identically on both
    // screens and lets the session restart.
    if (Recovering_) {
        RecoveryNs_ += ElapsedNs;
        if (RecoveryNs_ >= DesyncRecoveryTimeoutNs) {
            FailRecovery("the survivor's history never arrived");
            return;
        }
        // CARRY the held wall time rather than dropping it. The survivor keeps ticking through the
        // exchange while we are frozen, so time discarded here is time we can never make up: WallTicks
        // is the execution target and the ceiling is min(WallTicks, ...), so the adopter would resume
        // permanently one tick behind — and a second recovery would put it two behind, compounding.
        // Giving the time back produces a short catch-up burst instead, which the design already
        // guarantees is result-neutral (scheduling never changes results, §3's sprint law) and which
        // Execute's per-call cap keeps from starving input.
        RecoveryCarryNs_ += ElapsedNs;
        return;   // hold production and execution until the repair lands
    }
    if (RecoveryCarryNs_ != 0) {
        ElapsedNs += RecoveryCarryNs_;
        RecoveryCarryNs_ = 0;
    }
    if (Awaiting) {
        AwaitingNs += ElapsedNs;
        if (AwaitingNs < ResyncStallTimeoutNs) return;
        Lur::Log::Info("RPS: resync stalled %llums with no peer marker — resuming on our own state "
                       "(tick %u)", static_cast<unsigned long long>(AwaitingNs / 1'000'000ull),
                       TheSim.Tick);
        Awaiting = false;
        AwaitingNs = 0;
    }
#if LUR_INTERNAL
    DrainCvarQueue();  // apply any UI-thread gameplay-CVar edits (stamps + sends MsgCvar)
#endif
    // #139: pre-match — hold the clock (ElapsedNs is dropped, so the match times from start, not
    // from the menu) while the two camps are exchanged. ALWAYS return this call: whether the
    // match is still pending OR just started here, no wall tick is produced yet, so both peers
    // begin advancing from WallTicks==0 on their NEXT Tick — symmetric, no start-skew (one peer
    // readies during its own Tick, the other during a delivered message).
    if (!MatchStarted_) { PreMatchTick(ElapsedNs); return; }
    const uint32_t N = Clock.AdvancePreserving(ElapsedNs, 64);
    for (uint32_t I = 0; I < N; ++I) {
        // All events queued since the last produced tick fold into the FIRST new tick's batch
        // (mirrors the old mask's accumulate-then-consume); later ticks in this burst are empty.
        // If N==0 the pending events persist for the next Tick (never dropped).
        std::vector<InputEvent> Batch;
        if (I == 0) {
            std::lock_guard<std::mutex> Lock(EventQueueMutex_);
            Batch.swap(PendingLocalEvents);
        }
        ProduceAndSend(Batch);
        ++WallTicks;
    }
    Execute();

    // #162: bound the ceiling stall — the one hold here that had no timeout, which is how a load
    // collapse became terminal (both phones dead, in different matches, nothing reconciling them).
    // Measured as CONTINUOUS starvation: any executed tick re-arms it, so ordinary lockstep waiting
    // and a phone that briefly falls behind cost nothing. Only counted while we are live; the resync
    // and recovery holds have their own, much tighter, bounds.
    if (TheSim.Tick != LastExecTick_) {
        LastExecTick_ = TheSim.Tick;
        StallNs_ = 0;
        return;
    }
    if (TheSim.Tick >= WallTicks) return;   // nothing to wait FOR — we are simply up to date
    StallNs_ += ElapsedNs;
    if (StallNs_ < CeilingStallTimeoutNs) return;
    Lur::Log::Error("RPS: starved at the ceiling for %llums at tick %u — the peer's input is not coming "
                    "(it collapsed under load or restarted into another match, #162). Ending the match "
                    "so both sides return to the camp handshake, which always re-converges.",
                    static_cast<unsigned long long>(StallNs_ / 1'000'000ull), TheSim.Tick);
    StallNs_ = 0;
    TheSim.Result = ResultDraw;   // #149's post-match hold + restart takes it from here
}

// #139 match-start: pre-match, the clock is held. Capture the local camp (the first miner-place
// the UI queued) as tick 0's local input, send it once so the peer can mirror it, and start the
// match the moment both camps are in. No wall ticks are produced/executed until then.
void LockstepPeer::PreMatchTick(uint64_t ElapsedNs) {
    if (!LocalReady_) {
        std::lock_guard<std::mutex> Lock(EventQueueMutex_);
        for (const InputEvent& E : PendingLocalEvents) {
            if (E.Kind != EventPlaceBuilding || E.Type != UnitMiner) continue;
            // #167: "ready" must mean "ready with a camp that will EXIST". This used to take the camp
            // on faith, so an unplaceable one still set LocalReady_, the match started, and tick 0's
            // ApplyPlace then discarded it — leaving a peer in a live match with NO camp and its full
            // opening gold. That state is reachable by no rule of the game, and it reads downstream as
            // an economy desync rather than as a bad input.
            //
            // CanPlaceBuilding, NOT WouldAcceptPlace, and the difference is deliberate: the latter
            // also requires the camp to be AFFORDABLE, and gold is the one input that legitimately
            // changes between here and tick 0 — MsgCvarSync converges the tunables pre-tick-0 (#147),
            // and the two phones really do arrive holding different starting_gold. Refusing on gold
            // observed before the merge would leave a peer silently never-ready on a camp tick 0 would
            // have accepted, which presents as #163's "pre-match stalled / half-open link" and sends
            // the reader hunting the transport. Spatial validity has no such timing: it is a pure
            // function of the coordinate and the map. An unaffordable camp is still discarded at tick
            // 0 as before — that case is a tunables choice, not a malformed input.
            //
            // A human cannot trip this (drag-to-place only emits once ResolvePlacement succeeded);
            // the agent harness can and did, because injecting exact coordinates is its whole purpose.
            if (!TheSim.CanPlaceBuilding(E.Team, E.Type, Fixed{E.X}, Fixed{E.Y})) {
                Lur::Log::Error("RPS: pre-match camp at (%d,%d) is NOT placeable — ignoring it and "
                                "staying unready. Place again on valid ground.",
                                Fixed{E.X}.ToInt(), Fixed{E.Y}.ToInt());
                continue;  // a later candidate in the same batch may still be good
            }
            LocalCamp_ = E; LocalReady_ = true;
            break;
        }
        PendingLocalEvents.clear();  // pre-match: only the mining camp is accepted; drop the rest
    }
    // #149: send our camp, then KEEP re-sending it on a period until the peer's arrives. One send
    // was safe only while both peers entered a match together; across a post-match restart the
    // earlier riser's camp can land while the other is still on its win screen, where it is
    // buffered as the old match's input and then wiped by that peer's own restart. Re-sending
    // costs one tiny frame every 500ms and makes the exchange self-healing whoever restarts first.
    // Gated on LocalReady_ only — NOT on "the peer hasn't readied". Holding our camp back once
    // theirs is in hand deadlocks the handshake: whoever readies SECOND already knows the other's
    // camp, so it would never send its own and the first peer would wait forever.
    if (LocalReady_) {
        CampResendNs_ += ElapsedNs;
        if (!LocalCampSent_ || CampResendNs_ >= PreMatchCampResendNs) SendLocalCamp();
        // #163: we have readied and kept re-sending, and the peer has not readied back. Past
        // PreMatchStallWarnNs, silence is the likelier explanation than the other player still
        // deciding where to build — and to the player this state looks exactly like a frozen app
        // ("placed a camp on both phones and nothing happened"). Say it once, with the diagnosis;
        // repeating it every tick would bury the rest of the log.
        if (!PreMatchStalled_ && !PeerReady_) {
            PreMatchWaitNs_ += ElapsedNs;
            if (PreMatchWaitNs_ >= PreMatchStallWarnNs) {
                PreMatchStalled_ = true;
                Lur::Log::Error("RPS: pre-match stalled %llums — our camp has been re-sent since then "
                                "and the peer has not readied. If the peer reports started=1 the link "
                                "is HALF-OPEN: our direction is dead while theirs works (#163).",
                                static_cast<unsigned long long>(PreMatchWaitNs_ / 1'000'000ull));
            }
        }
    }
    TryStartMatch();
}

// The local camp on its OWN message type (#160). It used to go out as a MsgInput batch — "the
// identical shape a live input tick has, so the receiver's normal decode path handles it" — and that
// shared shape was the bug: the receiver then had to decide from the bytes whether a frame was a
// produced tick or a camp re-send, and a produced tick can be byte-identical to a re-send. Same
// payload encoding, different channel, so the decision is made by the sender (which knows) instead
// of guessed by the receiver (which cannot).
void LockstepPeer::SendLocalCamp() {
    Lur::Serialization::BitWriter W;
    EncodeEventBatch(W, &LocalCamp_, 1);
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgCamp, B.data(), B.size());
    LocalCampSent_ = true;
    CampResendNs_ = 0;
}

// Both camps in hand -> make them tick 0's input on BOTH peers and start the clock. LocalEvents[0]
// = our camp, PeerEvents[0] = the peer's; Execute combines team0-first, so both peers apply the
// identical [team0 camp, team1 camp] at tick 0 and diverge from an identical state. The Delay-1
// pre-seeded empties after index 0 stay the delay buffer; real input still lands at Delay+.
void LockstepPeer::TryStartMatch() {
    if (MatchStarted_ || !LocalReady_ || !PeerReady_) return;
#if LUR_INTERNAL
    // #169 asked for the disagreement to be LOUD and BEFORE tick 0 rather than a mid-match draw, and
    // this is the whole detector — no extra wire slot needed, because after the BeginResync re-offer
    // every peer sends its set at least twice and an EMPTY set is still a message. So "I am starting
    // a match having never received one" is not a benign case: it means the peer's offers never
    // reached us, which is precisely the state that ran four consecutive matches on mismatched
    // tunables. Cheap, exact, and it fires before the first tick instead of after the match is lost.
    //
    // Deliberately not a refusal (unlike badbuild): the sets may well be identical, and killing a
    // match on a suspicion is worse than playing one that says so in the log.
    if (CvarSyncsSeen_ == 0)
        Lur::Log::Error("RPS: starting a linked match having received NO cvar sync from the peer — if "
                        "either phone has persisted tunables the two sims will differ from tick 0 "
                        "(#169). Check both peers' pre-match gold/hash agree.");
#endif
    LocalEvents[0] = {LocalCamp_};
    PeerEvents[0]  = {PeerCamp_};
    MatchStarted_ = true;
}

void LockstepPeer::Execute() {
    // Ceiling: wallclock pace, gated by BOTH input timelines (min of the three).
    auto Ceiling = [this]() -> uint32_t {
        uint32_t C = WallTicks;
        if (LocalEvents.size() < C) C = static_cast<uint32_t>(LocalEvents.size());
        if (PeerEvents.size() < C)  C = static_cast<uint32_t>(PeerEvents.size());
        return C;
    };
    // Cap ticks per call (#90): a catch-up burst drains over subsequent calls instead
    // of monopolizing this one and starving input -> ANR. Nothing is discarded — the
    // ceiling/masks persist. Scheduling never changes results (design §3 sprint law),
    // so the capped drain lands on the exact same state as the old uncapped loop.
    const uint32_t Backlog = Ceiling() > TheSim.Tick ? Ceiling() - TheSim.Tick : 0;
    const bool     Burst   = Backlog > AnchorBurstThreshold;
    uint32_t Ran = 0;
    while (!Desync && TheSim.Tick < Ceiling() && Ran < MaxExecTicksPerService) {
        const uint32_t T = TheSim.Tick;
        // Combine the tick's per-team batches in a TEAM0-FIRST order both peers agree on
        // (each event also carries its Team), so StepEvents applies the identical sequence on
        // both sides — the determinism precondition. Fixed stack scratch, no per-tick heap.
        InputEvent Combined[2 * MaxEventsPerTick];
        int NC = 0;
        const std::vector<InputEvent>& L = LocalEvents[T];
        const std::vector<InputEvent>& P = PeerEvents[T];
        const std::vector<InputEvent>& First  = MyTeam == 0 ? L : P;  // team 0's batch
        const std::vector<InputEvent>& Second = MyTeam == 0 ? P : L;  // team 1's batch
        for (const InputEvent& E : First)  if (NC < 2 * MaxEventsPerTick) Combined[NC++] = E;
        for (const InputEvent& E : Second) if (NC < 2 * MaxEventsPerTick) Combined[NC++] = E;
#if LUR_INTERNAL
        ApplyCvarsForTick(T);  // #112: land any gameplay-CVar overrides stamped for tick T
#endif
        TheSim.StepEvents(Combined, NC);
        if (Recording) RecEvents.emplace_back(Combined, Combined + NC);
#if LUR_INTERNAL
        // #159: hand the executed tick to whoever is recording it. T is the tick these events were
        // applied ON (TheSim.Tick has already advanced past it), matching MatchRecord's convention.
        // The hash is computed here rather than by the sink so the sink never has to reach back into
        // this object mid-Execute — and only when someone is actually listening, so a match with no
        // recorder attached pays a null check.
        if (Sink_ != nullptr) Sink_(SinkCtx_, T, Combined, NC, TheSim.StateHash());
#endif
        // Normal cadence: anchor every 10th tick. During a burst, suppress these and
        // emit a single anchor at the frontier below (avoids flooding the GATT queue).
        if (!Burst && TheSim.Tick % 10 == 0) EmitAnchor();
        ++Ran;
    }
    if (Burst && Ran > 0) EmitAnchor();  // one anchor at the reached frontier
}

#if LUR_INTERNAL
void LockstepPeer::StorePendingCvar(uint32_t Tick, uint8_t Id, int32_t Raw, uint64_t WallMs) {
    auto& Vec = PendingCvars[Tick];
    for (auto& P : Vec) {
        if (P.Id == Id) {  // same CVar stamped twice for one tick: last wall-clock writer wins
            if (WallMs > P.WallMs) { P.Raw = Raw; P.WallMs = WallMs; }
            return;
        }
    }
    Vec.push_back({Id, Raw, WallMs});
}

void LockstepPeer::ApplyCvarsForTick(uint32_t T) {
    const auto It = PendingCvars.find(T);
    if (It == PendingCvars.end()) return;
    for (const PendingCvar& P : It->second) ApplyCvOverride(TheSim.Cv, P.Id, P.Raw);
    PendingCvars.erase(It);
}

void LockstepPeer::SetGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t EditWallClockMs) {
    // Stamp at the same horizon as a produced input (WallTicks + Delay): a few ticks ahead,
    // so it lands before either peer simulates that tick. Store locally AND send, so both
    // peers apply the identical override at the identical exec tick.
    const uint32_t ApplyTick = WallTicks + Delay;
    StorePendingCvar(ApplyTick, GameplayId, RawValue, EditWallClockMs);
    MergeCvar(GameplayId, RawValue, EditWallClockMs);  // keep the current-override set current
    Lur::Serialization::BitWriter W;
    Lur::Serialization::WriteVarUint(W, ApplyTick);
    W.WriteBits(GameplayId, 8);
    W.WriteBits(static_cast<uint32_t>(EditWallClockMs >> 32), 32);
    W.WriteBits(static_cast<uint32_t>(EditWallClockMs & 0xFFFFFFFFu), 32);
    W.WriteBits(static_cast<uint32_t>(RawValue), 32);
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgCvar, B.data(), B.size());
}

void LockstepPeer::MergeCvar(uint8_t Id, int32_t Raw, uint64_t WallMs) {
    // Last-writer-wins by wall clock; an exact timestamp collision with a DIFFERENT value
    // reverts to the compile-time default (drop the override) — the one value both peers
    // unambiguously agree on (C.2). Commutative, so both peers reach the same merged set.
    // #147: from the first merge onward THIS peer is in the synced regime, so every later fresh sim
    // must come from the merged set rather than the globals — including when the merge resolver has
    // emptied the set (a wall-clock tie), which means "all compile-time defaults", NOT "our locals".
    HaveMergedCvs_ = true;
    const auto It = ActiveCvars.find(Id);
    if (It == ActiveCvars.end()) { ActiveCvars[Id] = {Raw, WallMs}; return; }
    if (WallMs > It->second.WallMs)                          It->second = {Raw, WallMs};
    else if (WallMs == It->second.WallMs && Raw != It->second.Raw) ActiveCvars.erase(It);
    // else: incoming is older, or identical -> keep existing.
}

void LockstepPeer::ApplyActiveCvars() {
    // #147: several HASHED initial values are DERIVED from Cv inside Sim::Init (each team's
    // frontier high-water, the opening gold, the home base's Y). Assigning Cv alone leaves those
    // at the value baked from this peer's PRE-sync Cv, so two peers whose persisted cvars differed
    // desynced at the very first anchor with no units on the field. Rebuild the sim from the merged
    // Cv instead (ResetSim), so every derived value comes from the same input on both peers.
    //
    // Pre-tick-0 is the normal case: the transport is reliable+ordered and each peer sends
    // MsgCvarSync (at Init) BEFORE its camp on MsgInput, so a peer's sync lands before PeerReady_
    // — i.e. before the match can start. Once the match IS running, only Cv can move; rebuilding
    // would wipe live state, and the anchor hash would flag any resulting divergence.
    if (!MatchStarted_ && TheSim.Tick == 0) ResetSim(TheSim.Seed);
    else                                    TheSim.Cv = MergedCvs();
}

void LockstepPeer::SeedGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t EditWallClockMs) {
    MergeCvar(GameplayId, RawValue, EditWallClockMs);
    ApplyActiveCvars();  // reflect locally now; the match-start sync re-merges across peers
}

void LockstepPeer::SendCvarSync() {
    Lur::Serialization::BitWriter W;
    Lur::Serialization::WriteVarUint(W, static_cast<uint32_t>(ActiveCvars.size()));
    for (const auto& [Id, V] : ActiveCvars) {
        W.WriteBits(Id, 8);
        W.WriteBits(static_cast<uint32_t>(V.WallMs >> 32), 32);
        W.WriteBits(static_cast<uint32_t>(V.WallMs & 0xFFFFFFFFu), 32);
        W.WriteBits(static_cast<uint32_t>(V.Raw), 32);
    }
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgCvarSync, B.data(), B.size());
}

void LockstepPeer::QueueGameplayCvar(uint8_t GameplayId, int32_t RawValue, uint64_t WallMs) {
    std::lock_guard<std::mutex> Lock(CvQueueMutex_);
    CvQueue_.push_back({GameplayId, RawValue, WallMs});
}

void LockstepPeer::DrainCvarQueue() {
    std::vector<PendingCvar> Local;
    {
        std::lock_guard<std::mutex> Lock(CvQueueMutex_);
        Local.swap(CvQueue_);
    }
    for (const PendingCvar& P : Local) SetGameplayCvar(P.Id, P.Raw, P.WallMs);
}

void LockstepPeer::SendFingerprint() {
    const char* Fp = Lur::BuildFingerprint();
    if (Send) Send(Ctx, MsgFingerprint, reinterpret_cast<const uint8_t*>(Fp), std::strlen(Fp));
}
#endif  // LUR_INTERNAL

void LockstepPeer::EmitAnchor() {
    const uint32_t T = TheSim.Tick;
    const uint32_t H = static_cast<uint32_t>(TheSim.StateHash());
    MyHash[T] = H;
    Lur::Serialization::BitWriter W;
    Lur::Serialization::WriteVarUint(W, T);
    W.WriteBits(H, 32);
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgAnchor, B.data(), B.size());
    CrossCheck(T);  // peer's anchor for T may already be in hand
}

// ---- #161: recovery ----------------------------------------------------------------------------
// A desync must RECOVER the match, not end it. e6d6abf's draw was a stopgap for a worse bug (both
// phones frozen forever) and threw away a perfectly playable match to escape it.
//
// The survivor is decided by the GUID tie-break, which MyTeam already encodes (Team = MyGuid <
// PeerGuid ? 0 : 1 on every path), so both peers reach the same verdict with nothing negotiated —
// essential, because a desync is exactly the situation where the two sides cannot agree about
// anything derived from state. The loser replays the survivor's input history through a fresh sim.
void LockstepPeer::BeginRecovery(const char* Why) {
    if (Recovering_) return;                 // one round at a time; the timeout ends it
    if (TheSim.Result != ResultOngoing) return;  // a decided match has nothing left to repair
    if (RecoveryAttempts_ >= MaxDesyncRecoveries) {
        FailRecovery("recovery budget spent");
        return;
    }
    ++RecoveryAttempts_;
    Recovering_ = true;
    RecoveryNs_ = 0;
    MyHash.clear();  // anchors from before the repair say nothing about the state after it
    PeerHash.clear();
    if (IsRecoverySurvivor()) {
        RecoveryAdopting_ = false;
        Lur::Log::Info("RPS: recovering (%s) — we hold the lower device id, so our timeline survives; "
                       "handing it to the peer (attempt %d/%d, tick %u)",
                       Why, RecoveryAttempts_, MaxDesyncRecoveries, TheSim.Tick);
        OfferHistoryToLoser();
        // Our own state stands, so stop gating execution on it. We will still sit at the ceiling until
        // the peer resumes producing, which is ordinary lockstep waiting rather than a stall.
        Desync = false;
        Recovering_ = false;
        return;
    }
    RecoveryAdopting_ = true;
    IncomingHistory.clear();
    Awaiting = true;    // hold production AND execution: our state is the one known to be suspect
    AwaitingNs = 0;
    Lur::Log::Info("RPS: recovering (%s) — peer holds the lower device id, rebuilding from its history "
                   "(attempt %d/%d, our tick %u)",
                   Why, RecoveryAttempts_, MaxDesyncRecoveries, TheSim.Tick);
}

// #163's gap detector named a frame that never arrived, so WE know we are the incomplete peer while
// the other has seen nothing wrong. It cannot deduce that, so ask. The gap is caught before the hole's
// tick executes (the ceiling is gated on PeerEvents.size(), which the missing frame never grew), so
// this repairs the timeline while both sims are still identical — the divergence never happens.
void LockstepPeer::RequestRecovery(uint32_t MissingTick) {
    if (Recovering_ || Awaiting) return;
    if (TheSim.Result != ResultOngoing) return;  // the match is over; the peer's later frames are noise
    if (RecoveryAttempts_ >= MaxDesyncRecoveries) return;  // a lost frame is not worth ending a match
    ++RecoveryAttempts_;
    Recovering_ = true;
    RecoveryAdopting_ = true;
    RecoveryNs_ = 0;
    IncomingHistory.clear();
    Awaiting = true;
    AwaitingNs = 0;
    MyHash.clear();
    PeerHash.clear();
    Lur::Log::Info("RPS: input gap at tick %u — asking the peer for its history before the hole is "
                   "executed, so nothing diverges (attempt %d/%d)",
                   MissingTick, RecoveryAttempts_, MaxDesyncRecoveries);
    Lur::Serialization::BitWriter W;
    Lur::Serialization::WriteVarUint(W, MissingTick);
    const std::vector<uint8_t>& MB = W.Finish();
    std::vector<uint8_t> Req;
    Req.reserve(MB.size() + 1);
    Req.push_back(ResyncTagRequest);
    Req.insert(Req.end(), MB.begin(), MB.end());
    if (Send) Send(Ctx, MsgResyncChunk, Req.data(), Req.size());
}

// Hand our timeline over and re-base to the frontier we just published. BOTH sides must truncate to
// the same frontier and re-add the delay slack, or the loser's next produced frame lands at an index
// we no longer expect — which is the same misalignment the recovery exists to repair.
void LockstepPeer::OfferHistoryToLoser() {
    SendResyncOffer();
    ReseedFrom(TheSim.Tick);
}

void LockstepPeer::FinishRecovery() {
    Recovering_ = false;
    RecoveryAdopting_ = false;
    RecoveryNs_ = 0;
    Awaiting = false;
    AwaitingNs = 0;
    Desync = false;   // the ONLY place besides BeginMatch that clears it: we are provably converged
    Lur::Log::Info("RPS: recovered — resuming from the peer's timeline at tick %u", TheSim.Tick);
}

// The last resort, and the only legitimate home for a draw. Reached when the attempt budget is spent
// (input replay cannot fix genuine nondeterminism — it reproduces it) or when the survivor's history
// never arrives. A draw is the one outcome that can be declared SYMMETRICALLY: awarding the win needs
// agreement about whose state was right, which is precisely what a desync destroys.
void LockstepPeer::FailRecovery(const char* Why) {
    Recovering_ = false;
    RecoveryAdopting_ = false;
    Awaiting = false;
    RecoveryNs_ = 0;
    RecoveryCarryNs_ = 0;   // the match is over; there is nothing left to catch up to
    Desync = true;
    Lur::Log::Error("RPS: recovery FAILED (%s) after %d attempt(s) — ending the match as a draw at tick "
                    "%u. Recovery converges on a lost input and cannot on nondeterminism, so this is "
                    "the signal to look for the latter (a float in sim state, a compiler difference).",
                    Why, RecoveryAttempts_, TheSim.Tick);
    TheSim.Result = ResultDraw;   // #149's post-match hold + restart then clears the latch
}

void LockstepPeer::CrossCheck(uint32_t Tick) {
    const auto Mine = MyHash.find(Tick);
    const auto Theirs = PeerHash.find(Tick);
    if (Mine == MyHash.end() || Theirs == PeerHash.end() || Mine->second == Theirs->second) return;
    if (Desync || Recovering_) return;   // already handling it — don't re-trip on every later anchor
    // A mismatch under a reliable transport + a deterministic sim is always a bug. The RESPONSE has to
    // be a playable one, and it has been wrong twice: first Desync simply gated the exec loop and
    // nothing ever cleared it, so the match STOPPED (2026-07-30, both peers pinned at tick 8180 with
    // different hashes, datagrams still flowing, no message on screen, no way out but killing the app);
    // then e6d6abf declared a draw to escape that freeze, which is survivable but throws away a
    // playable match and tells the players something untrue about it.
    //
    // #161: RECOVER. Gate execution while the repair runs — our state may be the wrong one — and let
    // the tie-break decide whose timeline survives. Both peers cross-check the SAME anchor tick and
    // both see the same mismatch, so both reach this line and both compute the same survivor, which is
    // the consistency rule (a contested outcome must resolve the same way on both screens). A draw
    // still exists, but only after recovery has failed its bounded attempts.
    Desync = true;
    Lur::Log::Error("RPS: DESYNC at tick %u — mine %08x, peer %08x. Recovering (this is still a bug: "
                    "identical builds should never diverge).",
                    Tick, Mine->second, Theirs->second);
    BeginRecovery("anchor hash mismatch");
}


void LockstepPeer::OnMessage(Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N) {
    if (Type == MsgInput) {
        // #160: EVERY MsgInput frame is a produced tick, with no exceptions — so it is always
        // buffered and the index==tick alignment of PeerEvents cannot be disturbed by anything on
        // this channel. That invariant is the fix. Previously the camp exchange shared this slot and
        // a frame was classified by its CONTENTS, so a produced batch that happened to equal the
        // peer's opening camp (re-placing where their camp already stands — a legal tap) was dropped
        // as a re-send, and every later peer batch landed one exec tick early for the rest of the
        // match. It only escaped notice because re-placing onto an occupied square is a sim no-op,
        // so the hashes stayed equal while the streams were skewed (found by diffing two peers'
        // recordings, #159/#160).
        Lur::Serialization::BitReader R(Data, N);
        const uint32_t Seq = R.ReadBits(8);  // #163: low byte of the exec tick this frame is for
        InputEvent Buf[MaxEventsPerTick];
        const int Cnt = DecodeEventBatch(R, Buf, MaxEventsPerTick);
        if (Cnt < 0) return;
        // #163: does this frame land where we expect it to? PeerTickNext_ is the exec tick of the next
        // frame the peer owes us, so a sequence ahead of its low byte proves one went missing. Report
        // and LOCATE it — the reason a gap took two recordings and a diff to find is that nothing here
        // could say "the peer's tick 4528 never arrived".
        //
        // The expectation is tracked separately from PeerEvents rather than read off its size, for two
        // reasons. It is re-based past the hole after reporting, so ONE lost frame costs one report
        // instead of one per frame for the rest of the match — otherwise "how many did we lose", the
        // number that matters, is unanswerable. And keeping it a full tick (not just the wire byte)
        // means the reported tick stays exact after an earlier gap has already shifted the buffer.
        //
        // Detection lands BEFORE the hole's tick executes — the ceiling is gated on PeerEvents.size(),
        // which the missing frame never grew — so it is early enough to recover from rather than
        // diagnose after the fact. Acting on it is #161; this commit only makes it visible.
        // A decided match is exempt: our sim has stopped while the peer may still be producing, so an
        // "expected next tick" no longer means anything and every later frame would read as a gap.
        if (!Awaiting && TheSim.Result == ResultOngoing) {
            const uint32_t Missing = (Seq - (PeerTickNext_ & 0xFFu)) & 0xFFu;
            if (Missing != 0) {
                ++InputGaps_;
                LastGapTick_ = PeerTickNext_;
                Lur::Log::Error("RPS: INPUT GAP — peer frame for tick %u never arrived (it sent seq %u, "
                                "we expected %u; %u frame(s) missing). The link reported no error, so "
                                "this is the lost-input shape of #163 and the leading candidate for "
                                "#159's divergence.",
                                PeerTickNext_, Seq, PeerTickNext_ & 0xFFu, Missing);
                PeerTickNext_ += Missing;  // re-base past the hole: count the gap once, not forever
                // #161: ACT on it. We are provably the incomplete peer and the other side has seen
                // nothing wrong, so it must be asked — and because detection beat execution to the
                // hole, this repairs the timeline while both sims are still identical. Return without
                // buffering: appending this frame at the hole's index is exactly the misalignment
                // being repaired.
                RequestRecovery(LastGapTick_);
                if (Recovering_) return;
            }
            ++PeerTickNext_;
        }
        if (!MatchStarted_) {
            // Pre-match, a produced frame can only be from a peer that has ALREADY started (it
            // restarted before us). Buffer it — it lands at PeerEvents[Delay]+ once we start — so
            // nothing is dropped across the restart skew. Its camp arrived earlier on MsgCamp; the
            // transport is reliable and ordered, so a produced tick can never overtake it.
            PeerEvents.emplace_back(Buf, Buf + Cnt);
            return;
        }
        if (!Awaiting) {
            PeerEvents.emplace_back(Buf, Buf + Cnt);  // live wire: each Input frame = next peer exec tick
            Execute();                                // peer input may unblock the ceiling
        }
    } else if (Type == MsgCamp) {
        // #139/#149/#160: the peer's opening camp, and its 500 ms re-sends. NEVER buffered as a tick
        // — it is not a produced tick — which is what makes an arbitrary number of re-sends safe.
        Lur::Serialization::BitReader R(Data, N);
        InputEvent Buf[MaxEventsPerTick];
        const int Cnt = DecodeEventBatch(R, Buf, MaxEventsPerTick);
        if (Cnt < 1) return;
        if (!PeerReady_) {
            PeerCamp_ = Buf[0];
            PeerReady_ = true;
            TryStartMatch();  // both camps in hand -> tick 0 carries them on both peers
            return;
        }
        // #149: a re-send arriving at a peer that already holds this camp. If we have STARTED, the
        // peer is still waiting on OURS — it never got it (we started on its camp; our own send
        // raced its restart and was dropped). Re-sending is the only thing that can unstrand it:
        // whoever starts first leaves PreMatchTick and would otherwise never send again.
        if (MatchStarted_) SendLocalCamp();
    } else if (Type == MsgAnchor) {
        Lur::Serialization::BitReader R(Data, N);
        const uint32_t T = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
        const uint32_t H = R.ReadBits(32);
        if (!Awaiting && R.IsOk()) {
            PeerHash[T] = H;
            CrossCheck(T);
        }
    } else if (Type == MsgResyncChunk) {
        if (N < 1) return;
        const uint8_t Tag = Data[0];
        if (Tag == 0) {  // #137: a chunk of the peer's combined event history
            uint32_t Ft = 0;
            DecodeEventResyncChunk(Data + 1, N - 1, Ft, IncomingHistory);  // reliable order -> append
        } else if (Tag == ResyncTagRequest) {
            // #161: the peer detected a gap in OUR stream and is asking for our history. It has
            // already stopped executing, so nothing is racing; hand the timeline over and re-base to
            // the same frontier so its next produced frame lands where we expect it.
            //
            // If we are ALSO recovering as the adopter (both directions lost frames, or a desync trip
            // crossed with a gap), the tie-break settles it: only the survivor answers, and the other
            // keeps waiting rather than the two swapping histories forever.
            Lur::Serialization::BitReader R(Data + 1, N - 1);
            const uint32_t MissingTick = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
            if (!R.IsOk()) return;
            if (RecoveryAdopting_ && !IsRecoverySurvivor()) return;
            Lur::Log::Info("RPS: peer lost our frame for tick %u and asked for the history — sending it "
                           "from our frontier %u", MissingTick, TheSim.Tick);
            OfferHistoryToLoser();
        } else if (Tag == ResyncTagMarker) {
            Lur::Serialization::BitReader R(Data + 1, N - 1);
            const uint32_t F = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
            // #161: while ADOPTING, take the survivor's history whatever its frontier is relative to
            // ours. The reconnect rule ("only if the peer is ahead") cannot serve here: a desync has
            // both peers at the SAME tick with different states, so `F > TheSim.Tick` is false on both
            // sides and neither would ever yield — which is why the existing machinery, though it was
            // all present, never converged anything on a hash mismatch.
            const bool Adopt = R.IsOk() && IncomingHistory.size() >= F &&
                               (RecoveryAdopting_ || F > TheSim.Tick);
            if (Adopt) {
                RebuildFromHistory(F);
                if (Recovering_) { FinishRecovery(); return; }
            } else {
                IncomingHistory.clear();  // we're ahead / short -> keep ours
                // #161: the peer published a frontier we are ALREADY standing on. Offering a history
                // means it has re-based its own timeline to that frontier, so we must re-base to the
                // same one or its next produced frame lands at an index we no longer expect — a fresh
                // misalignment created by the repair. This matters even when we saw nothing wrong
                // ourselves (a mismatch the peer detected and we did not), which is precisely the case
                // the reconnect rule never had to consider. Only when our sim is exactly AT F: with the
                // sim ahead of F, re-basing would rewind the timeline under already-executed ticks.
                if (R.IsOk() && F == TheSim.Tick) {
                    ReseedFrom(F);
                }
                // #148: the peer is BEHIND us — a restarted app rejoins at F=0. It cannot catch up
                // unless we hand it our history, and our first offer may have been thrown away by
                // its own Lp.Init (which clears IncomingHistory). So re-offer, once per round.
                else if (R.IsOk() && F < TheSim.Tick && ReoffersLeft > 0) {
                    --ReoffersLeft;
                    SendResyncOffer();
                }
            }
            Awaiting = false;              // reconciled either way; resume live
            AwaitingNs = 0;
        }
    }
#if LUR_INTERNAL
    else if (Type == MsgCvar) {
        Lur::Serialization::BitReader R(Data, N);
        const uint32_t ApplyTick = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
        const uint8_t  Id  = static_cast<uint8_t>(R.ReadBits(8));
        const uint32_t Hi  = R.ReadBits(32);
        const uint32_t Lo  = R.ReadBits(32);
        const int32_t  Raw = static_cast<int32_t>(R.ReadBits(32));
        if (!Awaiting && R.IsOk()) {
            StorePendingCvar(ApplyTick, Id, Raw, (static_cast<uint64_t>(Hi) << 32) | Lo);
            MergeCvar(Id, Raw, (static_cast<uint64_t>(Hi) << 32) | Lo);
        }
        // No Execute() kick: overrides don't gate the ceiling (only inputs do); the override
        // lands when tick ApplyTick runs. Reliable+ordered transport + the Delay horizon put
        // it in hand before either peer reaches that tick.
    }
    else if (Type == MsgCvarSync) {
        ++CvarSyncsSeen_;   // #169: "never heard from the peer" is the failure — see TryStartMatch
        // A sync that arrives AFTER tick 0 is dropped, and that is a correctness rule, not caution.
        // The whole-set exchange has no agreed apply tick (unlike MsgCvar, which is stamped): merging
        // it mid-match moves TheSim.Cv the instant the datagram lands, i.e. on whatever tick each peer
        // happened to be on. Two peers applying the same set on different ticks diverge — so honouring
        // a late sync would CAUSE the desync it exists to prevent. Pre-tick-0 is the only safe window,
        // and it is the only one the protocol needs: every offer precedes its sender's camp, and the
        // match cannot start until both camps are in. #169 made this reachable by re-offering on every
        // resync, so the rule has to be stated rather than merely relied upon.
        if (MatchStarted_) {
            Lur::Log::Info("RPS: ignoring a cvar sync that arrived mid-match (tick %u) — the set can "
                           "only move on a stamped tick once the match is running", TheSim.Tick);
            return;
        }
        Lur::Serialization::BitReader R(Data, N);
        const uint32_t Count = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
        for (uint32_t I = 0; I < Count && R.IsOk(); ++I) {
            const uint8_t  Id  = static_cast<uint8_t>(R.ReadBits(8));
            const uint32_t Hi  = R.ReadBits(32);
            const uint32_t Lo  = R.ReadBits(32);
            const int32_t  Raw = static_cast<int32_t>(R.ReadBits(32));
            if (R.IsOk()) MergeCvar(Id, Raw, (static_cast<uint64_t>(Hi) << 32) | Lo);
        }
        if (R.IsOk()) ApplyActiveCvars();  // both peers now hold the identical merged set (pre-tick-0)
    }
    else if (Type == MsgFingerprint) {
        // Compare the peer's compile-time fingerprint to ours; a mismatch means different
        // builds -> refuse the match (the app checks BuildMismatch() and aborts before
        // tick 0). Loud, located, and BEFORE any divergence instead of a mid-match draw.
        const char* Mine = Lur::BuildFingerprint();
        const std::size_t Ml = std::strlen(Mine);
        if (N != Ml || std::memcmp(Data, Mine, Ml) != 0) {
            BuildMismatch_ = true;
            Lur::Log::Error("RPS: build-fingerprint mismatch — peer '%.*s' vs local '%s' "
                            "(refuse match; rebuild both from the same commit)",
                            static_cast<int>(N), reinterpret_cast<const char*>(Data), Mine);
        }
    }
#endif
}

// #148: send our executed history + the frontier marker. Split out of BeginResync so the peer
// that is AHEAD can re-send it: a rejoining peer's Lp.Init can wipe an offer that arrived before
// it (Session::Tick delivers datagrams BEFORE the main reaches its "session ready -> Lp.Init"
// branch), and then nobody would ever hand it the history again.
void LockstepPeer::SendResyncOffer() {
    const uint32_t F = TheSim.Tick;  // our executed frontier
    // Reconstruct the executed COMBINED history (team0-first per tick, the Execute order) so a
    // rejoiner replays it through a fresh sim and both split it back per team.
    std::vector<std::vector<InputEvent>> Hist;
    Hist.reserve(F);
    for (uint32_t T = 0; T < F; ++T) {
        const std::vector<InputEvent>& L = LocalEvents[T];
        const std::vector<InputEvent>& P = PeerEvents[T];
        std::vector<InputEvent> C;
        C.reserve(L.size() + P.size());
        const std::vector<InputEvent>& First  = MyTeam == 0 ? L : P;
        const std::vector<InputEvent>& Second = MyTeam == 0 ? P : L;
        C.insert(C.end(), First.begin(), First.end());
        C.insert(C.end(), Second.begin(), Second.end());
        Hist.push_back(std::move(C));
    }
    const std::vector<std::vector<uint8_t>> Chunks = EncodeEventResyncChunks(0, Hist);
    for (const std::vector<uint8_t>& C : Chunks) {
        std::vector<uint8_t> Payload;
        Payload.reserve(C.size() + 1);
        Payload.push_back(0);  // tag 0 = history chunk
        Payload.insert(Payload.end(), C.begin(), C.end());
        if (Send) Send(Ctx, MsgResyncChunk, Payload.data(), Payload.size());
    }
    Lur::Serialization::BitWriter W;  // completion marker [0xFF][varint frontier]
    Lur::Serialization::WriteVarUint(W, F);
    const std::vector<uint8_t>& MB = W.Finish();
    std::vector<uint8_t> Marker;
    Marker.reserve(MB.size() + 1);
    Marker.push_back(ResyncTagMarker);
    Marker.insert(Marker.end(), MB.begin(), MB.end());
    if (Send) Send(Ctx, MsgResyncChunk, Marker.data(), Marker.size());
}

void LockstepPeer::BeginResync() {
    SendResyncOffer();
#if LUR_INTERNAL
    // #169: re-offer the TUNABLES too, not just the input history. This is #148's hole, in the one
    // place #148 didn't look.
    //
    // SendCvarSync used to be called exactly once, by the main, next to Lp.Init — so the exchange
    // only converged when BOTH peers Init'd while the link was up. It usually is, which is why this
    // passed for weeks. But when one phone's app restarts and the other keeps running, the restarted
    // peer Inits and sends its set, and the INCUMBENT never re-sends: its Init already happened and
    // the `!Started && PeerReady` gate that calls SendCvarSync can never fire again. The rejoiner
    // therefore simulates on compile-time defaults while the incumbent simulates on its persisted
    // overrides, forever — desync at the first anchor, every match, until someone restarts the other
    // phone too. Measured on hardware 2026-08-01: whichever phone launched LAST ran the whole session
    // un-merged (miner 600/gold 800 against the Galaxy's 400/750), across four consecutive matches.
    //
    // The asymmetry is what hid it: the incumbent is never wrong (it merged its own set when it seeded
    // it), and the rejoiner's own empty set merges into the incumbent as a no-op. Both peers report a
    // clean exchange; only the pre-tick-0 state differs.
    //
    // BeginResync is the right home because it is already the "a peer is (re)joining, reconcile with
    // it" event, called by both mains on entering a match AND by Session's reconnect edge — which is
    // exactly when the other side restarted. Bounded by construction: it re-offers state, it does not
    // reply to an offer, so there is no ping-pong to budget (ReoffersLeft exists for the history
    // because that path DOES answer).
    SendCvarSync();
#endif
    ReseedFrom(TheSim.Tick);  // re-base our own timeline (drops in-flight beyond F); sim already at F
    IncomingHistory.clear();
    Awaiting = true;    // cleared when we process the peer's marker (or by the stall timeout)
    AwaitingNs = 0;
    ReoffersLeft = 1;   // #148: one re-offer per round is enough, and can't ping-pong
}

void LockstepPeer::RebuildFromHistory(uint32_t Frontier) {
    const uint64_t S = TheSim.Seed;
    // #147: ResetSim, NOT TheSim.Init — a rebuild must start from the MERGED cvar set. Re-latching
    // the local globals here silently un-converged an already-synced match on every reconnect (the
    // replay then ran on a different Cv than the peer's), which is the same defect as at match
    // start and the resync path is the easiest one to miss: it only fires after a link blip.
    ResetSim(S);  // fresh sim, same seed
    LocalEvents.clear();
    PeerEvents.clear();
    for (uint32_t T = 0; T < Frontier; ++T) {
        const std::vector<InputEvent>& C = IncomingHistory[T];
        TheSim.StepEvents(C.data(), static_cast<int32_t>(C.size()));  // free-run (the replay law)
        // Split the combined batch back into this peer's local + peer streams by Team.
        std::vector<InputEvent> Loc, Peer;
        for (const InputEvent& E : C) (E.Team == MyTeam ? Loc : Peer).push_back(E);
        LocalEvents.push_back(std::move(Loc));
        PeerEvents.push_back(std::move(Peer));
    }
    ReseedFrom(Frontier);  // sim now at Frontier; append the fresh Delay slack
    IncomingHistory.clear();
    // #139: a cold rejoin resumes an already-running match (the camps are in the replayed
    // history at tick 0), so the ready gate is already satisfied — don't hold the clock.
    MatchStarted_ = true;
    LocalReady_ = PeerReady_ = LocalCampSent_ = true;
}

void LockstepPeer::ReseedFrom(uint32_t Frontier) {
    LocalEvents.resize(Frontier);  // drop anything in-flight beyond the frontier
    PeerEvents.resize(Frontier);
    for (uint32_t I = 0; I < Delay; ++I) {  // fresh empty delay slack, both sides agree
        LocalEvents.push_back({});
        PeerEvents.push_back({});
    }
    WallTicks = Frontier;
    { std::lock_guard<std::mutex> Lock(EventQueueMutex_); PendingLocalEvents.clear(); }
    MyHash.clear();  // old anchors are pre-outage; resume with fresh ones
    PeerHash.clear();
    // #163: both peers re-base their timelines to the same frontier here, so the sequence expectation
    // must move with it. Without this every frame after a resync would read as a gap — the detector
    // would then cry wolf on the ONE path where frames are legitimately dropped, which is precisely
    // how a diagnostic earns its way into being ignored.
    PeerTickNext_ = Frontier + Delay;
}

}  // namespace Rps
