// Tests for Lur::Net::BuildGate — the build-fingerprint refusal promoted out of Rps::LockstepPeer
// (#112, #164, #166, #201).
//
// All three rules came from the same symptom: THE TWO PHONES DISAGREEING about whether there was a
// mismatch at all. So the tests are written as two-sided scenarios wherever the rule is about symmetry,
// because a one-sided assertion cannot see the bug — that is precisely how it survived.
#include <cstdio>
#include <cstring>

#include "Lur/Net/BuildGate.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Net::BuildGate;

static const char* Fp1 = "9bf59f4c1a2b+Development";
static const char* Fp2 = "0123456789ab+Development";
static const char* Fp1Dirty = "9bf59f4c1a2b-dirty+Development";

static bool Hear(BuildGate& G, const char* Peer, const char* Local) {
    return G.OnPeerFingerprint(Peer, std::strlen(Peer), Local);
}

// ---- Matching builds are allowed; different ones are refused ----
static void TestBasicVerdict() {
    BuildGate G;
    CHECK(!G.HeardPeer());
    CHECK(!G.Mismatch());                       // nothing heard yet is NOT a mismatch
    CHECK(!Hear(G, Fp1, Fp1));
    CHECK(G.HeardPeer() && !G.Mismatch());
    CHECK(std::strcmp(G.PeerFingerprint(), Fp1) == 0);

    BuildGate H;
    CHECK(Hear(H, Fp2, Fp1));
    CHECK(H.Mismatch());
    // A dirty tree is a different build from the clean one at the same commit — a shared PREFIX must
    // not read as a match, or the one case where someone edited locally goes undetected.
    BuildGate D;
    CHECK(Hear(D, Fp1Dirty, Fp1));
    CHECK(D.Mismatch());
    BuildGate E;
    CHECK(Hear(E, Fp1, Fp1Dirty));
    CHECK(E.Mismatch());
    // A strict PREFIX must not read as a match either: length is part of the identity. A comparison
    // over min(len) would wave this through, and that is the shape a hand-written memcmp takes.
    BuildGate P;
    CHECK(P.OnPeerFingerprint("abc", 3, "abcdef"));
    CHECK(P.Mismatch());
    BuildGate Q;
    CHECK(Q.OnPeerFingerprint("abcdef", 6, "abc"));
    CHECK(Q.Mismatch());
}

// ---- RULE 1: the verdict is ASSIGNED, not latched ----
// A peer reinstalled from a matching commit left the other phone reporting a mismatch against a build
// that no longer existed. A diagnostic that stays lit after the fault is one people learn to ignore.
static void TestVerdictIsAssignedNotLatched() {
    BuildGate G;
    CHECK(Hear(G, Fp2, Fp1));
    CHECK(G.Mismatch());
    // The peer is rebuilt from our commit and reconnects. Its new fingerprint is positive evidence.
    CHECK(!Hear(G, Fp1, Fp1));
    CHECK(!G.Mismatch());                       // cleared ON EVIDENCE, not on an unrelated local event
    // And it can go bad again.
    CHECK(Hear(G, Fp2, Fp1));
    CHECK(G.Mismatch());
}

// ---- RULES 2+3: a fingerprint heard BEFORE local init is not thrown away ----
// THE headline test. The peer's fingerprint routinely arrives before this side initialises (the normal
// iOS ordering). Clearing at init discarded it, so which phone noticed depended on init order.
static void TestFingerprintHeardBeforeInitSurvives() {
    BuildGate G;
    Hear(G, Fp2, Fp1);                          // arrived early, while we were still starting up
    CHECK(G.Mismatch());
    G.Rederive(Fp1);                            // ...and now we init
    CHECK(G.Mismatch());                        // the evidence SURVIVED (a clear-on-init would blank it)
    CHECK(G.HeardPeer());
}

// ---- The SYMMETRY the bug broke: both phones must reach the same verdict ----
// This is the assertion that would have caught it. Peer A hears early, peer B hears late; after both
// have initialised and exchanged, the two verdicts must AGREE. A one-sided test cannot see this.
static void TestBothPeersAgreeRegardlessOfInitOrder() {
    // A: fingerprint first, then init.
    BuildGate A;
    Hear(A, Fp2, Fp1);
    A.Rederive(Fp1);
    // B: init first, then fingerprint.
    BuildGate B;
    B.Rederive(Fp2);
    Hear(B, Fp1, Fp2);
    CHECK(A.Mismatch() == B.Mismatch());
    CHECK(A.Mismatch());                        // and they agree on YES, since the builds do differ

    // Same again for a matching pair, which must agree on NO.
    BuildGate C, D;
    Hear(C, Fp1, Fp1);
    C.Rederive(Fp1);
    D.Rederive(Fp1);
    Hear(D, Fp1, Fp1);
    CHECK(C.Mismatch() == D.Mismatch());
    CHECK(!C.Mismatch());
}

