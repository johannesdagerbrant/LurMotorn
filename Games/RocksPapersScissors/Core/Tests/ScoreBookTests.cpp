// Host tests for Rps::ScoreBook — the persistent W-L-D record behind the opponent dropdown.
//
// These go THROUGH THE REAL Lur::Save::Store into a temp directory, not just through Write/Read.
// A byte-codec test would have passed with the store never wired up at all, which is exactly the
// #147 failure mode: green tests around a feature that was dead in the shipping composition. What
// the player experiences is "close the app, reopen it, is my ladder still there", so the test
// closes and reopens the book against the same directory.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Rps/ScoreBook.h"
#include "Rps/Sim.h"

using namespace Rps;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// Two well-formed device ids in a known order, so "who is lower" is not a guess.
static const std::string LowId  = "00112233445566778899aabbccddeeff";
static const std::string HighId = "ff112233445566778899aabbccddeeff";

static std::string TempDir(const char* Name) {
    std::filesystem::path P = std::filesystem::temp_directory_path() / "lur-rps-scorebook" / Name;
    std::error_code Ec;
    std::filesystem::remove_all(P, Ec);       // a stale run must not seed this one
    return P.string();
}

// ---- The AI tallies: the thing the player actually notices across a relaunch ----
static void TestAiTalliesSurviveAReopen() {
    const std::string Dir = TempDir("ai");
    {
        Lur::Save::Store S(Dir);
        ScoreBook B;
        CHECK(B.Load(S));                              // absent record = fresh, not a failure
        CHECK(B.Ai(static_cast<int>(EAiTier::Hard)).Total() == 0);
        // The human is team 0 in a solo match and the AI is team 1 (every main wires it that way).
        B.RecordAi(static_cast<int>(EAiTier::Hard), ResultTeam0Wins, /*MyTeam*/ 0);
        B.RecordAi(static_cast<int>(EAiTier::Hard), ResultTeam1Wins, 0);
        B.RecordAi(static_cast<int>(EAiTier::Hard), ResultTeam1Wins, 0);
        B.RecordAi(static_cast<int>(EAiTier::Hard), ResultDraw, 0);
        B.RecordAi(static_cast<int>(EAiTier::Easy), ResultTeam0Wins, 0);
        CHECK(B.Save(S));
    }
    {
        Lur::Save::Store S(Dir);                       // a fresh store + book: the next launch
        ScoreBook B;
        CHECK(B.Load(S));
        const Tally H = B.Ai(static_cast<int>(EAiTier::Hard));
        CHECK(H.Wins == 1 && H.Losses == 2 && H.Draws == 1);
        const Tally E = B.Ai(static_cast<int>(EAiTier::Easy));
        CHECK(E.Wins == 1 && E.Losses == 0 && E.Draws == 0);
        // Untouched tiers stay at zero — a per-tier record, not one pooled score.
        CHECK(B.Ai(static_cast<int>(EAiTier::Medium)).Total() == 0);
    }
}

// ---- The peer tallies: stored GUID-anchored, read back per-device ----
// The point of the anchoring is that ONE row serves both phones, so the same bytes must read as
// 3-1 on the winner's phone and 1-3 on the loser's. That is what a future link-time merge relies
// on, and it is invisible unless a test reads one record from both sides.
static void TestPeerTallyReadsOppositeOnEachDevice() {
    const std::string Dir = TempDir("peer");
    Lur::Save::Store S(Dir);
    ScoreBook B;
    CHECK(B.Load(S));
    // Play as the LOWER-guid device (team 0 by the mains' convention) and win three, lose one.
    CHECK(B.RecordPeer(HighId, LowId, ResultTeam0Wins, /*MyTeam*/ 0));
    CHECK(B.RecordPeer(HighId, LowId, ResultTeam0Wins, 0));
    CHECK(B.RecordPeer(HighId, LowId, ResultTeam0Wins, 0));
    CHECK(B.RecordPeer(HighId, LowId, ResultTeam1Wins, 0));
    const Tally Mine = B.Peer(HighId, LowId);
    CHECK(Mine.Wins == 3 && Mine.Losses == 1 && Mine.Draws == 0);
    // The SAME record read from the other phone's point of view.
    const Tally Theirs = B.Peer(LowId, HighId);
    CHECK(Theirs.Wins == 1 && Theirs.Losses == 3 && Theirs.Draws == 0);
    CHECK(B.Save(S));

    ScoreBook Reloaded;
    CHECK(Reloaded.Load(S));
    const Tally After = Reloaded.Peer(HighId, LowId);
    CHECK(After.Wins == 3 && After.Losses == 1);
    // An opponent never played is 0-0-0, not a phantom row.
    CHECK(Reloaded.Peer("0f112233445566778899aabbccddeeff", LowId).Total() == 0);
}

