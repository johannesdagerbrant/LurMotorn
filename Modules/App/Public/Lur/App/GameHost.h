#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Lur/Core/Assert.h"
#include "Lur/Net/Session.h"
#include "Lur/Save/SaveState.h"
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Transport/Transport.h"

namespace Lur::App {

// The engine's session + persistence choreography, owned once (issue #43).
//
// WHY THIS EXISTS. Both chess mains carried the same ~70-line block, comment-for-comment:
// Store -> LoadOrCreateDeviceId -> SyncManager -> match-end persist+log -> loggers -> the
// hijack-guarded record send -> ReadyHandler/ResyncHandler/StateHashFn/Sync handler ->
// Session::Start. None of it is chess: it is how the engine links two peers, adopts one, and
// reconciles a per-opponent record. Every future game would have forked it again — and the two
// existing copies had ALREADY drifted, in ways that are worth naming because they are what
// duplication actually costs:
//
//   * ownership     — Android held Store/SyncManager on the stack, iOS on the heap with `new`;
//   * capture style — `[&State, &Sync]` against `[View, Sync, Session]`;
//   * ORDER         — Android wired the view before the send lambda, iOS after;
//   * the log line  — the same MATCH END line printed `%u` on one phone and `%d` on the other,
//                     and only the Android copy labelled the tally `WLD(lo/hi/dr)`. Two phones
//                     reporting one match differently is the shape of bug this batch kept
//                     finding (#202, #204), here sitting in the log format itself.
//
// WHAT STAYS GAME-SIDE. Every *decision* — the #38 hijack rule (which peer we adopt, and whether
// an incoming record may touch ours), what a state hash is, what a match tally means. Those are
// the Hooks below. The host owns the sequencing and nothing else, which is the line the
// shared-first doctrine draws: a platform/engine layer may hold verbs and order, never policy.
class GameHost {
public:
    // WHAT EVERY GAME NEEDS (this struct), versus what only some do (RecordSync below).
    //
    // The split was NOT in the first cut of this class, and RPS is what found it: this started out
    // shaped entirely around chess's per-opponent record — SyncManager, an adopt rule, a state hash —
    // and RPS uses NONE of that. Its `ScoreBook` is not an `ISaveState` at all (it Loads/Saves itself
    // and is never sent over the wire), it has no hijack rule to apply, and its resync means "tell
    // LockstepPeer to rebase", not "re-adopt and re-send a record". Forcing it through the chess shape
    // would have meant a dummy save-state and three no-op hooks — which is the tell that a default is
    // wrong, and exactly the guard rail this phase set for itself: if the second game has to fight the
    // seam, fix the seam.
    struct Hooks {
        // Optional: the game's state hash, for the session's divergence detection (#72). Chess hashes
        // its board; RPS detects divergence itself with per-tick anchors and leaves this unset.
        std::function<uint64_t()> StateHash;

        // Optional: a peer link just came up. Fired on the INITIAL link only.
        std::function<void()> OnLinkReady;

        // Optional: a reconnect, or a forced resync. RPS rebases its lockstep timeline here; chess
        // re-adopts, which the record-sync half below does for it.
        std::function<void()> OnResync;
    };

    // THE OPT-IN HALF: the per-opponent record flow (chess today). Supply this and the host owns a
    // SyncManager and runs the adopt/send/receive choreography; omit it and the host is just identity
    // plus session lifecycle, which is all RPS wants.
    struct RecordSync {
        // A peer link came up (initial link OR a reconnect — both routes land here, so a peer
        // that rejoins is re-adopted rather than only ever adopted on first contact). Return
        // whether it is now our active opponent; the host sends our record iff it is. This is
        // where the game applies the #38 hijack rule.
        std::function<bool(const std::string& PeerGuid)> OnPeerAdopted;

        // Is this peer the one we are actually playing? Gates an INCOMING record, so a third
        // device — or a stale peer we did not adopt — cannot overwrite ours. Separate from
        // OnPeerAdopted because this is asked per message, not per link.
        std::function<bool(const std::string& PeerGuid)> IsActiveOpponent;

