// Tests for Lur::Net::RecoveryPolicy — the desync/lost-frame recovery state machine promoted out of
// Rps::LockstepPeer (#161, #167, #201, #210, #212).
//
// This is the object whose rules were each learned from a bug that presented as something else, so the
// tests are named after the bug rather than the method:
//
//   * TestSurvivorNeverAdopts            — #210's history-exchange deadlock
//   * TestFirstAdoptionWaitsThenAsks     — the request that crossed the unsolicited offer (#210)
//   * TestGapBudgetCannotForceADraw      — #167: a lost frame must not spend the desync budget
//   * TestRoundsClimbWhileBudgetResets   — a climbing round count is the nondeterminism diagnostic
//   * TestHeldTimeIsBankedNotDropped     — the opposite choice from SimThread's pre-match hold
//
// Having them here rather than only inside a two-peer soak matters: the soak can tell you the pair
// stopped converging, but not WHICH rule broke.
#include <cstdio>
#include <cstdint>

#include "Lur/Net/RecoveryPolicy.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Net::ERecoveryAction;
using Lur::Net::RecoveryPolicy;
using ETick = RecoveryPolicy::ETick;

constexpr bool Survivor = true;
constexpr bool Adopter = false;

// ---- RULE 2: the survivor OFFERS and is never left adopting ----
// A survivor that also adopted would exchange histories in both directions and neither peer would
// settle. Offer and Adopt are two OUTCOMES of one decision, not two things a peer might do.
static void TestSurvivorNeverAdopts() {
    RecoveryPolicy P;
    CHECK(P.BeginDesync(Survivor) == ERecoveryAction::Offer);
    CHECK(!P.Adopting());
    CHECK(!P.Recovering());          // the survivor is done the moment it has published
    CHECK(P.Attempts() == 1);        // but it still SPENT an attempt
    // ...and it can offer again immediately, because it is not holding anything open.
    CHECK(P.BeginDesync(Survivor) == ERecoveryAction::Offer);
    CHECK(!P.Adopting());
    CHECK(P.Attempts() == 2);
    // Same on the gap path.
    RecoveryPolicy Q;
    CHECK(Q.BeginGap(Survivor) == ERecoveryAction::Offer);
    CHECK(!Q.Adopting() && !Q.Recovering());
}

// ---- RULE 3: the FIRST adoption waits; every later one asks ----
// On the first attempt of the first round the survivor offers unsolicited, and a request crossing that
// offer deadlocked the exchange. From the second attempt on, the offer demonstrably did not arrive.
static void TestFirstAdoptionWaitsThenAsks() {
    RecoveryPolicy P;
    CHECK(P.BeginDesync(Adopter) == ERecoveryAction::WaitForOffer);
    CHECK(P.Recovering() && P.Adopting());
    CHECK(P.Attempts() == 1);
    // A second Begin while a repair is in flight is refused outright.
    CHECK(P.BeginDesync(Adopter) == ERecoveryAction::None);
    CHECK(P.Attempts() == 1);
    // Time out -> the round is over. The NEXT attempt asks.
    P.Fail();
    CHECK(P.Rounds() == 1);
    CHECK(P.BeginDesync(Adopter) == ERecoveryAction::Request);
    // And within a round, attempt 2 asks too (Attempts_ > 1), not just later rounds.
    RecoveryPolicy Q;
    CHECK(Q.BeginDesync(Adopter) == ERecoveryAction::WaitForOffer);
    Q.Finish();
    CHECK(Q.BeginDesync(Adopter) == ERecoveryAction::Request);
}

// ---- RULE 3 does NOT apply to a gap: the adopter always asks ----
// Only one side can see a hole in its own stream, so there is no unsolicited offer to wait for.
// Waiting here would hang the adopter until the timeout on every single lost frame.
static void TestGapAdopterAlwaysAsksImmediately() {
    RecoveryPolicy P;
    CHECK(P.BeginGap(Adopter) == ERecoveryAction::Request);   // never WaitForOffer
    CHECK(P.Recovering() && P.Adopting());
    CHECK(P.GapRepairs() == 1);
    CHECK(P.Attempts() == 0);        // and it did NOT touch the desync budget
}

// ---- RULE 4: a lost frame can never push the match toward the draw a spent desync budget declares ----
static void TestGapBudgetCannotForceADraw() {
    RecoveryPolicy::Config C;
    C.MaxAttemptsPerRound = 3;
    C.MaxGapRepairs = 5;
    RecoveryPolicy P{C};
    // Spend the WHOLE gap budget.
    for (int I = 0; I < 5; ++I) {
        CHECK(P.BeginGap(Adopter) == ERecoveryAction::Request);
        P.Finish();
    }
    CHECK(P.GapRepairs() == 5);
    // Further gaps are simply not repaired — refused, NOT reported as a spent budget, because a spent
    // gap budget must not read as "declare a draw".
    CHECK(P.BeginGap(Adopter) == ERecoveryAction::None);
    CHECK(P.Rounds() == 0);
    // The desync budget is untouched and fully available.
    CHECK(P.Attempts() == 0);
    CHECK(P.BeginDesync(Adopter) == ERecoveryAction::WaitForOffer);
}

