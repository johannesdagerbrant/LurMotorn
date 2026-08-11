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
    Lur::App::GameHost::RecordSync Ra, Rb;
    Ra.OnPeerAdopted    = [&](const std::string&) { ++AdoptedA; return true; };
    Ra.IsActiveOpponent = [](const std::string&) { return true; };
    Rb.OnPeerAdopted    = [&](const std::string&) { ++AdoptedB; return true; };
    Rb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca);  P.HostA.EnableRecordSync(P.StateA, Ra);  P.HostA.Start({});
    P.HostB.Init(Cb);  P.HostB.EnableRecordSync(P.StateB, Rb);  P.HostB.Start({});

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

    Lur::App::GameHost::RecordSync Ra, Rb;
    Ra.OnPeerAdopted    = [](const std::string&) { return false; };   // A refuses this peer
    Ra.IsActiveOpponent = [](const std::string&) { return false; };
    Rb.OnPeerAdopted    = [](const std::string&) { return true; };
    Rb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca);  P.HostA.EnableRecordSync(P.StateA, Ra);  P.HostA.Start({});
    P.HostB.Init(Cb);  P.HostB.EnableRecordSync(P.StateB, Rb);  P.HostB.Start({});

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
    Lur::App::GameHost::RecordSync Ra, Rb;
    Ra.OnPeerAdopted    = [&](const std::string&) { ++AdoptedA; return true; };
    Ra.IsActiveOpponent = [](const std::string&) { return true; };
    Rb.OnPeerAdopted    = [](const std::string&) { return true; };
    Rb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca);  P.HostA.EnableRecordSync(P.StateA, Ra);  P.HostA.Start({});
    P.HostB.Init(Cb);  P.HostB.EnableRecordSync(P.StateB, Rb);  P.HostB.Start({});
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

    Lur::App::GameHost::RecordSync R;
    R.OnPeerAdopted    = [](const std::string&) { return true; };
    R.IsActiveOpponent = [](const std::string&) { return true; };
    R.Summarize = [] {
        Lur::App::GameHost::RecordSync::MatchSummary S;
        S.Result = 2; S.WinsLower = 3; S.WinsHigher = 1; S.Draws = 4; S.Total = 8;
        return S;
    };

    Lur::App::GameHost Host;
    Host.Init(C);
    Host.EnableRecordSync(State, R);
    Host.Start({});
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
    Lur::App::GameHost::RecordSync R2 = R;
    R2.Summarize = nullptr;
    CounterState State2;
    Lur::App::GameHost Host2;
    Host2.Init(C2);
    Host2.EnableRecordSync(State2, R2);
    Host2.Start({});
    const std::size_t QBefore = Quiet.size();
    Host2.OnMatchEnded();
    CHECK(Quiet.size() == QBefore);
}

// ---- 5. A game with NO record sync still gets identity + a session, and no SyncManager ------
// This is the RPS shape, and it is why the record flow became opt-in: RPS's ScoreBook is not an
// ISaveState, it is never sent over the wire, and its resync means "rebase the lockstep timeline",
// not "re-adopt and re-send a record". Forcing it through the chess shape would have meant a dummy
// save-state plus three no-op hooks — the tell that a default is wrong.
static void TestHostWithoutRecordSync() {
    Pair P;
    P.Ta.SetDeferred(true);
    P.Tb.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(P.Ta, P.Tb);

    int ReadyA = 0, ResyncA = 0;
    Lur::App::GameHost::Hooks Ha, Hb;
    Ha.OnLinkReady = [&] { ++ReadyA; };
    Ha.OnResync    = [&] { ++ResyncA; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DirA(); Ca.Transport = &P.Ta;
    Cb.SaveDir = DirB(); Cb.Transport = &P.Tb;
    P.HostA.Init(Ca);  P.HostA.Start(Ha);   // no EnableRecordSync
    P.HostB.Init(Cb);  P.HostB.Start(Hb);

    P.Pump(40);

    CHECK(P.HostA.Session().IsReady());          // identity + session still work
    CHECK(!P.HostA.DeviceId().empty());
    CHECK(!P.HostA.HasRecordSync());             // ...and no SyncManager was ever constructed
    CHECK(ReadyA == 1);                          // the game's own link hook fired, exactly once

    // The resync route reaches the game's hook — this is RPS's BeginResync path.
    P.HostA.Session().RequestResync();
    P.Pump(10);
    CHECK(ResyncA >= 1);

    // And the no-record-sync host tolerates the record calls without a save state behind it, so a
    // game need not guard every call site on "did I enable that half".
    P.HostA.OnBackground();
    P.HostA.OnMatchEnded();
}

int main() {
    TestInitialLinkAdoptsAndSyncs();
    TestNonAdoptSendsNothing();
    TestReconnectReAdoptsAndReSyncs();
    TestMatchEndPersistsAndReportsOnce();
    TestHostWithoutRecordSync();
    if (Failures == 0) std::printf("app_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
