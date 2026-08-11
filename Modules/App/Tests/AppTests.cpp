// Host unit tests for Lur::App::GameHost (issue #43) — the ready/resync/adopt/record-sync
// choreography that both chess mains had duplicated.
//
// The point of these tests is that the flow HAD NO TEST AT ALL while it lived in two platform
// mains: nothing on the host could reach it, which is precisely how the two copies drifted (stack
// vs heap ownership, different wiring order, and a MATCH END line whose format differed per phone).
// Moving it into a module is what makes the three cases below expressible.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Lur/App/GameHost.h"
#include "Lur/Transport/Loopback.h"

// Each host needs its OWN save dir, because the device GUID is persisted per directory: point two
// hosts at one dir and they load the SAME id, which makes a peer indistinguishable from itself and
// quietly invalidates every assertion below. (Distinct dirs also mean Persist() really writes, so
// the disk half of the flow is exercised rather than failing silently into a missing directory.)
static const char* DirA() {
    static const std::string D = (std::filesystem::temp_directory_path() / "lur_app_tests_a").string();
    std::filesystem::create_directories(D);
    return D.c_str();
}
static const char* DirB() {
    static const std::string D = (std::filesystem::temp_directory_path() / "lur_app_tests_b").string();
    std::filesystem::create_directories(D);
    return D.c_str();
}

static int Failures = 0;
#define CHECK(Cond)                                                                    \
    do {                                                                               \
        if (!(Cond)) {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);                \
            ++Failures;                                                                \
        }                                                                              \
    } while (false)

// A minimal ISaveState: one monotonic counter, so "newer" is unambiguous and MergeIfNewer is
// trivially checkable. Stands in for ChessRecord/ScoreBook without dragging a game in — the module
// wall means this suite links no game at all.
class CounterState : public Lur::Save::ISaveState {
public:
    uint32_t Value = 0;
    int      Merges = 0;

    void Write(std::vector<uint8_t>& Out) const override {
        Out.push_back(static_cast<uint8_t>(Value & 0xFF));
        Out.push_back(static_cast<uint8_t>((Value >> 8) & 0xFF));
    }
    void Read(const uint8_t* Data, std::size_t Size) override {
        Value = Size >= 2 ? static_cast<uint32_t>(Data[0] | (Data[1] << 8)) : 0;
    }
    bool MergeIfNewer(const uint8_t* Data, std::size_t Size) override {
        if (Size < 2) return false;
        const uint32_t Theirs = static_cast<uint32_t>(Data[0] | (Data[1] << 8));
        ++Merges;
        if (Theirs <= Value) return false;
        Value = Theirs;
        return true;
    }
};

// Drive two hosts over a deferred loopback pair until both report ready, or give up. Deferred
// delivery is what the phones do (#40), and it is required here: a Sync sent from inside a ready
// handler would otherwise recurse straight back into the peer.
struct Pair {
    Lur::Transport::LoopbackTransport Ta, Tb;
    Lur::App::GameHost HostA, HostB;
    CounterState StateA, StateB;

    void Pump(int Ticks) {
        const uint64_t OneTickNs = 16'000'000ull;
        for (int I = 0; I < Ticks; ++I) {
            HostA.Tick(OneTickNs);
            HostB.Tick(OneTickNs);
            HostA.PumpInbox();
            HostB.PumpInbox();
        }
    }
};

// ---- 1. Initial link: the peer is adopted, so our record goes out and theirs lands ----------
static void TestInitialLinkAdoptsAndSyncs() {
    Pair P;
    P.Ta.SetDeferred(true);
    P.Tb.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(P.Ta, P.Tb);

    P.StateA.Value = 7;    // A holds the newer record
    P.StateB.Value = 3;

    int AdoptedA = 0, AdoptedB = 0;
    Lur::App::GameHost::Hooks Ha, Hb;
    Ha.OnPeerAdopted    = [&](const std::string&) { ++AdoptedA; return true; };
    Ha.IsActiveOpponent = [](const std::string&) { return true; };
    Hb.OnPeerAdopted    = [&](const std::string&) { ++AdoptedB; return true; };
    Hb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca, P.StateA);  P.HostA.Start(Ha);
    P.HostB.Init(Cb, P.StateB);  P.HostB.Start(Hb);

    P.Pump(40);

    CHECK(P.HostA.Session().IsReady());
    CHECK(P.HostB.Session().IsReady());
    CHECK(AdoptedA >= 1);              // both sides ran their adopt rule
    CHECK(AdoptedB >= 1);
    // B adopts A's strictly-newer record; A keeps its own (B's is older, so MergeIfNewer refuses).
    CHECK(P.StateB.Value == 7);
    CHECK(P.StateA.Value == 7);
}