        // Optional: the tally to report when a match ends. Both games happen to keep exactly
        // this shape (Chess::ChessRecord, Rps::ScoreBook) anchored to the lower/higher GUID
        // rather than to a player, which is what makes ONE log line correct for both. Supply it
        // and the host emits that line; omit it and the host logs the persist alone.
        struct MatchSummary {
            int      Result     = 0;   // the game's own result enum, printed as an int
            uint32_t WinsLower  = 0;   // wins by the lower-GUID device (player-agnostic)
            uint32_t WinsHigher = 0;
            uint32_t Draws      = 0;
            uint32_t Total      = 0;
        };
        std::function<MatchSummary()> Summarize;
    };

    struct Config {
        std::string SaveDir;                                  // platform: filesDir / App Support
        Lur::Transport::ITransport* Transport = nullptr;       // platform: the BLE backend
        std::function<void(const char*)> Log;                 // platform: the tag lives app-side
    };

    // PHASES, and the ordering is load-bearing rather than stylistic:
    //
    //   Init  -> Store + device id exist (the game can now hand them to its view/UI)
    //   [EnableRecordSync] -> optional; the SyncManager exists from here
    //   Start -> handlers installed, handshake begins
    //
    // The middle matters. The game's view is handed Store/Sync/DeviceId (chess:
    // BoardView::AttachPersistence) and must hold them BEFORE a peer can go ready, because the ready
    // handler calls straight back into the game's adopt rule. Doing it all in one call would leave the
    // game attaching itself *after* the session was live — safe today only because a BLE handshake
    // takes round-trips, which is precisely the "can't happen fast enough" reasoning that has been
    // wrong repeatedly. Each is safe to call once.
    void Init(const Config& Cfg);

    // Opt in to the per-opponent record flow. `State` must outlive the host. Call between Init and
    // Start. A game that never calls this (RPS) gets identity + session lifecycle and nothing else —
    // no SyncManager is constructed, and OnBackground/OnMatchEnded become no-ops it need not call.
    void EnableRecordSync(Lur::Save::ISaveState& State, RecordSync Sync);

    void Start(Hooks GameHooks);

    // Per frame, real-time denominated: drives the handshake, keepalives and liveness.
    void Tick(uint64_t ElapsedNs);

    // Deliver queued inbound datagrams on the engine thread (#40).
    void PumpInbox();

    // The app is going away / to the background: make the in-progress record durable. No-op unless
    // record sync is enabled (a game persisting its own way does that itself).
    void OnBackground();

    // The game's match ended: persist, then report. Call this from the game's own match-end
    // hook — the host cannot know when a match is over, only what to do about it. No-op unless
    // record sync is enabled.
    void OnMatchEnded();

    bool HasRecordSync() const { return Sync_ != nullptr; }

    Lur::Net::Session&      Session()       { return Session_; }
    // ONLY valid after EnableRecordSync. This asserts because getting it wrong is SILENT: the caller
    // hands the resulting pointer to its view, the view's `if (Sync != nullptr)` guard then skips
    // SyncManager::OnLink, the peer key is never set, and Persist() no-ops for the life of the
    // process — the per-opponent record simply stops being written, with every log line still
    // reporting success. That is exactly what happened on device (2026-08-12): both chess mains
    // called AttachPersistence(&Host.Sync(), ...) BEFORE EnableRecordSync, and only a file mtime
    // gave it away. A dereference of a null unique_ptr is UB, so the fix is to trap, not to hope.
    Lur::Save::SyncManager& Sync() {
        LUR_ASSERT(Sync_ != nullptr && "GameHost::Sync() before EnableRecordSync()");
        return *Sync_;
    }
    Lur::Save::Store&       Store()         { return *Store_; }
    const std::string&      DeviceId() const { return DeviceId_; }
    bool                    Started() const { return Started_; }

private:
    void Log(const char* Msg) const { if (Cfg_.Log) Cfg_.Log(Msg); }
    void Logf(const char* Fmt, ...) const;
    // Both the initial link and a reconnect route here: adopt per the game's rule, and send our
    // record only if it adopted. One function, so the two paths cannot drift apart.
    void OnPeerLive();

    Config     Cfg_;
    Hooks      Hooks_;
    RecordSync Record_;
    // Heap-owned so the host is movable-free and the game's main does not choose an ownership
    // model (the two chess mains chose differently, which is half of why this exists).
    std::unique_ptr<Lur::Save::Store>       Store_;
    std::unique_ptr<Lur::Save::SyncManager> Sync_;
    Lur::Net::Session Session_;
    std::string       DeviceId_;
    bool              Started_ = false;
};

} // namespace Lur::App
