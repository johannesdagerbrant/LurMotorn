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
#include <map>
#include <string>
#include <vector>

#include "Lur/App/GameHost.h"
#include "Lur/Transport/Loopback.h"

// Each host needs its OWN save dir, because the device GUID is persisted per directory: point two
// hosts at one dir and they load the SAME id, which makes a peer indistinguishable from itself and
// quietly invalidates every assertion below. (Distinct dirs also mean Persist() really writes, so
// the disk half of the flow is exercised rather than failing silently into a missing directory.)
// WIPED once per run, not merely created. These tests really do write to disk (that is the point of
// TestMatchEndReachesDisk), so a dir left over from the previous run carries a persisted record — and
// since adoption LOADS that record and merges it, a stale file silently changes what later tests
// observe. Found the hard way: the disk test's own output leaked into the first test's expectations.
static std::string MakeCleanDir(const std::string& Name) {
    const std::string D = (std::filesystem::temp_directory_path() / Name).string();
    std::error_code Ec;
    std::filesystem::remove_all(D, Ec);        // ignore "not there"; we only need it gone
    std::filesystem::create_directories(D, Ec);
    return D;
}

// PER-TEST, and wiped. These tests really write to disk (that is the point of
// TestMatchEndReachesDisk), and adoption LOADS the stored record and merges it — so any shared
// directory turns one test's output into another's hidden input. Both variants of that bit me: a dir
// left from the previous RUN, and then within one run, test 1's OnSync persisting a record that test 2
// then adopted. Keyed on __func__ so each test gets its own empty disk without having to invent a name.
static const char* Dir(const char* Test, const char* Side) {
    static std::map<std::string, std::string> Cache;
    const std::string Key = std::string(Test) + "_" + Side;
    auto It = Cache.find(Key);
    if (It == Cache.end()) It = Cache.emplace(Key, MakeCleanDir("lur_app_tests_" + Key)).first;
    return It->second.c_str();
}
#define DIR_A Dir(__func__, "a")
#define DIR_B Dir(__func__, "b")

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
    Ca.SaveDir = DIR_A; Ca.Transport = &P.Ta;
    Cb.SaveDir = DIR_B; Cb.Transport = &P.Tb;
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
    int AdoptCallsB = 0;
    Rb.OnPeerAdopted    = [&](const std::string&) { ++AdoptCallsB; return true; };
    Rb.IsActiveOpponent = [](const std::string&) { return true; };
    // The capture seam: A REFUSES B's record, and must still be handed the bytes. The host owns
    // EMsgType::Sync outright, so without this hook the desktop workbench's flight recorder — whose
    // only DatagramIn call site was the handler the host replaces — would have gone quiet on
    // adoption, and a shorter recording is not a failure anything reports.
    int  ObservedA = 0;
    std::size_t ObservedBytesA = 0;
    Ra.OnRecordDatagram = [&](const uint8_t* D, std::size_t N) {
        if (D != nullptr && N > 0) { ++ObservedA; ObservedBytesA += N; }
    };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DIR_A; Ca.Transport = &P.Ta;
    Cb.SaveDir = DIR_B; Cb.Transport = &P.Tb;
    int ReadyB = 0, ResyncB = 0;
    Lur::App::GameHost::Hooks Hb;
    Hb.OnLinkReady = [&] { ++ReadyB; };
    Hb.OnResync    = [&] { ++ResyncB; };
    P.HostA.Init(Ca);  P.HostA.EnableRecordSync(P.StateA, Ra);  P.HostA.Start({});
    P.HostB.Init(Cb);  P.HostB.EnableRecordSync(P.StateB, Rb);  P.HostB.Start(Hb);

    P.Pump(40);

    // The link still forms — refusing to adopt is a record-sharing decision, not a transport one.
    CHECK(P.HostA.Session().IsReady());
    // A never sent its record, so B's counter is untouched by A.
    CHECK(P.StateB.Value == 1);
    // And B's record never entered A's, because A's IsActiveOpponent gate refused it.
    CHECK(P.StateA.Value == 9);
    CHECK(P.StateA.Merges == 0);
    // B adopted A, so B DID send its record; A observed the bytes and then refused them. Both
    // halves matter: seeing it proves the observer runs, Merges == 0 proves it did not become a
    // back door around the gate.
    CHECK(ObservedA == 1);
    CHECK(ObservedBytesA > 0);
    // ONE record per link-up, from ONE adopt call. Both counts read 2 until 2026-08-12, because
    // Session fired a spurious reconnect on the initial link (see TestInitialLinkIsNotReportedAsReconnect
    // in the Net suite) — so pin them: a doubled record send is invisible in behaviour, since
    // MergeIfNewer is monotonic, and only a count catches it coming back.
    CHECK(AdoptCallsB == 1);
    CHECK(ReadyB == 1);
    CHECK(ResyncB == 0);
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
    Ca.SaveDir = DIR_A; Ca.Transport = &P.Ta;
    Cb.SaveDir = DIR_B; Cb.Transport = &P.Tb;
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
    C.SaveDir = DIR_A;
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
    Ca.SaveDir = DIR_A; Ca.Transport = &P.Ta;
    Cb.SaveDir = DIR_B; Cb.Transport = &P.Tb;
    P.HostA.Init(Ca);  P.HostA.Start(Ha);   // no EnableRecordSync
    P.HostB.Init(Cb);  P.HostB.Start(Hb);

    P.Pump(40);

    CHECK(P.HostA.Session().IsReady());          // identity + session still work
    CHECK(!P.HostA.DeviceId().empty());
    CHECK(!P.HostA.HasRecordSync());             // ...and no SyncManager was ever constructed
    CHECK(ReadyA == 1);                          // the game's own link hook fired, exactly once

    // The resync route reaches the game's hook — this is RPS's BeginResync path.
    //
    // The Pump first, and it is not padding: going Ready ARMS the resync gate (#71 — hold moves until
    // the peer's Sync reconciles), and RequestResync refuses while that gate is up so a resync cannot
    // storm. A host with no record sync never sends a Sync, so only the ~3 s fallback lifts it.
    //
    // Until 2026-08-12 this assertion passed WITHOUT any of that, because Session mistook the initial
    // link for a reconnect and fired OnResync spuriously — so the line below proved nothing about the
    // route it names, and the count it read came from the bug. Fixing Session is what exposed it. A
    // test that passes for the wrong reason is worse than a missing one: it reports coverage.
    CHECK(ResyncA == 0);                         // nothing has dropped, so nothing has resynced yet
    P.Pump(200);                                 // 200 x 16 ms > the 3 s gate fallback
    CHECK(!P.HostA.Session().IsAwaitingResync());
    P.HostA.Session().RequestResync();
    P.Pump(10);
    CHECK(ResyncA == 1);
    CHECK(ReadyA == 1);                          // a resync is not a re-handshake

    // And the no-record-sync host tolerates the record calls without a save state behind it, so a
    // game need not guard every call site on "did I enable that half".
    P.HostA.OnBackground();
    P.HostA.OnMatchEnded();
}