// ---- 2. A NON-adopting peer sends nothing ---------------------------------------------------
// The #38 hijack rule: we are already playing someone else, so this peer must not receive our
// record — and must not be able to push its own into ours either.
static void TestNonAdoptSendsNothing() {
    Pair P;
    P.Ta.SetDeferred(true);
    P.Tb.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(P.Ta, P.Tb);

    P.StateA.Value = 9;
    P.StateB.Value = 1;

    Lur::App::GameHost::Hooks Ha, Hb;
    Ha.OnPeerAdopted    = [](const std::string&) { return false; };   // A refuses this peer
    Ha.IsActiveOpponent = [](const std::string&) { return false; };
    Hb.OnPeerAdopted    = [](const std::string&) { return true; };
    Hb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca, P.StateA);  P.HostA.Start(Ha);
    P.HostB.Init(Cb, P.StateB);  P.HostB.Start(Hb);

    P.Pump(40);

    // The link still forms — refusing to adopt is a record-sharing decision, not a transport one.
    CHECK(P.HostA.Session().IsReady());
    // A never sent its record, so B's counter is untouched by A.
    CHECK(P.StateB.Value == 1);
    // And B's record never entered A's, because A's IsActiveOpponent gate refused it.
    CHECK(P.StateA.Value == 9);
    CHECK(P.StateA.Merges == 0);
}

// ---- 3. A reconnect RE-adopts and re-syncs --------------------------------------------------
// The bug this guards: the two mains wired the same lambda into ReadyHandler AND ResyncHandler, so
// nothing structural stopped a later edit from fixing only one. A peer that rejoins must be adopted
// by the same rule as one arriving first — the host now routes both through one function.
static void TestReconnectReAdoptsAndReSyncs() {
    Pair P;
    P.Ta.SetDeferred(true);
    P.Tb.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(P.Ta, P.Tb);

    int AdoptedA = 0;
    Lur::App::GameHost::Hooks Ha, Hb;
    Ha.OnPeerAdopted    = [&](const std::string&) { ++AdoptedA; return true; };
    Ha.IsActiveOpponent = [](const std::string&) { return true; };
    Hb.OnPeerAdopted    = [](const std::string&) { return true; };
    Hb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca, P.StateA);  P.HostA.Start(Ha);
    P.HostB.Init(Cb, P.StateB);  P.HostB.Start(Hb);
    P.Pump(40);
    CHECK(P.HostA.Session().IsReady());
    const int AfterFirstLink = AdoptedA;
    CHECK(AfterFirstLink >= 1);

    // A's record advances while linked, then the peer rejoins: the resync path must adopt again and
    // hand over the CURRENT record, not the one from first contact.
    P.StateA.Value = 42;
    P.HostA.Session().RequestResync();
    P.Pump(40);

    CHECK(AdoptedA > AfterFirstLink);   // the resync route ran the adopt rule again
    CHECK(P.StateB.Value == 42);        // ...and the newer record actually crossed
}

// ---- 4. Match end persists and reports through ONE log seam ---------------------------------
// The two copies printed the same numbers with different specifiers, and only one labelled the
// tally. Whatever the format is, it must now come from a single place.
static void TestMatchEndPersistsAndReportsOnce() {
    Lur::Transport::LoopbackTransport T;
    CounterState State;
    State.Value = 5;

    std::vector<std::string> Lines;
    Lur::App::GameHost::Config C;
    C.SaveDir = DirA();
    C.Transport = &T;
    C.Log = [&](const char* M) { Lines.emplace_back(M); };

    Lur::App::GameHost::Hooks H;
    H.OnPeerAdopted    = [](const std::string&) { return true; };
    H.IsActiveOpponent = [](const std::string&) { return true; };
    H.Summarize = [] {
        Lur::App::GameHost::Hooks::MatchSummary S;
        S.Result = 2; S.WinsLower = 3; S.WinsHigher = 1; S.Draws = 4; S.Total = 8;
        return S;
    };

    Lur::App::GameHost Host;
    Host.Init(C, State);
    Host.Start(H);
    const std::size_t Before = Lines.size();
    Host.OnMatchEnded();

    CHECK(Lines.size() == Before + 1);   // exactly one line, from one place
    bool Found = false;
    for (const std::string& L : Lines)
        if (L.find("MATCH END") != std::string::npos && L.find("WLD(lo/hi/dr)=3/1/4") != std::string::npos
            && L.find("total=8") != std::string::npos)
            Found = true;
    CHECK(Found);

    // A host with no Summarize hook still persists and simply says nothing extra — the summary is
    // optional because it is the one part of this flow that is genuinely game-shaped.
    std::vector<std::string> Quiet;
    Lur::App::GameHost::Config C2 = C;
    C2.Log = [&](const char* M) { Quiet.emplace_back(M); };
    Lur::App::GameHost::Hooks H2 = H;
    H2.Summarize = nullptr;
    CounterState State2;
    Lur::App::GameHost Host2;
    Host2.Init(C2, State2);
    Host2.Start(H2);
    const std::size_t QBefore = Quiet.size();
    Host2.OnMatchEnded();
    CHECK(Quiet.size() == QBefore);
}

int main() {
    TestInitialLinkAdoptsAndSyncs();
    TestNonAdoptSendsNothing();
    TestReconnectReAdoptsAndReSyncs();
    TestMatchEndPersistsAndReportsOnce();
    if (Failures == 0) std::printf("app_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
