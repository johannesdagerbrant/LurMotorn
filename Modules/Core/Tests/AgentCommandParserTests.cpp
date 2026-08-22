// Tests for Lur::AgentCommandParser — the assistant remote-control command line promoted out of
// Rps::AgentControl (#201).
//
// Two properties carry the whole design and both have cost real time on hardware:
//
//   * IDEMPOTENCE via a strictly-increasing sequence number. Both delivery channels are
//     level-triggered and POLLED, so a command with no identity is re-applied forever. The failure is
//     not subtle when it happens (a thousand placements) but it IS silent in the other direction: a
//     reused seq is a no-op that presents as the channel being dead.
//   * TOTALITY on hostile input. This reads text from outside the process, so no input may trap, read
//     past the terminator, or wrap an integer.
//
// The parser is compiled in every config precisely so these live in the ordinary host suite rather
// than only in a build nobody runs.
#include <cstdio>
#include <cstring>

#include "Lur/Core/AgentCommandParser.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::AgentCommandLine;
using Lur::AgentCommandParser;
using Lur::AgentVerb;

namespace {
// A stand-in verb table. "place" and "placement" share a prefix on purpose — whole-token matching is
// what stops a typo running a different command.
enum { VPlace = 10, VQueue = 11, VLinked = 12, VPlacement = 13 };
constexpr AgentVerb Verbs[] = {
    {"place", VPlace}, {"queue", VQueue}, {"linked", VLinked}, {"placement", VPlacement},
};
constexpr int NVerbs = static_cast<int>(sizeof(Verbs) / sizeof(Verbs[0]));

bool Poll(AgentCommandParser& P, const char* Text, AgentCommandLine& Out) {
    return P.Poll(Text, Verbs, NVerbs, Out);
}
}  // namespace

// ---- The basic line, with all three optional arguments ----
static void TestParsesVerbAndArgs() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(Poll(P, "7 place 17 16 2", C));
    CHECK(C.Verb == VPlace);
    CHECK(C.A == 17 && C.B == 16 && C.C == 2);
    CHECK(C.Seq == 7);
    CHECK(P.LastSeq() == 7);
}

// ---- Absent arguments leave the caller's defaults alone ----
static void TestMissingArgsKeepDefaults() {
    AgentCommandParser P;
    AgentCommandLine C;
    C.A = C.B = C.C = -99;      // Poll fills a FRESH command, so these must come back as 0, not -99
    CHECK(Poll(P, "1 linked", C));
    CHECK(C.Verb == VLinked);
    CHECK(C.A == 0 && C.B == 0 && C.C == 0);

    // Partial arguments: B and C stay at the struct default.
    AgentCommandLine D;
    CHECK(Poll(P, "2 queue 4", D));
    CHECK(D.A == 4 && D.B == 0 && D.C == 0);
}

// ---- IDEMPOTENCE: re-reading the same text does nothing ----
// The headline test. A level-triggered channel holds its text until something changes it, and the app
// polls it repeatedly.
static void TestSameSequenceIsIgnoredOnReRead() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(Poll(P, "5 place 1 2 3", C));
    for (int I = 0; I < 100; ++I) CHECK(!Poll(P, "5 place 1 2 3", C));   // a hundred polls, one effect
    CHECK(P.LastSeq() == 5);
    // A LOWER seq is also refused — a stale channel value must not replay an old command.
    CHECK(!Poll(P, "4 queue 9", C));
    CHECK(!Poll(P, "0 queue 9", C));
    // Only strictly greater is accepted.
    CHECK(Poll(P, "6 queue 9", C));
    CHECK(C.Verb == VQueue && C.A == 9);
}

// ---- An unknown verb does NOT consume the sequence number ----
// So a typo can be fixed in place. Without this, the author must also remember to bump the seq, and
// the symptom of forgetting is a channel that appears dead.
static void TestUnknownVerbDoesNotConsumeTheSequence() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(!Poll(P, "9 plcae 1 2", C));
    CHECK(P.LastSeq() == 0);
    CHECK(Poll(P, "9 place 1 2", C));    // same seq, corrected verb -> accepted
    CHECK(C.Verb == VPlace && C.A == 1 && C.B == 2);
}

// ---- Whole-token matching: a prefix must not match ----
// "place" and "placement" are different commands. A prefix match would run the wrong one, and with a
// harness driving two phones the wrong command is worse than none.
static void TestVerbMatchIsWholeToken() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(Poll(P, "1 placement 5", C));
    CHECK(C.Verb == VPlacement);          // NOT VPlace, even though "place" is listed first
    CHECK(Poll(P, "2 place 5", C));
    CHECK(C.Verb == VPlace);
    // A verb with trailing junk attached is not that verb.
    CHECK(!Poll(P, "3 placex 5", C));
    CHECK(P.LastSeq() == 2);
}

