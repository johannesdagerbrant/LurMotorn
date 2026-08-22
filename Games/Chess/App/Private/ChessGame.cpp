#include "Chess/App/ChessGame.h"

namespace Chess {

void ChessGame::Configure(Lur::App::GameHost& Host, Lur::App::GameHost::Hooks& Hooks) {
    // ---- The per-opponent record flow (chess's opt-in half of GameHost) ----
    Lur::App::GameHost::RecordSync Rec;
    // The view applies the #38 hijack rule and sets identity + loads the record for the adopted
    // peer; the host sends ours only when it adopted. Both the initial link and a reconnect route
    // through this one hook, so they cannot drift apart.
    Rec.OnPeerAdopted = [this](const std::string& Peer) { return View_.OnPeerLinked(Peer); };
    // Only share OUR game with the peer we are actually playing — a third device, or a stale peer
    // we did not adopt, must not overwrite it.
    Rec.IsActiveOpponent = [this](const std::string& Peer) {
        return View_.ActiveOpponentGuid() == Peer;
    };
    // The tally the host prints when a match ends. Anchored to the lower/higher GUID rather than to
    // a player, which is what makes one log line correct on both phones.
    Rec.Summarize = [this] {
        Lur::App::GameHost::RecordSync::MatchSummary S;
        S.Result     = static_cast<int>(Match_.LastResult());
        S.WinsLower  = Match_.Record().WinsLower;
        S.WinsHigher = Match_.Record().WinsHigher;
        S.Draws      = Match_.Record().Draws;
        S.Total      = Match_.Record().TotalMatches();
        return S;
    };
    Host.EnableRecordSync(Match_, std::move(Rec));

    // ---- Attach the view ----
    // Order is load-bearing and is the reason IGame::Configure runs between Init and Start (see
    // GameHost's phase comment): the view must hold Store/Sync/DeviceId BEFORE a peer can go ready,
    // because the ready handler calls straight back into the adopt rule above.
    View_.SetState(&Match_);
    View_.AttachSession(&Host.Session());
    View_.AttachPersistence(&Host.Store(), &Host.Sync(), Host.DeviceId());
    // Route the view's log lines through the host's sink, so they carry the platform's tag rather
    // than each main installing its own lambda with its own prefix.
    View_.SetLogger([H = &Host](const char* M) { H->Logf("View: %s", M); });

    Match_.SetOnMatchEnd([H = &Host] { H->OnMatchEnded(); });   // persist + report

    // ---- The session's divergence check ----
    // Chess hashes its board so the session can catch a divergence (#72). RPS leaves this unset —
    // it detects divergence itself with per-tick anchors. That asymmetry is why Hooks::StateHash is
    // optional rather than a pure-virtual on IGame.
    Hooks.StateHash = [this] { return Match_.PositionHash(); };
}

}  // namespace Chess
