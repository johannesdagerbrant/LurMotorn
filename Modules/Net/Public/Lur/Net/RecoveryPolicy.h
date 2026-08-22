#pragma once
// The desync/lost-frame RECOVERY state machine, with no wire and no sim in it.
//
// Promoted out of Rps::LockstepPeer (#161, #167, #201, #210, #212). This is the object that decides
// WHETHER to repair, WHO repairs, HOW MANY times, and WHEN to give up and retry. The caller keeps
// everything that touches bytes or state: publishing history, requesting it, re-basing a timeline,
// logging.
//
// It is promoted as a policy rather than left inline because every rule below was learned from a bug
// that presented as something else, and because a state machine with no I/O in it can be tested
// exhaustively — which is what the entangled version could not be.
//
// ---- 1. A DESYNC MUST RECOVER THE MATCH, NOT END IT ----
// The first response gated the exec loop and nothing ever cleared it, so the match simply STOPPED:
// both peers pinned at the same tick with different hashes, datagrams still flowing, nothing on
// screen, no way out but killing the app. The second response declared a draw to escape that freeze —
// survivable, but it throws away a playable match and tells the players something untrue. A draw is
// only acceptable after recovery has failed its bounded attempts.
//
// ---- 2. THE SURVIVOR NEVER ADOPTS ----
// Authority is decided by a symmetric tie-break (the lower device id), so both peers compute the SAME
// survivor — that is the consistency rule: a contested outcome must resolve the same way on both
// screens. A survivor that also tried to adopt would exchange histories in both directions and neither
// peer would ever settle. Offer and Adopt are therefore mutually exclusive OUTCOMES of one decision,
// not two things a peer might do.
//
// ---- 3. THE FIRST ADOPTION WAITS; LATER ONES ASK ----
// On the very first attempt of the very first round, the adopter must NOT send a request: the survivor
// reaches the same cross-check and offers unsolicited, and a request crossing that offer deadlocked
// the exchange (#210). From the second attempt on — or any later round — the unsolicited offer
// demonstrably did not arrive, so asking is the only thing that can unstrand the pair.
//
// ---- 4. TWO SEPARATE BUDGETS ----
// A lost frame must never push the match toward the draw that a spent desync budget declares (#167),
// so gap repairs are counted apart from desync attempts. Both are PER MATCH: a match that needed two
// repairs must not start the next one already inside a spent budget.
//
// ---- 5. HELD TIME IS BANKED AND GIVEN BACK ----
// Execution is gated while a repair runs, and the wall time that passes is handed back afterwards so
// no tick is lost. This is the opposite choice from SimThread's pre-match hold, which DROPS held time —
// and the difference is the point: a pre-match hold is the player thinking, while a repair is the
// match owing ticks it must still run.
//
// ---- 6. A ROUND COUNT THAT KEEPS CLIMBING MEANS NONDETERMINISM ----
// Replay converges on a lost input and CANNOT converge on nondeterminism (a float in sim state, a
// compiler difference). So the retry ladder backs off exponentially rather than hammering, and the
// round count is the diagnostic to read.
#include <cstdint>

namespace Lur::Net {

// What the caller must DO as a result of a Begin* call.
enum class ERecoveryAction : uint8_t {
    None,         // refused — a repair is already in flight
    Offer,        // we are the survivor: publish our history and re-base to what we published
    Request,      // we are the adopter: hold production/execution AND ask for the history
    WaitForOffer, // we are the adopter: hold production/execution and wait (rule 3)
    BudgetSpent,  // nothing left in this round's budget — the caller should Fail()
};

// Declared at namespace scope, not nested: a defaulted constructor argument cannot see the default
// member initializers of a class nested inside its own enclosing class (they are not available until
// the enclosing class is complete).
struct RecoveryConfig {
    // Attempts inside ONE round before the round is declared spent.
    int MaxAttemptsPerRound = 3;
    // Lost-frame repairs per match, counted apart from desync attempts (rule 4).
    int MaxGapRepairs = 16;
    // How long an adopter waits for the survivor's history before failing the round.
    uint64_t TimeoutNs = 4'000'000'000ull;
    // Exponential retry ladder between rounds (rule 6).
    uint64_t RetryBaseNs = 1'000'000'000ull;
    uint64_t RetryMaxNs = 16'000'000'000ull;
};

class RecoveryPolicy {
public:
    using Config = RecoveryConfig;

    explicit RecoveryPolicy(RecoveryConfig C = RecoveryConfig{}) : Cfg(C) {}

    const RecoveryConfig& GetConfig() const { return Cfg; }