// ---- A spent DESYNC budget is reported, so the caller can fail the round ----
static void TestDesyncBudgetReportsSpent() {
    RecoveryPolicy::Config C;
    C.MaxAttemptsPerRound = 3;
    RecoveryPolicy P{C};
    for (int I = 0; I < 3; ++I) {
        CHECK(P.BeginDesync(Survivor) == ERecoveryAction::Offer);
    }
    CHECK(P.Attempts() == 3);
    CHECK(P.BeginDesync(Survivor) == ERecoveryAction::BudgetSpent);
    CHECK(P.Attempts() == 3);        // and it did not consume a fourth
}

// ---- RULE 6: rounds climb (the diagnostic) while each round gets a fresh budget ----
// Replay converges on a lost input and CANNOT converge on nondeterminism, so a round count that keeps
// climbing is the signal to look for a float in sim state or a compiler difference.
static void TestRoundsClimbWhileBudgetResets() {
    RecoveryPolicy::Config C;
    C.MaxAttemptsPerRound = 2;
    RecoveryPolicy P{C};
    for (int Round = 1; Round <= 4; ++Round) {
        CHECK(P.BeginDesync(Adopter) != ERecoveryAction::BudgetSpent);
        P.Finish();
        CHECK(P.BeginDesync(Adopter) != ERecoveryAction::BudgetSpent);
        P.Finish();
        CHECK(P.BeginDesync(Adopter) == ERecoveryAction::BudgetSpent);
        P.Fail();
        CHECK(P.Rounds() == Round);   // climbs...
        CHECK(P.Attempts() == 0);     // ...and the next round starts fresh
    }
}

// ---- The retry ladder doubles and then clamps ----
static void TestRetryBackoffLadder() {
    RecoveryPolicy::Config C;
    C.RetryBaseNs = 1000;
    C.RetryMaxNs = 8000;
    RecoveryPolicy P{C};
    CHECK(P.RetryBackoffNs(1) == 1000);
    CHECK(P.RetryBackoffNs(2) == 2000);
    CHECK(P.RetryBackoffNs(3) == 4000);
    CHECK(P.RetryBackoffNs(4) == 8000);
    CHECK(P.RetryBackoffNs(5) == 8000);    // stops at the ceiling, not hammering
    CHECK(P.RetryBackoffNs(50) == 8000);
    CHECK(P.RetryBackoffNs(0) == 1000);    // degenerate round numbers do not underflow the loop

    // The CLAMP is only observable when the doubling OVERSHOOTS the ceiling — with a power-of-two
    // ceiling the loop guard happens to stop exactly on it, so a missing clamp passes unnoticed. Pin a
    // ceiling that is not a doubling of the base: 1000 -> 2000 -> 4000 -> 8000 must come back as 5000,
    // not 8000, or a long-running match waits longer between retries than the ladder claims.
    RecoveryPolicy::Config D;
    D.RetryBaseNs = 1000;
    D.RetryMaxNs = 5000;
    RecoveryPolicy Q{D};
    CHECK(Q.RetryBackoffNs(3) == 4000);
    CHECK(Q.RetryBackoffNs(4) == 5000);
    CHECK(Q.RetryBackoffNs(9) == 5000);
    // And a ceiling BELOW the base clamps immediately rather than returning the larger base.
    RecoveryPolicy::Config E;
    E.RetryBaseNs = 9000;
    E.RetryMaxNs = 500;
    RecoveryPolicy R{E};
    CHECK(R.RetryBackoffNs(1) == 500);
}

// ---- An adopter that never receives the history TIMES OUT ----
static void TestAdopterTimesOut() {
    RecoveryPolicy::Config C;
    C.TimeoutNs = 1000;
    RecoveryPolicy P{C};
    P.BeginDesync(Adopter);
    CHECK(P.Advance(400, /*DesyncOutstanding*/ true) == ETick::None);
    CHECK(P.Advance(400, true) == ETick::None);
    CHECK(P.Advance(400, true) == ETick::Timeout);   // 1200 >= 1000
    P.Fail();
    CHECK(!P.Recovering());
    // A survivor is never in flight, so it can never time out.
    RecoveryPolicy Q{C};
    Q.BeginDesync(Survivor);
    CHECK(Q.Advance(99999, true) == ETick::None);
}

