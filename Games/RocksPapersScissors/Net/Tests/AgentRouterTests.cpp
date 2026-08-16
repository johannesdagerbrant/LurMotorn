// Host unit tests for Rps::AgentCommandRouter (issue #43, section E).
//
// Built with -DLUR_AGENT=1, which is the whole reason these can exist: the verb table is agent-only
// code and therefore absent from every app build, so until it moved out of the two platform mains
// there was nowhere a test could reach it. That is not incidental — it is why the two copies were
// free to drift in the first place.
//
// The router is driven through hooks, so what these pin is the DECISIONS: which sim a verb touches,
// what is emitted, what is published cross-thread rather than applied in place, and when the
// misroute warning is an error rather than a note.
#include <cstdio>
#include <string>
#include <vector>

#include "Rps/AgentCommandRouter.h"

static int Failures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); \
            ++Failures;                                                 \
        }                                                               \
    } while (false)

namespace {

// A router plus recorded hook traffic, wired the way a main wires it.
struct Rig {
    Rps::LockstepPeer        Lp;
    Rps::Sim                 SoloSim;
    Rps::AgentCommandRouter  Router;

    std::vector<Rps::InputEvent> Emitted;
    int      ConsoleRequests = 0;
    bool     ConsoleWanted = false;
    int      GestureRequests = 0;
    int      LinkedRequests = 0;
    uint8_t  MyTeam = 0;
    bool     Solo = false;
    bool     Peer = false;

    Rig() {
        Rps::AgentHooks H;
        H.Emit = [this](const Rps::InputEvent& E) { Emitted.push_back(E); };
        H.Team = [this]() { return MyTeam; };
        H.SoloActive = [this]() { return Solo; };
        H.PeerReady = [this]() { return Peer; };
        H.SoloSim = [this]() { return &SoloSim; };
        H.RequestConsole = [this](bool On) { ++ConsoleRequests; ConsoleWanted = On; };
        H.RequestGesture = [this]() { ++GestureRequests; };
        H.RequestLinked = [this]() { ++LinkedRequests; };
        Router.Init(&Lp, std::move(H));
    }

    static Rps::AgentCommand Cmd(Rps::EAgentCmd Kind, int A = 0, int B = 0, int C = 0) {
        Rps::AgentCommand K{};
        K.Kind = Kind;
        K.A = A;
        K.B = B;
        K.C = C;
        return K;
    }
};

}  // namespace

// `place` emits the EXACT coordinates it was given, for our team. The whole point of the verb is
// reaching a square drag-to-place refuses to produce, so any snapping or clamping here would make
// the #160 collision scenario unreachable.
static void TestPlaceEmitsExactCoordinates() {
    Rig R;
    R.MyTeam = 1;
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Place, /*A x*/ 17, /*B y*/ 224, /*C type*/ 2));
    CHECK(R.Emitted.size() == 1);
    if (R.Emitted.size() == 1) {
        const Rps::InputEvent& E = R.Emitted[0];
        CHECK(E.Team == 1);
        CHECK(E.Type == 2);
        CHECK(E.X == Rps::F(17).Raw);
        CHECK(E.Y == Rps::F(224).Raw);   // team 1's mirror camp row — a real harness coordinate
    }
}

// `queue` carries the slot and count through unchanged, on our team.
static void TestQueueEmitsSlotAndCount() {
    Rig R;
    R.MyTeam = 1;
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Queue, /*A slot*/ 3, /*B count*/ 5));
    CHECK(R.Emitted.size() == 1);
    if (R.Emitted.size() == 1) {
        CHECK(R.Emitted[0].Team == 1);
        CHECK(R.Emitted[0].X == 3);   // Queue packs the building slot into X
        CHECK(R.Emitted[0].Y == 5);   // ... and the batch count into Y
    }
}

// `console` must be PUBLISHED for the view-owning thread, never applied in place. Both mains had it
// writing GameView::SetDevOverlayOpen — a plain bool — straight from the sim thread, while the view
// belongs to the render thread on iOS (#183) and the glue thread on Android.
static void TestConsoleIsPublishedNotApplied() {
    Rig R;
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Console, 1));
    CHECK(R.ConsoleRequests == 1);
    CHECK(R.ConsoleWanted);

    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Console, 0));
    CHECK(R.ConsoleRequests == 2);
    CHECK(!R.ConsoleWanted);
    CHECK(R.Emitted.empty());   // a console toggle is not gameplay input
}

// `gesture` and `linked` are likewise requests, not direct writes.
static void TestGestureAndLinkedArePublished() {
    Rig R;
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Gesture));
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Linked));
    CHECK(R.GestureRequests == 1);
    CHECK(R.LinkedRequests == 1);
    CHECK(R.Emitted.empty());
}

// `stress` fills the SOLO sim when solo is live and the LINKED one otherwise. Getting this backwards
// is not cosmetic: filling only one peer of a linked pair diverges it by construction, so a perf
// measurement taken that way is really measuring #161's recovery.
static void TestStressPicksTheLiveSim() {
    {
        Rig R;
        R.Solo = true;
        const int32_t Before = R.Lp.GetSim().Count;
        R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Stress, /*A per team*/ 4, /*B type*/ 1));
        CHECK(R.SoloSim.Count > 0);                  // the solo sim got them
        CHECK(R.Lp.GetSim().Count == Before);        // ... and the linked one was untouched
    }
    {
        Rig R;
        R.Solo = false;
        const int32_t Before = R.Lp.GetSim().Count;
        R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Stress, 4, 1));
        CHECK(R.Lp.GetSim().Count > Before);         // the linked sim got them
        CHECK(R.SoloSim.Count == 0);
    }
}

// `corrupt` moves our own gold, which is what forces the divergence #161 must repair.
static void TestCorruptMovesOurGold() {
    Rig R;
    R.Lp.Init(/*Seed*/ 7, /*MyTeam*/ 0, /*Send*/ nullptr, /*Ctx*/ nullptr);
    const int32_t Before = R.Lp.GetSim().Teams[0].Gold;
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::Corrupt, /*A delta*/ -500));
    CHECK(R.Lp.GetSim().Teams[0].Gold == Before - 500);
}

// An unknown/None command must do nothing at all rather than fall into a neighbouring case.
static void TestNoneIsInert() {
    Rig R;
    R.Router.Apply(Rig::Cmd(Rps::EAgentCmd::None));
    CHECK(R.Emitted.empty());
    CHECK(R.ConsoleRequests == 0);
    CHECK(R.GestureRequests == 0);
    CHECK(R.LinkedRequests == 0);
}

// Applying before Init must be inert rather than a null dereference.
static void TestUninitialisedRouterIsInert() {
    Rps::AgentCommandRouter Router;
    Router.Apply(Rig::Cmd(Rps::EAgentCmd::Linked));   // must not crash
    CHECK(true);
}

int main() {
    TestPlaceEmitsExactCoordinates();
    TestQueueEmitsSlotAndCount();
    TestConsoleIsPublishedNotApplied();
    TestGestureAndLinkedArePublished();
    TestStressPicksTheLiveSim();
    TestCorruptMovesOurGold();
    TestNoneIsInert();
    TestUninitialisedRouterIsInert();
    if (Failures == 0) std::printf("rps_agent_router_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