// ---- 6. A match end actually REACHES DISK -----------------------------------------------------
// The regression this exists for, found on device 2026-08-12 and not by any test: persistence had
// silently stopped. `SyncManager::Persist()` writes under the peer KEY, the key is set by OnLink, and
// OnLink was being skipped — so Persist() no-opped for the whole run while every log line, including
// the MATCH END tally, still read as success. Nothing on the host noticed, because nothing on the host
// had ever asserted that the bytes land.
//
// So assert the artifact, not the log. This is the same lesson as the rest of this batch: in this
// codebase a success-shaped signal is not evidence, and the only honest check is the far end.
static void TestMatchEndReachesDisk() {
    Pair P;
    P.Ta.SetDeferred(true);
    P.Tb.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(P.Ta, P.Tb);

    P.StateA.Value = 11;

    Lur::App::GameHost::RecordSync Ra, Rb;
    Ra.OnPeerAdopted    = [](const std::string&) { return true; };
    Ra.IsActiveOpponent = [](const std::string&) { return true; };
    Rb.OnPeerAdopted    = [](const std::string&) { return true; };
    Rb.IsActiveOpponent = [](const std::string&) { return true; };

    Lur::App::GameHost::Config Ca, Cb;
    Ca.SaveDir = DIR_A; Ca.Transport = &P.Ta;
    Cb.SaveDir = DIR_B; Cb.Transport = &P.Tb;
    P.HostA.Init(Ca);  P.HostA.EnableRecordSync(P.StateA, Ra);  P.HostA.Start({});
    P.HostB.Init(Cb);  P.HostB.EnableRecordSync(P.StateB, Rb);  P.HostB.Start({});
    P.Pump(40);
    CHECK(P.HostA.Session().IsReady());

    const std::string Peer = P.HostA.Session().GetPeerGuid();
    CHECK(!Peer.empty());

    // Nothing on disk for this peer until a match resolves...
    P.StateA.Value = 12;
    P.HostA.OnMatchEnded();
    // ...and after it, the bytes are there under the peer's key. If OnLink never ran, Persist() is a
    // no-op and this vector is empty — which is exactly the bug, caught here instead of on a phone.
    const std::vector<uint8_t> OnDisk = P.HostA.Store().Load(Peer);
    CHECK(!OnDisk.empty());
    CHECK(OnDisk.size() >= 2);
    if (OnDisk.size() >= 2) {
        const uint32_t Stored = static_cast<uint32_t>(OnDisk[0] | (OnDisk[1] << 8));
        CHECK(Stored == 12);   // and they are the CURRENT record, not a stale one
    }

    // OnBackground is the other persist route (the app going away mid-match); it must land too.
    P.StateA.Value = 13;
    P.HostA.OnBackground();
    const std::vector<uint8_t> After = P.HostA.Store().Load(Peer);
    CHECK(After.size() >= 2);
    if (After.size() >= 2)
        CHECK(static_cast<uint32_t>(After[0] | (After[1] << 8)) == 13);
}

int main() {
    TestInitialLinkAdoptsAndSyncs();
    TestNonAdoptSendsNothing();
    TestReconnectReAdoptsAndReSyncs();
    TestMatchEndPersistsAndReportsOnce();
    TestHostWithoutRecordSync();
    TestMatchEndReachesDisk();
    if (Failures == 0) std::printf("app_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