// ---- The retry timer fires ONCE, and only while a divergence is outstanding ----
// Without the outstanding check, a desync that resolved by other means would keep re-arming forever.
static void TestRetryFiresOnceAndOnlyWhileOutstanding() {
    RecoveryPolicy::Config C;
    C.RetryBaseNs = 1000;
    RecoveryPolicy P{C};
    P.BeginDesync(Adopter);
    P.Fail();
    CHECK(P.RetryNs() == 1000);
    CHECK(P.Advance(500, true) == ETick::None);
    CHECK(P.RetryNs() == 500);
    CHECK(P.Advance(500, true) == ETick::RetryDue);
    CHECK(P.RetryNs() == 0);
    CHECK(P.Advance(500, true) == ETick::None);     // fires once, not on every later tick
    // Now the same with the divergence already resolved: the timer still drains, but nothing fires.
    RecoveryPolicy Q{C};
    Q.BeginDesync(Adopter);
    Q.Fail();
    CHECK(Q.Advance(2000, /*DesyncOutstanding*/ false) == ETick::None);
    CHECK(Q.RetryNs() == 0);
    CHECK(Q.Advance(2000, false) == ETick::None);
}

// ---- A retry timer does not run while a repair is in flight ----
static void TestRetryTimerIsPausedWhileRecovering() {
    RecoveryPolicy::Config C;
    C.RetryBaseNs = 1000;
    C.TimeoutNs = 100000;
    RecoveryPolicy P{C};
    P.BeginDesync(Adopter);
    P.Fail();                                  // arms 1000
    P.BeginDesync(Adopter);                    // in flight again
    CHECK(P.Recovering());
    CHECK(P.Advance(5000, true) == ETick::None);
    CHECK(P.RetryNs() == 1000);                // untouched: the ladder measures gaps BETWEEN rounds
}

// ---- RULE 5: held time is BANKED and handed back, so no tick is lost ----
// The opposite choice from SimThread's pre-match hold, which DROPS held time. A pre-match hold is the
// player thinking; a repair is the match owing ticks it must still run.
static void TestHeldTimeIsBankedNotDropped() {
    RecoveryPolicy P;
    P.BankHeldTime(300);
    P.BankHeldTime(700);
    CHECK(P.TakeHeldTime() == 1000);       // accumulated
    CHECK(P.TakeHeldTime() == 0);          // and handed back exactly once
    P.BankHeldTime(50);
    P.Finish();
    CHECK(P.TakeHeldTime() == 50);         // Finish must not silently discard owed ticks
}

// ---- ResetForMatch clears both budgets but keeps the lifetime round count ----
// Rule 4: a match that needed two repairs must not start the next one inside a spent budget. The round
// count is left alone because "how many rounds have we ever needed" is the nondeterminism signal.
static void TestResetForMatch() {
    RecoveryPolicy P;
    P.BeginDesync(Adopter);
    P.BankHeldTime(500);
    P.Fail();
    P.BeginGap(Adopter);
    CHECK(P.Rounds() == 1);
    CHECK(P.GapRepairs() == 1);
    P.ResetForMatch();
    CHECK(!P.Recovering() && !P.Adopting());
    CHECK(P.Attempts() == 0);
    CHECK(P.GapRepairs() == 0);
    CHECK(P.TakeHeldTime() == 0);          // a fresh match owes no ticks from the previous one
    CHECK(P.Rounds() == 1);                // kept
    CHECK(P.BeginDesync(Adopter) == ERecoveryAction::Request);   // rule 3: Rounds_ > 0, so it asks
}

// ---- Finish clears the in-flight state without touching either budget ----
static void TestFinishKeepsBudgetsSpent() {
    RecoveryPolicy P;
    P.BeginDesync(Adopter);
    P.Finish();
    CHECK(!P.Recovering() && !P.Adopting());
    CHECK(P.Attempts() == 1);              // the attempt is still spent — a repair that WORKED still cost one
    CHECK(P.Rounds() == 0);                // and it did not advance the round
}

int main() {
    TestSurvivorNeverAdopts();
    TestFirstAdoptionWaitsThenAsks();
    TestGapAdopterAlwaysAsksImmediately();
    TestGapBudgetCannotForceADraw();
    TestDesyncBudgetReportsSpent();
    TestRoundsClimbWhileBudgetResets();
    TestRetryBackoffLadder();
    TestAdopterTimesOut();
    TestRetryFiresOnceAndOnlyWhileOutstanding();
    TestRetryTimerIsPausedWhileRecovering();
    TestHeldTimeIsBankedNotDropped();
    TestResetForMatch();
    TestFinishKeepsBudgetsSpent();
    if (GFailures == 0) std::printf("recovery_policy_tests: ALL PASS\n");
    else std::printf("recovery_policy_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
