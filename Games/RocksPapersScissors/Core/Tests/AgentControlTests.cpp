// Host tests for the agent remote-control command grammar (CLAUDE.md's LUR_AGENT axis).
//
// The parser is compiled in every build precisely so it can be tested here — it turns text into a
// struct and cannot drive anything; the channel that reads it and every effect it applies are
// #if LUR_AGENT. The properties below are the ones that make a POLLED channel safe, and they are worth
// pinning because getting them wrong is destructive on a live phone rather than merely wrong: a
// command with no identity is re-applied on every poll, so "place a camp" becomes a thousand places.
#include <cstdio>
#include <string>

#include "Rps/AgentControl.h"

using namespace Rps;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

static void TestParsesEachVerb() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("1 place 17 16 0", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Place && Cmd.A == 17 && Cmd.B == 16 && Cmd.C == 0);
    CHECK(C.Poll("2 queue 2 5", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Queue && Cmd.A == 2 && Cmd.B == 5);
    CHECK(C.Poll("3 stress 800 0", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Stress && Cmd.A == 800 && Cmd.B == 0);
    CHECK(C.Poll("4 corrupt 999", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Corrupt && Cmd.A == 999);
    CHECK(C.Poll("5 droptx 1", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::DropTx && Cmd.A == 1);
    CHECK(C.Poll("6 console 1", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Console && Cmd.A == 1);
    CHECK(C.Poll("7 gesture", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Gesture);
    CHECK(C.Poll("8 killown 0", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::KillOwn && Cmd.A == 0);
    CHECK(C.Poll("9 linked", Cmd));                       // #170
    CHECK(Cmd.Kind == EAgentCmd::Linked);
}

// #170: `linked` takes no arguments, and it must not be confused with a prefix of anything else.
// Worth pinning because Match() is a hand-rolled word compare and the verb list is scanned in
// order — a verb that accepted a prefix would swallow commands meant for another one, silently,
// on a channel whose whole point is that nobody is watching it.
static void TestLinkedVerbIsExactAndArgumentless() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("1 linked", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Linked && Cmd.A == 0 && Cmd.B == 0 && Cmd.C == 0);
    CHECK(!C.Poll("2 link", Cmd));                        // a PREFIX is not the verb
    CHECK(!C.Poll("3 linkedx", Cmd));                     // nor is a longer word
    CHECK(C.Poll("4 linked  77", Cmd));                   // stray args are parsed, then ignored by the mains
    CHECK(Cmd.Kind == EAgentCmd::Linked && Cmd.A == 77);
    // A rejected verb must NOT consume the sequence number, so fixing the typo in place still works
    // — the channel is level-triggered and the operator only gets to rewrite the same file/property.
    CHECK(!C.Poll("5 linkd", Cmd));
    CHECK(C.Poll("5 linked", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Linked);
}

// THE property that makes a level-triggered channel usable. A system property and a file both HOLD
// their text; without identity every poll would re-fire the command.
static void TestSameCommandIsAppliedExactlyOnce() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("1 place 17 16 0", Cmd));
    for (int I = 0; I < 100; ++I) CHECK(!C.Poll("1 place 17 16 0", Cmd));   // re-read is inert
    CHECK(C.LastSeq() == 1);
    CHECK(C.Poll("2 place 17 16 0", Cmd));                                 // a NEW seq fires again
    CHECK(Cmd.Kind == EAgentCmd::Place);
}

// A seq that goes backwards (a channel reset, or a stale value racing a new one) must not re-fire
// history — replaying old commands on a live match is worse than dropping a new one.
static void TestOlderSequencesAreIgnored() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("50 corrupt 1", Cmd));
    CHECK(!C.Poll("49 corrupt 1", Cmd));
    CHECK(!C.Poll("1 corrupt 1", Cmd));
    CHECK(!C.Poll("50 corrupt 1", Cmd));      // equal is not greater
    CHECK(C.Poll("51 corrupt 1", Cmd));
}

// Absent arguments keep their defaults rather than reading adjacent memory or garbage.
static void TestMissingArgumentsDefaultToZero() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("1 gesture", Cmd));
    CHECK(Cmd.A == 0 && Cmd.B == 0 && Cmd.C == 0);
    CHECK(C.Poll("2 queue 3", Cmd));
    CHECK(Cmd.A == 3 && Cmd.B == 0 && Cmd.C == 0);
}

// Total on hostile/garbage input: this text arrives from outside the process (a system property anyone
// with adb can set), so it must never trap, never run past the terminator, and never half-apply.
static void TestTotalOnGarbage() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(!C.Poll("", Cmd));
    CHECK(!C.Poll(nullptr, Cmd));
    CHECK(!C.Poll("   ", Cmd));
    CHECK(!C.Poll("place 17 16", Cmd));            // no seq
    CHECK(!C.Poll("1 nonsense 1 2 3", Cmd));       // unknown verb
    CHECK(!C.Poll("1 pl", Cmd));                   // truncated verb
    CHECK(!C.Poll("1 placex 1", Cmd));             // verb must end at a boundary
    CHECK(!C.Poll("99999999999999999999 place", Cmd));  // seq overflow rejected, not wrapped
    CHECK(!C.Poll("-1 place", Cmd));               // negative seq is not a seq
    CHECK(C.LastSeq() == 0);                       // NOTHING above consumed the sequence
    // An unknown verb must not burn the seq, so correcting a typo at the same number still works.
    CHECK(!C.Poll("1 plaice 5", Cmd));
    CHECK(C.Poll("1 place 5", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Place && Cmd.A == 5);
}

// Whitespace and negatives, because the channel is hand-typed as often as scripted.
static void TestWhitespaceAndSigns() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("  10\tcorrupt   -500  ", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Corrupt && Cmd.A == -500);
    CHECK(C.Poll("11 place 17 16 0\n", Cmd));
    CHECK(Cmd.Kind == EAgentCmd::Place && Cmd.C == 0);
}

// A huge argument clamps instead of wrapping: a wrapped negative unit count would be a very confusing
// way to discover an off-by-one in a scenario script.
static void TestOversizedArgumentsClamp() {
    AgentControl C;
    AgentCommand Cmd;
    CHECK(C.Poll("1 stress 99999999999999", Cmd));
    CHECK(Cmd.A == 0x7FFFFFFF);
    CHECK(C.Poll("2 stress 12345", Cmd));   // and the parser is not left mid-number
    CHECK(Cmd.A == 12345);
}

int main() {
    TestParsesEachVerb();
    TestLinkedVerbIsExactAndArgumentless();   // #170
    TestSameCommandIsAppliedExactlyOnce();
    TestOlderSequencesAreIgnored();
    TestMissingArgumentsDefaultToZero();
    TestTotalOnGarbage();
    TestWhitespaceAndSigns();
    TestOversizedArgumentsClamp();

    if (GFailures == 0) std::printf("rps_agent_tests: ALL PASS\n");
    else std::printf("rps_agent_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
