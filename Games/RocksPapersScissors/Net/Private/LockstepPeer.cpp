#include "Rps/LockstepPeer.h"

#include <cstring>

#include "Lur/Core/Assert.h"
#include "Lur/Core/Log.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Rps/EventCodec.h"

#ifndef LUR_BUILD_FP
#define LUR_BUILD_FP "no-fp"  // fallback when the build system didn't inject one (#112)
#endif

namespace Rps {

void LockstepPeer::Init(uint64_t Seed, uint8_t InMyTeam, SendFn InSend, void* InCtx) {
    TheSim.Init(Seed);
    MyTeam = InMyTeam & 1u;
    Send = InSend;
    Ctx = InCtx;
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
    LocalCamp_ = InputEvent{};
    PeerCamp_ = InputEvent{};
}

void LockstepPeer::QueueLocalEvent(InputEvent E) {
    E.Team = MyTeam;  // authoritative — the UI can only ever act for its own team
    std::lock_guard<std::mutex> Lock(EventQueueMutex_);
    PendingLocalEvents.push_back(E);
}

void LockstepPeer::ProduceAndSend(const std::vector<InputEvent>& Batch) {
    LocalEvents.push_back(Batch);  // lands at exec tick Delay + WallTicks
    Lur::Serialization::BitWriter W;
    EncodeEventBatch(W, Batch.data(), static_cast<int>(Batch.size()));  // one framed batch per tick
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgInput, B.data(), B.size());
}

void LockstepPeer::Tick(uint64_t ElapsedNs) {
    if (Awaiting) return;  // in a resync exchange: hold production/execution until reconciled
#if LUR_INTERNAL
    DrainCvarQueue();  // apply any UI-thread gameplay-CVar edits (stamps + sends MsgCvar)
#endif
    // #139: pre-match — hold the clock (ElapsedNs is dropped, so the match times from start, not
    // from the menu) while the two camps are exchanged. ALWAYS return this call: whether the
    // match is still pending OR just started here, no wall tick is produced yet, so both peers
    // begin advancing from WallTicks==0 on their NEXT Tick — symmetric, no start-skew (one peer
    // readies during its own Tick, the other during a delivered message).
    if (!MatchStarted_) { PreMatchTick(); return; }
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
}

// #139 match-start: pre-match, the clock is held. Capture the local camp (the first miner-place
// the UI queued) as tick 0's local input, send it once so the peer can mirror it, and start the
// match the moment both camps are in. No wall ticks are produced/executed until then.
void LockstepPeer::PreMatchTick() {
    if (!LocalReady_) {
        std::lock_guard<std::mutex> Lock(EventQueueMutex_);
        for (const InputEvent& E : PendingLocalEvents)
            if (E.Kind == EventPlaceBuilding && E.Type == UnitMiner) { LocalCamp_ = E; LocalReady_ = true; break; }
        PendingLocalEvents.clear();  // pre-match: only the mining camp is accepted; drop the rest
    }
    if (LocalReady_ && !LocalCampSent_) {
        Lur::Serialization::BitWriter W;
        EncodeEventBatch(W, &LocalCamp_, 1);          // the same framed batch as a live input tick
        const std::vector<uint8_t>& B = W.Finish();
        if (Send) Send(Ctx, MsgInput, B.data(), B.size());
        LocalCampSent_ = true;
    }
    TryStartMatch();
}

// Both camps in hand -> make them tick 0's input on BOTH peers and start the clock. LocalEvents[0]
// = our camp, PeerEvents[0] = the peer's; Execute combines team0-first, so both peers apply the
// identical [team0 camp, team1 camp] at tick 0 and diverge from an identical state. The Delay-1
// pre-seeded empties after index 0 stay the delay buffer; real input still lands at Delay+.
void LockstepPeer::TryStartMatch() {
    if (MatchStarted_ || !LocalReady_ || !PeerReady_) return;
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
    const auto It = ActiveCvars.find(Id);
    if (It == ActiveCvars.end()) { ActiveCvars[Id] = {Raw, WallMs}; return; }
    if (WallMs > It->second.WallMs)                          It->second = {Raw, WallMs};
    else if (WallMs == It->second.WallMs && Raw != It->second.Raw) ActiveCvars.erase(It);
    // else: incoming is older, or identical -> keep existing.
}

void LockstepPeer::ApplyActiveCvars() {
    TheSim.Cv = LatchCvs();  // reset to the compile-time defaults...
    for (const auto& [Id, V] : ActiveCvars) ApplyCvOverride(TheSim.Cv, Id, V.Raw);  // ...then overlay
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
    const char* Fp = LUR_BUILD_FP;
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

void LockstepPeer::CrossCheck(uint32_t Tick) {
    const auto Mine = MyHash.find(Tick);
    const auto Theirs = PeerHash.find(Tick);
    if (Mine != MyHash.end() && Theirs != PeerHash.end() && Mine->second != Theirs->second)
        Desync = true;  // reliable transport + deterministic sim => a mismatch is always a bug
}

// Resync chunk tags: 0/1 = the team-0/team-1 history streams; 0xFF = the frontier marker.
namespace {
constexpr uint8_t ResyncTagMarker = 0xFF;
}  // namespace

void LockstepPeer::OnMessage(Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N) {
    if (Type == MsgInput) {
        Lur::Serialization::BitReader R(Data, N);
        InputEvent Buf[MaxEventsPerTick];
        const int Cnt = DecodeEventBatch(R, Buf, MaxEventsPerTick);
        if (!MatchStarted_) {
            // #139 pre-match: the peer's FIRST frame is its start camp (its "ready"); any later
            // frame is its post-match input arriving before we've started — buffer it (it lands
            // at PeerEvents[Delay]+ once we start) so nothing is dropped across the skew.
            if (Cnt < 0) return;
            if (!PeerReady_) { if (Cnt >= 1) { PeerCamp_ = Buf[0]; PeerReady_ = true; TryStartMatch(); } }
            else PeerEvents.emplace_back(Buf, Buf + Cnt);
            return;
        }
        if (!Awaiting && Cnt >= 0) {
            PeerEvents.emplace_back(Buf, Buf + Cnt);  // live wire: each Input frame = next peer exec tick
            Execute();                                // peer input may unblock the ceiling
        }
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
        } else if (Tag == ResyncTagMarker) {
            Lur::Serialization::BitReader R(Data + 1, N - 1);
            const uint32_t F = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
            if (R.IsOk() && F > TheSim.Tick && IncomingHistory.size() >= F)
                RebuildFromHistory(F);  // peer is ahead -> adopt its history
            else IncomingHistory.clear();  // we're ahead / short -> keep ours
            Awaiting = false;              // reconciled either way; resume live
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
        const char* Mine = LUR_BUILD_FP;
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

void LockstepPeer::BeginResync() {
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

    ReseedFrom(F);  // re-base our own timeline (drops in-flight beyond F); sim already at F
    IncomingHistory.clear();
    Awaiting = true;  // cleared when we process the peer's marker
}

void LockstepPeer::RebuildFromHistory(uint32_t Frontier) {
    const uint64_t S = TheSim.Seed;
    TheSim.Init(S);  // fresh sim, same seed
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
}

}  // namespace Rps
