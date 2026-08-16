// Host unit tests for Lur::App::RenderHandshake (issue #43, Phase 3 section C).
//
// What these are really testing is a DEADLOCK and a USE-AFTER-FREE that sit on opposite sides of one
// switch. Parking the renderer means "wait for the ack" when a render thread owns it and "there is
// nobody to wait for" when the frame loop is the caller — so a single unified park that always waits
// hangs chess, and one that never waits frees a live drawable under RPS. Both are the kind of bug
// that reproduces on a phone and nowhere else, which is exactly why the rule is here instead of in
// two .mm files.
//
// The rest is ordering: a reattach must not rebuild against the surface it is about to replace, and
// a request must not be lost because it arrived while the loop was parked. Those had no test at all
// while they were sixteen loose atomics in RpsMain.mm.
#include <cstdio>
#include <thread>

#include "Lur/App/RenderHandshake.h"

using Lur::App::ERenderTopology;
using Lur::App::RenderHandshake;
using Lur::App::RenderWork;

static int Failures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); \
            ++Failures;                                                 \
        }                                                               \
    } while (false)

static int SurfaceA = 0, SurfaceB = 0;   // stand-ins for two CAMetalLayers; only identity matters

// ---------------------------------------------------------------------------------------------
// The topology decision itself.

// Dedicated: requesting a park is NOT being parked. Main must see false until the frame loop acks
// at a safe point — that gap is the whole reason the ack exists.
static void TestDedicatedParkRequiresTheAck() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();

    H.RequestPark();
    CHECK(!H.IsParked());          // the render thread has not answered yet

    const RenderWork W = H.TakeWork();
    CHECK(W.Park);                 // ... and this is how it finds out
    H.AckParked(true);
    CHECK(H.IsParked());           // only now may main touch the surface

    H.Resume();
    H.AckParked(false);
    CHECK(!H.IsParked());
}

// Inline: the caller IS the frame loop, so it cannot be mid-frame while asking. Parked is true
// immediately. If this returned false, chess would spin forever waiting for an ack that only the
// spinning thread could give.
static void TestInlineParkIsImmediate() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Inline);
    H.Start();

    H.RequestPark();
    CHECK(H.IsParked());           // no second thread; requesting is parking

    H.Resume();
    CHECK(!H.IsParked());
}

// And the same call site, unchanged, gets both meanings. This is the absorption the section exists
// for: the platform code says "park", not "park differently depending on which game I am".
static void TestSameCallSiteBothTopologies() {
    for (const ERenderTopology T : {ERenderTopology::Inline, ERenderTopology::Dedicated}) {
        RenderHandshake H;
        H.Configure(T);
        H.Start();
        H.RequestPark();
        if (T == ERenderTopology::Dedicated) {   // the loop is there to answer
            (void)H.TakeWork();
            H.AckParked(true);
        }
        CHECK(H.IsParked());
    }
}

// Inline has no park to hand out: returning Park=true would tell the platform thread to park itself,
// which is a stop-the-app bug rather than a park. Chess stops its display link instead.
static void TestInlineNeverHandsOutPark() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Inline);
    H.Start();
    H.RequestPark();
    CHECK(!H.TakeWork().Park);
}

// ---------------------------------------------------------------------------------------------
// Ordering rules.

// #73 reattach: park -> ack -> platform swaps the surface -> release -> reinit. The reinit must NOT
// be consumed during the park, or the renderer rebuilds against the layer that is about to be
// replaced — a rebuild that succeeds and then renders to a dead window-server surface, which is the
// silent-black-screen shape this repo keeps rediscovering.
static void TestReattachDefersReinitUntilAfterTheRelease() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();
    H.PublishSurface(&SurfaceA, 100, 200);

    H.RequestReattach(&SurfaceB, 300, 400);

    const RenderWork Parked = H.TakeWork();
    CHECK(Parked.Park);
    CHECK(!Parked.Reinit);         // suppressed while parked — the point of this test
    H.AckParked(true);

    H.Resume();
    H.AckParked(false);

    const RenderWork After = H.TakeWork();
    CHECK(!After.Park);
    CHECK(After.Reinit);           // now, and against the NEW surface
    CHECK(After.Surface == &SurfaceB);
    CHECK(After.W == 300 && After.H == 400);
}

// A request raised while parked survives the park. These are level flags, not edges, precisely so a
// rotation during backgrounding is not silently dropped — the app would come back at the old
// swapchain size and letterbox.
static void TestRequestMadeWhileParkedIsNotLost() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();
    H.PublishSurface(&SurfaceA, 100, 200);

    H.RequestPark();
    (void)H.TakeWork();
    H.AckParked(true);

    H.RequestResize(640, 480);     // rotated while backgrounded

    H.Resume();
    H.AckParked(false);

    const RenderWork W = H.TakeWork();
    CHECK(W.Resize);
    CHECK(W.W == 640 && W.H == 480);
}

