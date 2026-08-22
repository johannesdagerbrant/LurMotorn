// Tests for Lur::Net::CvarLedger — the gameplay-CVar override merge promoted out of Rps::LockstepPeer
// (#147, #169, #201).
//
// The rule under test is a CONSISTENCY rule, not a fairness one: two peers editing the same tunable in
// the same millisecond have no ordering between them and no referee to ask, so both discard and fall
// back to the compiled default — a value they provably already share. Nobody gets what they typed;
// both screens agree. Getting this wrong makes the two sims differ from tick 0, which presents as an
// economy desync rather than as a settings problem.
#include <cstdio>
#include <cstdint>

#include "Lur/Net/CvarLedger.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Net::CvarEdit;
using Lur::Net::CvarLedger;

static int32_t Val(const CvarLedger& L, uint8_t Id, int32_t IfAbsent = -12345) {
    int32_t R = IfAbsent;
    L.Get(Id, R);
    return R;
}

// ---- An empty ledger overrides nothing ----
static void TestEmpty() {
    CvarLedger L;
    CHECK(L.Count() == 0);
    CHECK(!L.AnyMerged());
    CHECK(!L.Has(0));
    CHECK(!L.Has(255));
    int32_t R = 0;
    CHECK(!L.Get(7, R));
}

// ---- A later edit wins; an earlier one is ignored ----
static void TestLaterEditWins() {
    CvarLedger L;
    L.Merge(10, 100, /*t*/ 1000);
    CHECK(Val(L, 10) == 100);
    L.Merge(10, 200, 2000);
    CHECK(Val(L, 10) == 200);
    L.Merge(10, 300, 1500);        // stale: arrives late but was edited earlier
    CHECK(Val(L, 10) == 200);
    CHECK(L.Count() == 1);         // still one id
}

// ---- A same-value tie is NOT a disagreement ----
// Both peers echoing the same edit (which the sync exchange does) must not erase it.
static void TestSameValueTieIsHarmless() {
    CvarLedger L;
    L.Merge(3, 42, 5000);
    CHECK(L.Has(3) && L.Count() == 1);
    // Assert after EVERY merge, not just at the end. An earlier version of this test merged three
    // times and only checked the result: an implementation that erased on every same-millisecond
    // merge — including an identical one — passed it, because erase-then-re-add on an ODD number of
    // merges lands back on the right answer. The echo count must not decide the verdict.
    L.Merge(3, 42, 5000);
    CHECK(L.Has(3));
    CHECK(Val(L, 3) == 42);
    CHECK(L.Count() == 1);
    L.Merge(3, 42, 5000);
    CHECK(L.Has(3) && Val(L, 3) == 42 && L.Count() == 1);
    L.Merge(3, 42, 5000);
    CHECK(L.Has(3) && Val(L, 3) == 42 && L.Count() == 1);
}

// ---- THE RULE: a same-millisecond DISAGREEMENT erases, falling back to the default ----
static void TestConflictingTieErases() {
    CvarLedger L;
    L.Merge(3, 42, 5000);          // our edit
    L.Merge(3, 99, 5000);          // the peer's, same millisecond, different value
    CHECK(!L.Has(3));              // neither wins; the compiled default is common ground
    CHECK(L.Count() == 0);
    CHECK(L.AnyMerged());          // ...but something WAS discussed — see AnyMerged's contract
}

// ---- The tie-break is SYMMETRIC: both peers reach the same answer ----
// This is the assertion the rule exists for. Peer A sees its own edit first, peer B sees A's first,
// and after both have merged both messages the two ledgers must be identical.
static void TestBothPeersReachTheSameAnswer() {
    CvarLedger A, B;
    // A edited 42 at t=5000; B edited 99 at t=5000.
    A.Merge(3, 42, 5000);          // A's own, first
    A.Merge(3, 99, 5000);          // then B's
    B.Merge(3, 99, 5000);          // B's own, first
    B.Merge(3, 42, 5000);          // then A's
    CHECK(A.Has(3) == B.Has(3));
    CHECK(A.Count() == B.Count());
    CHECK(!A.Has(3));

    // And the ordinary (non-tie) case must also be order-independent.
    CvarLedger C, D;
    C.Merge(3, 42, 5000);
    C.Merge(3, 99, 6000);
    D.Merge(3, 99, 6000);
    D.Merge(3, 42, 5000);
    CHECK(C.Has(3) && D.Has(3));
    CHECK(Val(C, 3) == Val(D, 3));
    CHECK(Val(C, 3) == 99);
}

// ---- An erased id can be set again by a LATER edit ----
// Otherwise one unlucky tie would lock a knob to its default for the rest of the session.
static void TestErasedIdCanBeSetAgain() {
    CvarLedger L;
    L.Merge(3, 42, 5000);
    L.Merge(3, 99, 5000);
    CHECK(!L.Has(3));
    L.Merge(3, 7, 6000);
    CHECK(L.Has(3));
    CHECK(Val(L, 3) == 7);
    CHECK(L.Count() == 1);
    // ...and an edit at the ORIGINAL tie's timestamp does not resurrect the old value.
    L.Merge(3, 42, 5000);
    CHECK(Val(L, 3) == 7);
}

