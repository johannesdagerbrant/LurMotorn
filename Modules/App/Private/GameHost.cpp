#include "Lur/App/GameHost.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

#include "Lur/Save/DeviceId.h"

namespace Lur::App {

void GameHost::Logf(const char* Fmt, ...) const {
    if (!Cfg_.Log) return;
    char Buf[256];
    va_list Args;
    va_start(Args, Fmt);
    std::vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);
    Cfg_.Log(Buf);
}

// The record half of a link coming up. Both the initial link (ReadyHandler) and a reconnect
// (ResyncHandler) come through here, so a peer that rejoins is adopted by the same rule as one
// arriving for the first time. Keeping it one function is the point: the two mains each wired the
// same lambda into both hooks, and a future edit to one of those call sites would have silently
// changed only half the behaviour.
void GameHost::OnPeerLive() {
    if (!Sync_) return;                       // record sync not enabled: nothing to adopt or send
    const std::string& Peer = Session_.GetPeerGuid();
    // No hook means no adoption: without the game's rule we cannot know whether this peer is the
    // one being played, and sending our record to the wrong peer is the failure the rule exists to
    // prevent. Refusing is the safe direction.
    if (!Record_.OnPeerAdopted || !Record_.OnPeerAdopted(Peer)) return;
    // OnLink AFTER the hook, and yes it is usually the second call — chess's BoardView::OnPeerLinked
    // already reconciles inside the hook. It is here anyway because the host cannot depend on that:
    // OnLink is what sets the peer KEY, and without the key Persist() silently writes nothing, for
    // the life of the process, with every log line still reporting success. That is not theoretical —
    // it is what shipped for one build on 2026-08-12: the mains handed the view a null SyncManager
    // (Sync() called before EnableRecordSync), the view's null-guard skipped its OnLink, and the
    // per-opponent record simply stopped being written. Only a file mtime gave it away.
    //
    // Calling it here costs one idempotent re-reconcile (MergeIfNewer is monotonic, and it runs after
    // the game's own, so the game's ordering is unchanged) and buys a host that persists correctly
    // even if a game forgets. Ownership of persistence and ownership of the key belong together.
    Sync_->OnLink(Peer);
    const std::vector<uint8_t> Snap = Sync_->Snapshot();
    Session_.Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
}

void GameHost::Init(const Config& Cfg) {
    if (Store_) return;
    Cfg_ = Cfg;
    Store_ = std::make_unique<Lur::Save::Store>(Cfg_.SaveDir.empty() ? "." : Cfg_.SaveDir);
    DeviceId_ = Lur::Save::LoadOrCreateDeviceId(*Store_);
    if (Cfg_.Log) Session_.SetLogger([this](const char* M) { Logf("Net: %s", M); });
}

void GameHost::EnableRecordSync(Lur::Save::ISaveState& State, RecordSync Sync) {
    if (!Store_ || Sync_) return;   // Init first; once only
    Record_ = std::move(Sync);
    Sync_ = std::make_unique<Lur::Save::SyncManager>(*Store_, State);
}

void GameHost::Start(Hooks GameHooks) {
    if (Started_ || !Store_) return;   // Init first
    Hooks_ = std::move(GameHooks);

    // Ready/resync fan out to BOTH halves: the record flow (if enabled) and the game's own hook.
    // Chess needs the first, RPS the second (its resync rebases the lockstep timeline), and a game
    // wanting both must not have to choose which one the seam supports.
    Session_.SetReadyHandler([this] {
        OnPeerLive();
        if (Hooks_.OnLinkReady) Hooks_.OnLinkReady();
    });
    Session_.SetResyncHandler([this] {
        OnPeerLive();                                    // reconnect: re-adopt + re-sync
        if (Hooks_.OnResync) Hooks_.OnResync();
    });
    if (Hooks_.StateHash) Session_.SetStateHashFn(Hooks_.StateHash);   // divergence detection (#72)
    if (Sync_) {
        Session_.SetHandler(Lur::Net::EMsgType::Sync,
                            [this](const uint8_t* D, std::size_t N) {
                                // Only the peer we are actually playing may touch our record. Absent
                                // hook -> refuse, same reasoning as OnPeerLive.
                                if (!Record_.IsActiveOpponent) return;
                                if (!Record_.IsActiveOpponent(Session_.GetPeerGuid())) return;
                                Sync_->OnSync(D, N);
                            });
    }
    Session_.Start(Cfg_.Transport, DeviceId_);
    Started_ = true;
    Logf("Net session started (device id %zuB)", DeviceId_.size());
}

void GameHost::Tick(uint64_t ElapsedNs) { Session_.Tick(ElapsedNs); }

void GameHost::PumpInbox() { Session_.PumpInbox(); }

void GameHost::OnBackground() { if (Sync_) Sync_->Persist(); }

void GameHost::OnMatchEnded() {
    if (!Sync_) return;
    Sync_->Persist();   // durable all-time stats the moment a match resolves
    if (!Record_.Summarize) return;
    const RecordSync::MatchSummary S = Record_.Summarize();
    // ONE line, one format, both phones. The two copies of this printed the same numbers with
    // different specifiers and only one of them labelled the tally, so a log from the pair could
    // not be diffed — the tally is anchored to the lower/higher GUID, not to a player, which is
    // exactly why the label matters to whoever reads it.
    Logf("Net: MATCH END result=%d WLD(lo/hi/dr)=%u/%u/%u total=%u",
         S.Result, S.WinsLower, S.WinsHigher, S.Draws, S.Total);
}

} // namespace Lur::App
