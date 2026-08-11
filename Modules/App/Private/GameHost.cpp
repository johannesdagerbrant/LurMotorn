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

// Both the initial link (ReadyHandler) and a reconnect (ResyncHandler) come through here, so a peer
// that rejoins is adopted by the same rule as one arriving for the first time. Keeping it one
// function is the point: the two mains each wired the same lambda into both hooks, and a future
// edit to one of those call sites would have silently changed only half the behaviour.
void GameHost::OnPeerLive() {
    const std::string& Peer = Session_.GetPeerGuid();
    // No hook means no adoption: without the game's rule we cannot know whether this peer is the
    // one being played, and sending our record to the wrong peer is the failure the rule exists to
    // prevent. Refusing is the safe direction.
    if (!Hooks_.OnPeerAdopted || !Hooks_.OnPeerAdopted(Peer)) return;
    // NOTE the host does NOT call Sync::OnLink here. Adoption is one game-side step — set identity,
    // load that opponent's record, reconcile — and the game already does the reconcile inside the
    // hook (chess: BoardView::OnPeerLinked). Calling it again would be a harmless second
    // MergeIfNewer, but it would move the reconcile relative to the identity change, and "behaviour
    // on device is unchanged" is an acceptance criterion of this extraction, not a nice-to-have.
    const std::vector<uint8_t> Snap = Sync_->Snapshot();
    Session_.Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
}

void GameHost::Init(const Config& Cfg, Lur::Save::ISaveState& State) {
    if (Sync_) return;
    Cfg_ = Cfg;
    Store_ = std::make_unique<Lur::Save::Store>(Cfg_.SaveDir.empty() ? "." : Cfg_.SaveDir);
    DeviceId_ = Lur::Save::LoadOrCreateDeviceId(*Store_);
    Sync_ = std::make_unique<Lur::Save::SyncManager>(*Store_, State);
    if (Cfg_.Log) Session_.SetLogger([this](const char* M) { Logf("Net: %s", M); });
}

void GameHost::Start(Hooks GameHooks) {
    if (Started_ || !Sync_) return;   // Init first: the handlers below need Store/Sync to exist
    Hooks_ = std::move(GameHooks);

    Session_.SetReadyHandler([this] { OnPeerLive(); });
    Session_.SetResyncHandler([this] { OnPeerLive(); });   // reconnect: re-adopt + re-sync
    if (Hooks_.StateHash) Session_.SetStateHashFn(Hooks_.StateHash);   // divergence detection (#72)
    Session_.SetHandler(Lur::Net::EMsgType::Sync,
                        [this](const uint8_t* D, std::size_t N) {
                            // Only the peer we are actually playing may touch our record. Absent
                            // hook -> refuse, same reasoning as OnPeerLive.
                            if (!Hooks_.IsActiveOpponent) return;
                            if (!Hooks_.IsActiveOpponent(Session_.GetPeerGuid())) return;
                            Sync_->OnSync(D, N);
                        });
    Session_.Start(Cfg_.Transport, DeviceId_);
    Started_ = true;
    Logf("Net session started (device id %zuB)", DeviceId_.size());
}

void GameHost::Tick(uint64_t ElapsedNs) { Session_.Tick(ElapsedNs); }

void GameHost::PumpInbox() { Session_.PumpInbox(); }

void GameHost::OnBackground() { if (Sync_) Sync_->Persist(); }

void GameHost::OnMatchEnded() {
    if (Sync_) Sync_->Persist();   // durable all-time stats the moment a match resolves
    if (!Hooks_.Summarize) return;
    const Hooks::MatchSummary S = Hooks_.Summarize();
    // ONE line, one format, both phones. The two copies of this printed the same numbers with
    // different specifiers and only one of them labelled the tally, so a log from the pair could
    // not be diffed — the tally is anchored to the lower/higher GUID, not to a player, which is
    // exactly why the label matters to whoever reads it.
    Logf("Net: MATCH END result=%d WLD(lo/hi/dr)=%u/%u/%u total=%u",
         S.Result, S.WinsLower, S.WinsHigher, S.Draws, S.Total);
}

} // namespace Lur::App