    // ---- Entry points -------------------------------------------------------------------------
    // Called on an anchor-hash mismatch. The caller must already have decided that the match is live
    // and that it is not otherwise blocked; this owns only its own in-flight state.
    ERecoveryAction BeginDesync(bool IsSurvivor) {
        if (Recovering_) return ERecoveryAction::None;   // one round at a time; the timeout ends it
        if (Attempts_ >= Cfg.MaxAttemptsPerRound) return ERecoveryAction::BudgetSpent;
        ++Attempts_;
        if (IsSurvivor) {
            // Rule 2: the survivor publishes and is immediately done — it never enters the waiting
            // state, so it cannot be simultaneously offering and adopting.
            Adopting_ = false;
            return ERecoveryAction::Offer;
        }
        Recovering_ = true;
        Adopting_ = true;
        ElapsedNs_ = 0;
        // Rule 3: only ask once the unsolicited offer has demonstrably not arrived.
        return (Attempts_ > 1 || Rounds_ > 0) ? ERecoveryAction::Request
                                             : ERecoveryAction::WaitForOffer;
    }

    // Called when a peer input frame is known to be missing. Separate budget (rule 4), and the adopter
    // ALWAYS asks here: unlike an anchor mismatch, only one side can see a hole in its own stream, so
    // there is no unsolicited offer to wait for.
    ERecoveryAction BeginGap(bool IsSurvivor) {
        if (Recovering_) return ERecoveryAction::None;
        if (GapRepairs_ >= Cfg.MaxGapRepairs) return ERecoveryAction::None;
        ++GapRepairs_;
        if (IsSurvivor) {
            Adopting_ = false;
            return ERecoveryAction::Offer;
        }
        Recovering_ = true;
        Adopting_ = true;
        ElapsedNs_ = 0;
        return ERecoveryAction::Request;
    }

    // The survivor's history arrived and was applied: we are provably converged.
    void Finish() {
        Recovering_ = false;
        Adopting_ = false;
        ElapsedNs_ = 0;
    }

    // This round ended without converging. Arms the backoff and hands the next round a fresh budget —
    // the round counter, which is the nondeterminism diagnostic, keeps climbing.
    void Fail() {
        Recovering_ = false;
        Adopting_ = false;
        ElapsedNs_ = 0;
        ++Rounds_;
        Attempts_ = 0;
        RetryNs_ = RetryBackoffNs(Rounds_);
    }

    // ---- Timing -------------------------------------------------------------------------------
    enum class ETick : uint8_t {
        None,
        Timeout,   // the history never arrived: the caller should Fail()
        RetryDue,  // the backoff expired: the caller should Begin* again
    };

    // Advance both timers. DesyncOutstanding says whether there is still a divergence worth retrying;
    // without it a resolved desync would keep re-arming forever.
    ETick Advance(uint64_t ElapsedNs, bool DesyncOutstanding) {
        if (!Recovering_ && RetryNs_ > 0) {
            RetryNs_ = ElapsedNs >= RetryNs_ ? 0 : RetryNs_ - ElapsedNs;
            if (RetryNs_ == 0 && DesyncOutstanding) return ETick::RetryDue;
        }
        if (Recovering_) {
            ElapsedNs_ += ElapsedNs;
            if (ElapsedNs_ >= Cfg.TimeoutNs) return ETick::Timeout;
        }
        return ETick::None;
    }

    // Rule 5: wall time that passed while a repair gated execution, handed back so no tick is lost.
    void BankHeldTime(uint64_t Ns) { CarryNs_ += Ns; }
    uint64_t TakeHeldTime() {
        const uint64_t N = CarryNs_;
        CarryNs_ = 0;
        return N;
    }

    // ---- State --------------------------------------------------------------------------------
    bool Recovering() const { return Recovering_; }
    bool Adopting() const { return Adopting_; }
    int Attempts() const { return Attempts_; }
    int Rounds() const { return Rounds_; }
    int GapRepairs() const { return GapRepairs_; }
    uint64_t RetryNs() const { return RetryNs_; }

    // A fresh match: both budgets reset (rule 4), and a fresh match cannot be mid-recovery. Rounds_
    // deliberately survives nothing here — the caller decides, because "how many rounds did this match
    // need" is the per-match diagnostic.
    void ResetForMatch() {
        Recovering_ = false;
        Adopting_ = false;
        Attempts_ = 0;
        GapRepairs_ = 0;
        ElapsedNs_ = 0;
        CarryNs_ = 0;
    }

    uint64_t RetryBackoffNs(int Round) const {
        uint64_t Ns = Cfg.RetryBaseNs;
        for (int I = 1; I < Round && Ns < Cfg.RetryMaxNs; ++I) Ns *= 2;
        return Ns > Cfg.RetryMaxNs ? Cfg.RetryMaxNs : Ns;
    }

private:
    RecoveryConfig Cfg;
    bool Recovering_ = false;
    bool Adopting_ = false;
    int Attempts_ = 0;
    int Rounds_ = 0;
    int GapRepairs_ = 0;
    uint64_t ElapsedNs_ = 0;
    uint64_t RetryNs_ = 0;
    uint64_t CarryNs_ = 0;
};

}  // namespace Lur::Net