// ---- KNOWN LIMIT, pinned deliberately: three same-millisecond values are ORDER DEPENDENT ----
// Merging (1@t, 2@t) erases, and 4@t then lands as 4; merging (1@t, 4@t) erases, and 2@t lands as 2.
// Reaching this needs one peer to write the same knob twice inside one millisecond. Recorded as a test
// rather than a comment so that if anyone makes the tie-break total, this fails and names the decision.
static void TestThreeWayTieIsOrderDependent() {
    CvarLedger P;
    P.Merge(5, 1, 9000);
    P.Merge(5, 2, 9000);           // erases
    P.Merge(5, 4, 9000);           // lands fresh
    CHECK(Val(P, 5) == 4);

    CvarLedger Q;
    Q.Merge(5, 1, 9000);
    Q.Merge(5, 4, 9000);           // erases
    Q.Merge(5, 2, 9000);           // lands fresh
    CHECK(Val(Q, 5) == 2);

    CHECK(Val(P, 5) != Val(Q, 5)); // <-- the hole. If this line ever fails, the tie-break went total.
}

// ---- Ids are independent, and the whole one-byte range is usable ----
static void TestIdsAreIndependentAcrossTheFullRange() {
    CvarLedger L;
    L.Merge(0, 1, 100);
    L.Merge(255, 2, 100);
    L.Merge(128, 3, 100);
    CHECK(L.Count() == 3);
    CHECK(Val(L, 0) == 1 && Val(L, 255) == 2 && Val(L, 128) == 3);
    L.Merge(0, 9, 50);             // stale on id 0 only
    CHECK(Val(L, 0) == 1);
    CHECK(Val(L, 255) == 2);
    // Erasing one leaves the others alone.
    L.Merge(128, 4, 100);
    CHECK(!L.Has(128));
    CHECK(L.Count() == 2);
    CHECK(L.Has(0) && L.Has(255));
}

// ---- ForEach visits present ids in ASCENDING ORDER ----
// The sync message is built by iterating this. The std::unordered_map it replaces emitted entries in
// an implementation-defined order, so the same override set produced different bytes on the two
// phones — not a bug today, but not a property to rely on by accident either.
static void TestForEachIsAscendingAndSkipsErased() {
    CvarLedger L;
    L.Merge(200, 1, 10);
    L.Merge(5, 2, 10);
    L.Merge(77, 3, 10);
    L.Merge(5, 99, 10);            // conflicting tie -> id 5 is erased
    int Seen[8] = {};
    int N = 0;
    L.ForEach([&](uint8_t Id, const CvarEdit&) { if (N < 8) Seen[N++] = Id; });
    CHECK(N == 2);
    CHECK(Seen[0] == 77 && Seen[1] == 200);   // ascending, and 5 is gone
}

// ---- AnyMerged stays true after an erase; Clear resets everything ----
// The rebuild path asks "was any override ever discussed?", and a tie that resolved to the default is
// still a different starting point from a session where nobody mentioned one.
static void TestAnyMergedAndClear() {
    CvarLedger L;
    CHECK(!L.AnyMerged());
    L.Merge(1, 1, 10);
    L.Merge(1, 2, 10);             // erased again
    CHECK(L.Count() == 0);
    CHECK(L.AnyMerged());          // still true
    L.Clear();
    CHECK(!L.AnyMerged());
    CHECK(L.Count() == 0);
    CHECK(!L.Has(1));
    int Visits = 0;
    L.ForEach([&](uint8_t, const CvarEdit&) { ++Visits; });
    CHECK(Visits == 0);
}

// ---- Count never drifts under a long mixed stream ----
// Count is what the sync message's length prefix is built from, so a drifting count writes a header
// that disagrees with its own body.
static void TestCountTracksPresence() {
    CvarLedger L;
    for (int I = 0; I < 40; ++I) L.Merge(static_cast<uint8_t>(I), I, 1000);
    CHECK(L.Count() == 40);
    for (int I = 0; I < 40; I += 2) L.Merge(static_cast<uint8_t>(I), I + 1000, 1000);  // erase evens
    CHECK(L.Count() == 20);
    for (int I = 0; I < 40; I += 2) L.Merge(static_cast<uint8_t>(I), I, 1000);         // re-add them
    CHECK(L.Count() == 40);
    int Present = 0;
    L.ForEach([&](uint8_t, const CvarEdit&) { ++Present; });
    CHECK(Present == L.Count());   // the header and the body agree
}

int main() {
    TestEmpty();
    TestLaterEditWins();
    TestSameValueTieIsHarmless();
    TestConflictingTieErases();
    TestBothPeersReachTheSameAnswer();
    TestErasedIdCanBeSetAgain();
    TestThreeWayTieIsOrderDependent();
    TestIdsAreIndependentAcrossTheFullRange();
    TestForEachIsAscendingAndSkipsErased();
    TestAnyMergedAndClear();
    TestCountTracksPresence();
    if (GFailures == 0) std::printf("cvar_ledger_tests: ALL PASS\n");
    else std::printf("cvar_ledger_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