// ---- Negative arguments, and whitespace of every flavour ----
static void TestSignsAndWhitespace() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(Poll(P, "  1\tqueue\t-500  \r\n", C));
    CHECK(C.Verb == VQueue && C.A == -500);
    CHECK(Poll(P, "\n2   place   -1   -2   -3\n", C));
    CHECK(C.A == -1 && C.B == -2 && C.C == -3);
}

// ---- Integers CLAMP, they never wrap ----
// A wrapped coordinate is a placement somewhere unrelated; a clamped one is obviously wrong.
static void TestOversizedIntegersClamp() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(Poll(P, "1 place 99999999999999999999 -99999999999999999999", C));
    CHECK(C.A == 0x7FFFFFFF);
    CHECK(C.B == -0x7FFFFFFF);
    // An absurd SEQ is rejected outright rather than wrapped — accepting it would poison the gate for
    // the rest of the session, since nothing could ever exceed it.
    CHECK(!Poll(P, "99999999999999999999 place 1", C));
    CHECK(P.LastSeq() == 1);
}

// ---- Total on garbage: never traps, never reads past the terminator ----
static void TestHostileInputIsRejectedNotFatal() {
    AgentCommandParser P;
    AgentCommandLine C;
    const char* Bad[] = {
        "", " ", "\n", "place", "place 1 2", "-1 place", "abc", "1", "1 ", "1  ",
        "1 -", "x1 place", "99 ", "0 place",
    };
    for (const char* T : Bad) CHECK(!Poll(P, T, C));
    CHECK(!Poll(P, nullptr, C));
    CHECK(P.LastSeq() == 0);             // nothing above was accepted
    CHECK(Poll(P, "1 place", C));

    // No whitespace is REQUIRED between the seq and the verb: "2place" is seq 2 verb place. Recorded
    // here as the actual behaviour rather than left to be discovered — the digits and the verb cannot
    // be confused for each other, so rejecting it would only make a hand-edited channel fussier.
    CHECK(Poll(P, "2place", C));
    CHECK(C.Verb == VPlace && C.Seq == 2);

    // A DANGLING SIGN is a MISSING argument, not a parse failure: the verb still runs with its
    // defaults. That is the right call for a channel a human hand-edits — the alternative is a
    // command that silently does nothing because of a stray character.
    CHECK(Poll(P, "3 place -", C));
    CHECK(C.Verb == VPlace && C.A == 0 && C.B == 0 && C.C == 0);
    CHECK(Poll(P, "4 place - - -", C));
    CHECK(C.A == 0 && C.B == 0 && C.C == 0);
    // But the sign is CONSUMED, so a later number SHIFTS INTO THE NEXT SLOT: "5 - 7" gives A=5, B=0,
    // C=7, not A=5, B=-7. Worth pinning: an off-by-one-argument command is exactly the kind of thing
    // that reads as the harness being ignored rather than as a typo.
    CHECK(Poll(P, "5 place 5 - 7", C));
    CHECK(C.A == 5 && C.B == 0 && C.C == 7);
}

// ---- An empty or null verb table accepts nothing (and does not read it) ----
static void TestEmptyVerbTable() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(!P.Poll("1 place 1", nullptr, 3, C));
    CHECK(!P.Poll("1 place 1", Verbs, 0, C));
    CHECK(!P.Poll("1 place 1", Verbs, -5, C));
    CHECK(P.LastSeq() == 0);
    // A table with a null NAME row is skipped, not dereferenced.
    const AgentVerb Holey[] = {{nullptr, 1}, {"place", VPlace}};
    CHECK(P.Poll("1 place 3", Holey, 2, C));
    CHECK(C.Verb == VPlace && C.A == 3);
}

// ---- Trailing junk after the arguments is ignored, not fatal ----
// A channel may hold a trailing newline, a comment, or the remains of a longer previous command.
static void TestTrailingJunkIsIgnored() {
    AgentCommandParser P;
    AgentCommandLine C;
    CHECK(Poll(P, "1 place 1 2 3 and then some words", C));
    CHECK(C.A == 1 && C.B == 2 && C.C == 3);
}

int main() {
    TestParsesVerbAndArgs();
    TestMissingArgsKeepDefaults();
    TestSameSequenceIsIgnoredOnReRead();
    TestUnknownVerbDoesNotConsumeTheSequence();
    TestVerbMatchIsWholeToken();
    TestSignsAndWhitespace();
    TestOversizedIntegersClamp();
    TestHostileInputIsRejectedNotFatal();
    TestEmptyVerbTable();
    TestTrailingJunkIsIgnored();
    if (GFailures == 0) std::printf("agent_parser_tests: ALL PASS\n");
    else std::printf("agent_parser_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