// ---- RULE 3: re-deriving retires a STALE verdict when our own build changes ----
// We rebuilt; the peer is still on the old commit we used to share. Init must re-derive against our
// NEW fingerprint rather than keep the old (matching) answer.
static void TestRederiveAgainstOurNewBuild() {
    BuildGate G;
    CHECK(!Hear(G, Fp1, Fp1));
    CHECK(!G.Mismatch());
    G.Rederive(Fp2);                            // we rebuilt; the peer did not
    CHECK(G.Mismatch());
    G.Rederive(Fp1);                            // we went back
    CHECK(!G.Mismatch());
}

// ---- Nothing heard: re-deriving must NOT invent a mismatch ----
// A refusal on no evidence would block every first match.
static void TestRederiveWithNothingHeard() {
    BuildGate G;
    G.Rederive(Fp1);
    CHECK(!G.Mismatch());
    CHECK(!G.HeardPeer());
    G.Rederive(Fp2);
    CHECK(!G.Mismatch());
}

// ---- The log slot fires once per init, not once per pre-match tick ----
static void TestLogSlotIsOneShot() {
    BuildGate G;
    Hear(G, Fp2, Fp1);
    CHECK(G.ClaimLogSlot());
    for (int I = 0; I < 100; ++I) CHECK(!G.ClaimLogSlot());   // 100 pre-match ticks, one error line
    G.Rederive(Fp1);                                          // a fresh init re-arms it
    CHECK(G.ClaimLogSlot());
    CHECK(!G.ClaimLogSlot());
    Hear(G, Fp2, Fp1);                                        // so does a fresh fingerprint
    CHECK(G.ClaimLogSlot());
}

// ---- TRUNCATION can only ever cause a false MISMATCH, never a false match ----
// Refusing to play is the safe direction. An over-long fingerprint is remembered as truncated and never
// compares equal, so two unverifiable builds are never waved through.
static void TestTruncationFailsSafe() {
    char Long[BuildGate::MaxFingerprint + 40];
    for (std::size_t I = 0; I < sizeof(Long) - 1; ++I) Long[I] = 'a';
    Long[sizeof(Long) - 1] = '\0';

    BuildGate G;
    CHECK(G.OnPeerFingerprint(Long, std::strlen(Long), Long));   // IDENTICAL strings...
    CHECK(G.Mismatch());                                        // ...but refused, because we truncated
    CHECK(G.Truncated());
    // The stored copy is bounded and NUL-terminated, so a caller can still print it.
    CHECK(std::strlen(G.PeerFingerprint()) == BuildGate::MaxFingerprint);
    // Exactly at the cap is NOT truncated, and matches.
    char Exact[BuildGate::MaxFingerprint + 1];
    for (int I = 0; I < BuildGate::MaxFingerprint; ++I) Exact[I] = 'b';
    Exact[BuildGate::MaxFingerprint] = '\0';
    BuildGate H;
    CHECK(!H.OnPeerFingerprint(Exact, BuildGate::MaxFingerprint, Exact));
    CHECK(!H.Mismatch() && !H.Truncated());
}

// ---- Degenerate inputs are refusals, not crashes ----
// This reads bytes off the wire, so an empty or null payload must be total.
static void TestDegenerateInput() {
    BuildGate G;
    CHECK(G.OnPeerFingerprint(nullptr, 0, Fp1));       // no fingerprint != our real one -> refuse
    CHECK(G.Mismatch());
    CHECK(G.PeerFingerprint()[0] == '\0');
    BuildGate H;
    CHECK(H.OnPeerFingerprint("", 0, Fp1));
    CHECK(H.Mismatch());
    // A null LOCAL fingerprint cannot be verified, so it refuses rather than comparing against nothing.
    BuildGate I;
    CHECK(Hear(I, Fp1, nullptr));
    CHECK(I.Mismatch());
    // An empty local and an empty peer are equal, and BuildFingerprint() never returns empty anyway.
    BuildGate J;
    CHECK(!J.OnPeerFingerprint("", 0, ""));
}

// ---- Forget clears the heard fingerprint too (unlike init) ----
static void TestForget() {
    BuildGate G;
    Hear(G, Fp2, Fp1);
    CHECK(G.Mismatch() && G.HeardPeer());
    G.Forget();
    CHECK(!G.Mismatch() && !G.HeardPeer());
    CHECK(G.PeerFingerprint()[0] == '\0');
    G.Rederive(Fp1);
    CHECK(!G.Mismatch());     // and re-deriving after Forget has no evidence to derive FROM
}

int main() {
    TestBasicVerdict();
    TestVerdictIsAssignedNotLatched();
    TestFingerprintHeardBeforeInitSurvives();
    TestBothPeersAgreeRegardlessOfInitOrder();
    TestRederiveAgainstOurNewBuild();
    TestRederiveWithNothingHeard();
    TestLogSlotIsOneShot();
    TestTruncationFailsSafe();
    TestDegenerateInput();
    TestForget();
    if (GFailures == 0) std::printf("build_gate_tests: ALL PASS\n");
    else std::printf("build_gate_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