// A win recorded from the HIGHER device must land on the same row's other side — i.e. the sim's
// team number is NOT what is stored. Recording the same match from both phones (as really happens,
// each on its own device) must produce mirror-image reads of identical bytes.
static void TestPeerTallyIsTeamNumberAgnostic() {
    ScoreBook A, B;
    // Device LOW plays team 0 and wins.
    CHECK(A.RecordPeer(HighId, LowId, ResultTeam0Wins, /*MyTeam*/ 0));
    // Device HIGH plays team 1 and therefore LOST the same match.
    CHECK(B.RecordPeer(LowId, HighId, ResultTeam0Wins, /*MyTeam*/ 1));
    std::vector<uint8_t> Ba, Bb;
    A.Write(Ba);
    B.Write(Bb);
    // Byte-identical: this is the property a future link-time merge (#15-20) is built on.
    CHECK(Ba == Bb);
    CHECK(A.Peer(HighId, LowId).Wins == 1);
    CHECK(B.Peer(LowId, HighId).Losses == 1);
}

static void TestJunkIdsAndSelfPlayAreRefused() {
    ScoreBook B;
    CHECK(!B.RecordPeer("", LowId, ResultTeam0Wins, 0));             // no id yet (pre-handshake)
    CHECK(!B.RecordPeer("nothex", LowId, ResultTeam0Wins, 0));
    CHECK(!B.RecordPeer(HighId, "short", ResultTeam0Wins, 0));
    CHECK(!B.RecordPeer(LowId, LowId, ResultTeam0Wins, 0));          // loopback / same device
    std::vector<uint8_t> Bytes;
    B.Write(Bytes);
    ScoreBook R;
    CHECK(R.Read(Bytes.data(), Bytes.size()));
    CHECK(R.Peer(HighId, LowId).Total() == 0);                       // no junk row was created
}

// A corrupt or truncated score file must never stop the game from starting.
static void TestCorruptRecordFailsSafe() {
    const uint8_t Junk[] = {'X', 'Y', 'Z', 9, 9, 9};
    ScoreBook B;
    B.RecordAi(static_cast<int>(EAiTier::Easy), ResultTeam0Wins, 0);
    CHECK(!B.Read(Junk, sizeof(Junk)));                  // reported, not thrown
    CHECK(B.Ai(static_cast<int>(EAiTier::Easy)).Total() == 0);   // and reset to fresh defaults
    CHECK(B.Read(nullptr, 0));                           // absent == fresh, still fine
    // Truncated mid-record (header + a partial tally).
    const uint8_t Cut[] = {'R', 'S', 'B', 1, static_cast<uint8_t>(AiTierCount), 1, 0};
    CHECK(!B.Read(Cut, sizeof(Cut)));
}

// The record must round-trip its own capacity, and refuse the row past it rather than evicting a
// rivalry someone still cares about.
static void TestPeerCapIsRefusedNotEvicted() {
    ScoreBook B;
    char Id[Lur::Save::DeviceIdHexLen + 1] = "00000000000000000000000000000000";
    for (int I = 0; I < ScoreBook::MaxPeers; ++I) {
        std::snprintf(Id, sizeof(Id), "%032x", I + 1);
        CHECK(B.RecordPeer(Id, LowId, ResultTeam0Wins, 0));
    }
    std::snprintf(Id, sizeof(Id), "%032x", ScoreBook::MaxPeers + 1);
    CHECK(!B.RecordPeer(Id, LowId, ResultTeam0Wins, 0));   // full: refused
    std::snprintf(Id, sizeof(Id), "%032x", 1);
    CHECK(B.Peer(Id, LowId).Wins == 1);                    // the first rivalry is still there
    std::vector<uint8_t> Bytes;
    B.Write(Bytes);
    ScoreBook R;
    CHECK(R.Read(Bytes.data(), Bytes.size()));
    CHECK(R.Peer(Id, LowId).Wins == 1);
}

int main() {
    TestAiTalliesSurviveAReopen();
    TestPeerTallyReadsOppositeOnEachDevice();
    TestPeerTallyIsTeamNumberAgnostic();
    TestJunkIdsAndSelfPlayAreRefused();
    TestCorruptRecordFailsSafe();
    TestPeerCapIsRefusedNotEvicted();
    if (GFailures == 0) std::printf("rps score-book tests: all passed\n");
    else std::printf("rps score-book tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