// Consume-once, both directions. A resize taken twice is a wasted swapchain rebuild; a reattach-done
// taken twice releases the retiring view twice.
static void TestWorkIsConsumedOnce() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();

    H.RequestResize(10, 20);
    CHECK(H.TakeWork().Resize);
    CHECK(!H.TakeWork().Resize);

    H.SignalReinitDone();
    CHECK(H.TakeReattachDone());
    CHECK(!H.TakeReattachDone());
}

// The frame loop must not init before the platform has a sized surface, and must not be told the
// renderer is usable before init returns. Both gates start closed.
static void TestNothingIsReadyBeforeItIs() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    CHECK(!H.ShouldRun());         // Start() is explicit; a default-constructed host runs nothing
    CHECK(!H.HasSurface());
    CHECK(!H.IsReady());

    H.Start();
    CHECK(H.ShouldRun());
    CHECK(!H.HasSurface());        // Start does not conjure a surface

    H.PublishSurface(&SurfaceA, 100, 200);
    CHECK(H.HasSurface());
    CHECK(H.Surface() == &SurfaceA);
    CHECK(!H.IsReady());           // published != initialised

    H.SetReady(true);
    CHECK(H.IsReady());
}

// A failed init must leave Ready false, and a reinit must clear it for the duration. A frame issued
// against a torn-down device is a crash, not a dropped frame.
static void TestReadyClearsAcrossReinit() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();
    H.PublishSurface(&SurfaceA, 100, 200);
    H.SetReady(true);

    H.SetReady(false);             // frame loop entering the rebuild
    CHECK(!H.IsReady());
    H.SetReady(false);             // ... and this one failed
    CHECK(!H.IsReady());
    H.SignalReinitDone();
    CHECK(H.TakeReattachDone());   // done is reported even on failure — main still owns a dead view
    CHECK(!H.IsReady());
}

// Stop() must break a PARKED loop too. If it only cleared the run flag, teardown would have to
// Resume() first, and forgetting that is a hang on app exit rather than a visible error.
static void TestStopReleasesAParkedLoop() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();
    H.RequestPark();
    (void)H.TakeWork();
    H.AckParked(true);

    H.Stop();
    CHECK(!H.ShouldRun());         // the park loop's other exit condition
}

// ---------------------------------------------------------------------------------------------
// One real thread, because the whole class exists for a cross-thread handoff and a single-threaded
// test cannot show that the ack actually orders anything. Not a race detector — it is a check that
// the protocol terminates when the two sides really are concurrent.
static void TestParkHandshakeAcrossRealThreads() {
    RenderHandshake H;
    H.Configure(ERenderTopology::Dedicated);
    H.Start();
    H.PublishSurface(&SurfaceA, 100, 200);
    H.SetReady(true);

    int FramesDrawn = 0;
    std::thread Loop([&] {
        while (H.ShouldRun()) {
            const RenderWork W = H.TakeWork();
            if (W.Park) {
                H.AckParked(true);
                while (H.ParkRequested() && H.ShouldRun())
                    std::this_thread::yield();
                H.AckParked(false);
                continue;
            }
            ++FramesDrawn;
            std::this_thread::yield();
        }
    });

    H.RequestPark();
    while (!H.IsParked()) std::this_thread::yield();
    const int AtPark = FramesDrawn;
    // Parked means parked: no frame may start while main believes it owns the surface.
    for (int I = 0; I < 1000; ++I) std::this_thread::yield();
    CHECK(FramesDrawn == AtPark);

    H.Resume();
    while (FramesDrawn == AtPark && H.ShouldRun()) std::this_thread::yield();
    CHECK(FramesDrawn > AtPark);   // and it comes back

    H.Stop();
    Loop.join();
}

int main() {
    TestDedicatedParkRequiresTheAck();
    TestInlineParkIsImmediate();
    TestSameCallSiteBothTopologies();
    TestInlineNeverHandsOutPark();
    TestReattachDefersReinitUntilAfterTheRelease();
    TestRequestMadeWhileParkedIsNotLost();
    TestWorkIsConsumedOnce();
    TestNothingIsReadyBeforeItIs();
    TestReadyClearsAcrossReinit();
    TestStopReleasesAParkedLoop();
    TestParkHandshakeAcrossRealThreads();
    if (Failures == 0) std::printf("render_handshake_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
