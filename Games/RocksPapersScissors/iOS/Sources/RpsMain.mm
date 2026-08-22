// iOS entry point for RocksPapersScissors (Phase 2, slice 2) — the counterpart of
// Games/RocksPapersScissors/Android/.../RpsMain.cpp. A thin UIKit shim: a Metal-backed
// view (CAMetalLayer) hosts the shared Vulkan renderer (via MoltenVK) and drives ONE
// Rps::LockstepPeer + the shared Rps::GameView. A phone IS a single peer: it exchanges
// per-tick input with the OTHER phone over real BLE (reliable/ordered, so the same
// lockstep the host tests prove runs unchanged). Copy-pasted platform glue is #42's
// future extraction material — earned once this second consumer exists.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <os/log.h>

#include <atomic>    // #69: the sim<->render cross-thread surface (mirrors Android's AppState atomics)
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>   // setenv: MoltenVK log level, before any vkCreateInstance
#include <ctime>     // the flight recorder's per-match filename stamp
#include <mutex>     // #183: guards the main->render touch-event queue
#include <pthread.h> // #183: the render thread runs on a pthread (not std::thread) for a 4MB stack
#include <string>
#include <utility>
#include <thread>    // #69/#183: the sim/net thread and now the render thread, both off the UIKit main thread
#include <vector>    // #183: the touch-event queue backing store

#include "Lur/Core/CVar.h"   // #147: registry walk for the gameplay-CVar sync seed
#include "Lur/Core/Log.h"    // the engine logger — routed into os_log below
#include "Lur/Input/ConsoleGesture.h"  // #151: the ONE dev-console gesture, shared with Android
#include "Lur/Input/Input.h"           // #43 section D: the shared TouchEvent (was a private near-copy)
#include "Rps/AgentControl.h"          // LUR_AGENT: assistant remote-control command grammar
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/App/GameHost.h"     // #43: engine-owned identity + session lifecycle
#include "Lur/App/IosApp.h"        // #43 section B: the shared entry point + Metal view + delegate
#include "Lur/App/IosViewHost.h"  // #43 section C: the whole shared #73 heal, renderer included
#include "Lur/App/Platform.h"     // #43 section B: MoltenVK stdio guard + log sink
#include "Lur/App/RenderHandshake.h"  // #43 section C: the MAIN<->render surface/park protocol
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Trace/Trace.h"  // #69: emit the CPU-scope/latency TRACE line (ble.toApply is the target metric)
#include "Lur/Transport/Ble.h"
#include "Rps/AiController.h"
#include "Lur/Input/ScrollCamera.h"
#include "Rps/GameView.h"
#include "Rps/ViewMetrics.h"   // #43 section D: Ppu / WorldHeightF / WorldToFixed / GhostOffsetPx
#include "Rps/TouchRouter.h"   // #43 section D: what a touch MEANS, shared with Android + desktop
#include "Rps/AgentCommandRouter.h"  // #43 section E: the shared agent verb table (LUR_AGENT)
#include "Rps/LockstepPeer.h"
#include "Rps/MatchRecord.h"   // #144 solo flight recorder (LUR_INTERNAL; parity with Android)
#include "Rps/ScoreBook.h"     // persistent all-time W-L-D per AI tier / per rival
#include "Rps/SessionWiring.h" // the ONE Session->LockstepPeer routing table (#160)
#include "Rps/Snapshot.h"      // Snapshot + SnapshotMailbox (the sim->render hand-off)
#include "Rps/SoloInput.h"     // #69: SoloInputInbox — the thread-safe glue->sim solo event queue
#include "Rps/Tunables.h"

namespace {
// Both phones derive the SAME match seed (gameplay-inert v1 map; GUID-derived seeding is
// the design's later refinement — spec §2).
constexpr uint64_t kMatchSeed = 0x52505353ull;  // 'RPSS'
constexpr uint64_t kStepNs = 1'000'000'000ull / Rps::TickRateHz;

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
// #43 section D: shared with the Android + desktop mains via Rps/ViewMetrics.h.
using Rps::Ppu;
using Rps::WorldHeightF;
void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}
// The engine logger had NO sink on either phone, so every Lur::Log::Info/Error from inside the engine
// and the netcode went to a stderr nobody reads. The shim's own os_log calls were visible, which is
// what hid it — the engine's voice was muted while the app looked chatty. It cost a diagnosis on
// 2026-07-30: the #112 build-fingerprint gate fired, said so via Lur::Log::Error, and the line went
// nowhere. %{public}s is mandatory — a plain %s is redacted to <private> unless Xcode is attached,
// which is never our case (see the iOS notes in CLAUDE.md).
// View-side world (float) -> Fixed for a place event (#139). The raw int travels into the sim /
// over the wire, so no float crosses the determinism boundary.
using Rps::WorldToFixed;
// #183: a raw touch packaged on the MAIN (UIKit) thread and drained on the RENDER thread. The UIKit
// handlers must no longer touch _View/_Cam/_DevGesture (those live on the render thread now), so they
// capture only what UIKit alone can read — the point in DRAWABLE PIXELS (already ×contentsScale), the
// pointer count (event.allTouches.count, for the two-finger console gesture), and a timestamp for the
// gesture's hold/chain windows — and push it. The render thread replays the exact hit-test logic.
//
// #43 section D: the struct itself is Lur::Input::TouchEvent now. This file had a private near-copy
// that differed from the engine's by one field — the engine carried a PointerId nothing read, and
// lacked the PointerCount the console gesture needs — so the duplicate existed because the shared
// type could not say the one thing RPS had to say. It can now.
using TouchEvent = Lur::Input::TouchEvent;
}  // namespace

// #103: iOS-ONLY render-resolution knob for the fillrate-vs-encoding A/B. Multiplies the CAMetalLayer
// backing scale so the whole scene renders into a smaller drawable and Core Animation upscales it to
// the panel — one dial that shrinks BOTH the render extent and (because the touch handlers read the
// same contentsScale) the input mapping, so nothing goes out of register. RENDER ONLY (CVarFlagNone):
// never enters the sim/hash/wire, so the two phones may legitimately disagree on it — exactly like
// rps.mine.visual_size. Declared here, not in Tunables.h, because only the iPhone honours it: a knob
// that did nothing on Android would be a console footgun. The number is what the last comment on #103
// asked for — drop below native retina (~0.7×) and watch the TRACE line: if fps locks to 60 it is
// fillrate/overdraw-bound, if it stays ~40 it is CPU/MoltenVK-encoding-bound.
namespace {
LUR_CVAR_RANGE(CvRenderScale, "rps.dev.render_scale", Rps::F(1), Rps::F(1, 4), Rps::F(1),
               Lur::Core::CVarFlagNone,
               "iOS render-resolution multiplier (1.0 = native retina; lower = cheaper fill). #103 A/B");
}  // namespace

// #43 section B: the Metal-backed view (LurMetalView) and the app delegate are the engine's now —
// both games had declared their own, identical apart from the class name.

@interface RpsViewController : UIViewController
// #183: declared so the C pthread trampoline below can message it (the render thread runs on a raw
// pthread, not std::thread, to get a 4MB stack — see the ivar note).
- (void)renderThreadLoop;
@end

// #183: pthread entry trampoline — re-enters the ObjC render loop. Ctx is the VC passed raw at
// pthread_create; the VC outlives the thread (-dealloc joins it before destroying its C++ ivars).
static void* RpsRenderThreadTrampoline(void* Ctx) {
    @autoreleasepool { [(__bridge RpsViewController*)Ctx renderThreadLoop]; }
    return nullptr;
}

// #69: iOS mirrors Android's #91 split — a dedicated SIM thread owns Session + Lp + the solo sim
// (pumps BLE, ticks the lockstep/solo sim ~500 Hz, publishes snapshots). #183 then took the render
// loop OFF the main thread too, onto its own free-running RENDER thread (renderThreadLoop). Before
// #69 the iPhone serviced the transport once per vsync-locked frame, so an inbound datagram waited up
// to ~16 ms (measured: ble.toApply 0.2 -> ~11 ms, the textbook render-gate). The cross-thread surface
// below is the whole boundary: the Mailbox (snapshot sim->render), the SoloIn inbox +
// Lp.QueueLocalEvent (input render->sim, both thread-safe), and the atomics. Renderer/View/Cam/
// DevGesture are RENDER-only; UIKit is MAIN-only; Session/Lp/Sim are sim-only after the thread starts.
@implementation RpsViewController {
    // #183: the iPhone render loop now runs on its OWN thread (renderThreadLoop), not the CADisplayLink
    // main-thread callback. That is the fix for the 40fps MoltenVK vsync BEAT (#103): a main-thread loop
    // blocked on nextDrawable can never overlap frame N+1's CPU work with frame N's drawable wait, so
    // FIFO + CADisplayLink beat to 2-of-3 refreshes = 40.0fps. Free-running on its own thread with FIFO as
    // the ONE vsync clock lets the CPU run ahead -> clean 60. Mirrors #69, which already put sim+net on
    // their own thread. The hard invariant it buys: the renderer, _View, _Cam, _DevGesture and _Snap are
    // touched by the RENDER thread ONLY; UIKit objects (window/view/layer) + lifecycle by MAIN only; the
    // three threads meet solely at the atomics + queues below.

    // ---- MAIN (UIKit / lifecycle) thread only ----
    Lur::Transport::ITransport* _Transport;   // created on main; the CB delegate pushes to the (thread-safe) inbox
    Lur::Save::Store* _Store;  // -> &_Host.Store(): main uses it once (score load) BEFORE the sim
                               // thread; sim-only after
    std::string _SaveDir;      // Application Support — the Store's dir, kept for the .rec paths
    std::string _DeviceId;
    // #73: a DVT launch can initialise the renderer while the app is NOT active — the layer created in
    // that state is never composited (presents "succeed" into the void; screen black). Recorded on MAIN
    // when the layer is published (main is the only thread allowed to read applicationState); on becoming
    // active the lifecycle tick rebuilds window+view+layer+renderer from scratch (a swapchain recreate is
    // NOT enough — proven by 898999b).
    bool _InitWhileInactive;
    bool _BecameActive;
    NSTimer* _LifecycleTimer;  // #183: MAIN heartbeat — #73 heal, became-active reattach, render_scale; NO rendering
    UIView* _RetiringView;     // #183: old view/layer held alive across a reattach until the render thread finishes Shutdown/Init on it (UIView dealloc stays on main)
#if !LUR_SHIPPING
    // #103: the render-resolution A/B. Tracks the render-scale (Fixed raw) currently baked into the
    // swapchain, so the lifecycle tick recreates it ONLY when rps.dev.render_scale actually changes. Seeded
    // to Fixed::One by the layer-publish paths (native scale, k=1.0), so the default never triggers a recreate.
    int32_t _AppliedRenderScaleRaw;
#endif

    // ---- RENDER thread only (after _RenderThread starts; touches NO UIKit) ----
    Lur::Render::IRenderer* _Renderer;
    Rps::GameView _View;
    Rps::Snapshot _Snap;         // the render thread's consume target (the latest published snapshot)
    uint32_t _LastConsumedTick;  // consume from the mailbox only when the published tick changes
    bool _ViewLinkedApplied;     // one-shot — the peer row + blink is applied once
    Lur::Input::ScrollCamera _Cam;
    bool _CamInit;
    // #151: the dev-console gesture — two-finger triple-tap to open, drag-to-scroll while open. It now
    // runs on the RENDER thread (which owns _View), replayed from the touch queue; the MAIN handlers only
    // capture pointer counts + timestamps into TouchEvent. Shared recognizer (Lur::Input::ConsoleGesture).

    // ---- SIM thread only (after _SimThread starts) ----
    // #43: engine-owned identity + session lifecycle. RPS takes only that half of GameHost — no
    // record sync (ScoreBook is not an ISaveState and never crosses the wire).
    Lur::App::GameHost _Host;
    Rps::LockstepPeer _Lp;
    Rps::ScoreBook _Scores;    // loaded on MAIN before the thread starts (the handoff), sim-only after
    Rps::Sim _SoloSim;         // #2/#127 solo-vs-AI local sim
    Rps::AiController _SoloAi;
#if LUR_AGENT
    // ---- Assistant remote control (CLAUDE.md's LUR_AGENT axis) ----
    // Channel: Documents/agent.cmd inside the app container, holding "<seq> <verb> [args]". A FILE
    // rather than a system property because iOS has no equivalent, and because the dev rig can already
    // push into the container (`pymobiledevice3 apps push <bundle> <local> Documents/<name>`) — the
    // same mechanism the role/clearsave markers use. Polled on the SIM thread (as on Android), which
    // is the only thread allowed to touch _Lp/_SoloSim; the one command that needs the touch recognizer
    // (`gesture`) hands across via _AgentGestureRequest, exactly as Android does.
    //
    // This is the half that most needs it: there is no touch injection for iOS at all, so without this
    // every two-phone scenario needs a person tapping the iPhone. Absent from every ordinary build;
    // clear the file when handing the phone back.
    Rps::AgentControl _AgentCtl;   // sim thread
    std::string _AgentCmdPath;     // set on main, read-only after
    uint64_t _AgentPollNs;         // sim thread
#endif

    // ---- Sim <-> RENDER cross-thread surface. The sim publishes; the RENDER thread now consumes these
    //      (this was the sim<->MAIN surface pre-#183 — the atomics are unchanged, only the consuming thread
    //      moved off the CADisplayLink main thread onto the render thread). "sim -> main" below therefore
    //      means "sim -> render thread" now; input atomics (_SoloAiTier etc.) are set from the render thread
    //      during touch replay instead of from the main touch handlers. ----
    std::thread _SimThread;
    Rps::SnapshotMailbox _Mailbox;         // sim publishes, the render thread consumes
    Rps::SoloInputInbox  _SoloIn;          // render pushes solo place/queue events, sim drains (thread-safe)
    std::atomic<bool>     _SimRunning;     // main -> sim: keep looping (cleared + joined on teardown)
    std::atomic<bool>     _MatchLive;      // sim -> render: a match (solo or peer) is live (drives touch routing)
    std::atomic<uint8_t>  _LinkedTeam;     // sim -> render: which team you play (0 in solo)
    std::atomic<uint32_t> _PublishedTick;  // sim -> render: consume only on a new tick
    std::atomic<uint32_t> _PresentedFrames;// render -> sim: for the LOCKSTEP diag / heartbeat
    std::atomic<bool>     _Recovering;     // sim -> render: #161 desync repair in flight (drives the HUD)
    std::atomic<bool>     _LinkHalfOpen;   // sim -> render: #163 half-open link (drives the HUD banner)
    std::atomic<int>      _SoloAiTier;     // render -> sim: one-shot AI tier pick -> (re)start solo (-1 = none)
    std::atomic<bool>     _SoloActiveAtomic;   // sim -> render: solo match running (tap routing + view side)
    std::atomic<bool>     _PeerLinkedAtomic;   // sim -> render: a real peer connected (row + blink)
    std::atomic<bool>     _SwitchToLinkedAtomic;  // render -> sim: player picked the linked-opponent row
    std::atomic<bool>     _SelectLinkedRow;    // sim -> render: we switched to the peer; name it in the HUD
    // #139/feedback: sim -> main, the camp the player committed while the opponent hasn't placed theirs.
    // Pre-match it is NOT in the sim (both camps become tick 0's input together), so the view draws it
    // from here. RAW Fixed so no float crosses the boundary in a different form than the wire.
    std::atomic<bool>     _PendingCampAtomic;
    std::atomic<int32_t>  _PendingCampX, _PendingCampY;
    std::atomic<int>      _AiWinsA[Rps::AiTierCount], _AiLossesA[Rps::AiTierCount],
                          _AiDrawsA[Rps::AiTierCount];   // sim -> render: per-AI-tier W-L-D display
    std::atomic<int>      _PeerWinsA, _PeerLossesA, _PeerDrawsA;  // sim -> render: vs the linked peer
#if LUR_AGENT
    // Agent `gesture`: the console recognizer lives on the RENDER thread (it owns _DevGesture), so a
    // sim-thread command hands the request across the atomic rather than touching _DevGesture directly.
    std::atomic<bool>     _AgentGestureRequest;
    // -1 = nothing pending, 0/1 = close/open. The RENDER thread applies it (it owns _View);
    // the sim thread only publishes. See wireAgentRouter (#43 section E).
    std::atomic<int>      _AgentConsoleReq;
    Rps::AgentCommandRouter _AgentRouter;
#endif

    // ---- MAIN <-> RENDER cross-thread surface (#183). The render thread owns all Vulkan; MAIN owns UIKit.
    //      Init/Shutdown are handed to the render thread so the renderer is single-threaded even across a
    //      #73 rebuild, and the park/ack pair stops the loop at a safe point for BOTH a background
    //      transition (a free-running loop must not vend drawables off-screen) and a reattach. ----
    // The eleven atomics that used to sit here are now Lur::App::RenderHandshake (#43 section C). They moved
    // because the rule governing them — when "parked" is true — is the one thing that differs between a
    // dedicated render thread and a frame loop on the platform thread, and it was unwritten and untestable
    // while it was eleven loose flags in a .mm file. The object also carries the drawable size, so the
    // publish/consume pairing is visible instead of implied by two same-named atomics.
    Lur::App::RenderHandshake _RH;
    // #183: a raw pthread, NOT std::thread — the render call chain (GameView::Render -> Dropdown::Draw ->
    // Text) needs ~1MB of stack, which it got for free while it ran on the MAIN thread. A std::thread gets
    // the OS default secondary-thread stack (512KB on iOS) with NO way to enlarge it, and the chain
    // overflowed the guard page on launch (SIGBUS in ___chkstk_darwin). pthread_attr_setstacksize gives it
    // a generous 4MB. The sim thread stays std::thread — its path is shallow.
    pthread_t            _RenderThread;
    bool                 _RenderThreadStarted;
    std::atomic<int32_t> _InsetTopPx, _InsetBotPx;  // main -> render: safe-area insets (px)
    std::mutex           _TouchMx;         // guards _TouchQ
    std::vector<TouchEvent> _TouchQ;       // main pushes raw touches, the render thread drains once/frame
    // #43 section D: what a touch MEANS, shared with the Android + desktop mains. RENDER thread only
    // — it holds _View/_Cam/_DevGesture, which are render-owned since #183, so the router runs where
    // they live and the ownership rule is unchanged.
    Rps::TouchRouter _Router;
#if LUR_INTERNAL
    // #144 SOLO FLIGHT RECORDER — parity with Android, which has had it since #156 made it a dev-build
    // default. Without it an iPhone playtest is unreadable afterwards: you get the score and nothing
    // about how the match got there, while the same session on the Galaxy replays tick for tick. Every
    // recording is one file per match in Application Support, next to the score book.
    //
    // Read the switch ONCE per match, at Begin: toggling mid-match cannot truncate a half-written
    // file, and every other recorder call no-ops on an unopened file, so skipping Begin is the gate.
    Rps::MatchRecorder _SoloRec;
    int _SoloMatchNo;
    uint64_t _RecCensusNs;
    std::string _SoloRecFile;
    // #159: the LINKED match's recorder. Both phones write their own file for the same match, and
    // because both execute the identical combined stream the two files can be diffed: `e` lines
    // differing means the wire lost or duplicated a frame, `e` identical with `h` hashes diverging
    // means the sims computed different results from the same input. Before this a linked match
    // wrote nothing, which is why the 2026-07-30 desync could not be chased.
    Rps::MatchRecorder _LinkedRec;
    int _LinkedMatchNo;
    std::string _LinkedRecFile;
    uint32_t _LinkedRecIdx;    // which Lp match index the open recording belongs to (NoRecMatchIdx = none)
#endif
}

- (void)loadView {
    self.view = [[LurMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    // UIKit's DEFAULT IS NO. Without this a view is handed exactly one touch and the second finger
    // is never delivered, so event.allTouches.count never reaches 2 — which is why the two-finger
    // triple-tap could not open the console on the iPhone while working first time on the Galaxy.
    //
    // Not set on LurMetalView itself, deliberately: chess is a one-finger game whose taps commit
    // moves, and handing it a second simultaneous touch would be a behaviour change it has no use
    // for. RPS opts in because RPS is the one with a two-finger gesture.
    self.view.multipleTouchEnabled = YES;
}
- (CAMetalLayer*)metalLayer { return (CAMetalLayer*)self.view.layer; }

- (void)viewDidLoad {
    [super viewDidLoad];
    // BEFORE anything else, including the logger: this is what stops a full stdio pipe from
    // freezing the main thread and getting us watchdog-killed with a black screen.
    Lur::App::Platform::UnblockStdio();
    // Then give the engine logger a home, before anything can try to report a problem.
    Lur::App::Platform::InstallLogSink();   // #43 section B: one sink per platform
#if LUR_INTERNAL
    // #159: "no recording open yet" must be a value MatchIndex can never take, and an Obj-C ivar is
    // ZERO-initialised — which is exactly the index of the FIRST match. The open-on-match-started edge
    // (`_LinkedRecIdx != _Lp.MatchIndex()`) was therefore false for match 0, so this phone recorded
    // nothing until a post-match restart bumped the index to 1. Android got this right with an
    // explicit 0xFFFFFFFF sentinel and iOS inherited the default, so the pair silently recorded
    // ONE side of the first linked match — and diffing two peers is the entire point of #159, which
    // makes the first match the one you least want missing. Found on hardware 2026-07-31.
    _LinkedRecIdx = Rps::NoRecMatchIdx;
#endif

    CAMetalLayer* Layer = [self metalLayer];
    Layer.device = MTLCreateSystemDefaultDevice();
    Layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    Layer.contentsScale = UIScreen.mainScreen.scale;

    // #73: note every activation; the lifecycle tick reattaches if the renderer was born while the app
    // wasn't active (the black-screen precondition), and resumes the render thread parked on resign.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(onBecameActive)
                                                 name:UIApplicationDidBecomeActiveNotification
                                               object:nil];
    // #183: a free-running render thread must be PARKED while inactive/backgrounded — Metal disallows
    // rendering to an off-screen layer, and the old CADisplayLink loop was auto-paused for us. Resumed by
    // onBecameActive. Resign (not just background) also covers Control Center / the app switcher briefly.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(onWillResign)
                                                 name:UIApplicationWillResignActiveNotification
                                               object:nil];

    // #43: one answer for where this app writes. It was computed here, in the chess main, and
    // again inside the CoreBluetooth driver — and the device GUID is written through one and read
    // back through another, so a drift between them is a phone that forgets who it is.
    _SaveDir = Lur::App::Platform::SaveDir();
    {
        Lur::App::GameHost::Config HostCfg;
        HostCfg.SaveDir = _SaveDir;
        HostCfg.Log = [](const char* M) { os_log(OS_LOG_DEFAULT, "OnlyRps: %{public}s", M); };
        HostCfg.Transport = Lur::Transport::CreateBleTransport();
        _Transport = HostCfg.Transport;
        _Host.Init(HostCfg);
    }
    _Store = &_Host.Store();
#if LUR_AGENT
    {
        // Documents/, not Application Support: Documents is what the dev rig can push into.
        NSArray<NSString*>* Docs =
            NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* D = Docs.firstObject ?: NSTemporaryDirectory();
        _AgentCmdPath = std::string(D.UTF8String) + "/agent.cmd";
        _AgentPollNs = 0;
        _AgentConsoleReq.store(-1, std::memory_order_relaxed);
        [self wireAgentRouter];
        os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT CONTROL COMPILED IN (LUR_AGENT=1) — this build must not "
               "be handed to a player. Channel: %{public}s", _AgentCmdPath.c_str());
    }
#endif
    _DeviceId = _Host.DeviceId();   // cached: read-only after startup, read on both threads
    // All-time W-L-D per AI tier / per rival, loaded on MAIN before the sim thread exists (thread
    // creation is the handoff — from then on _Scores is written ONLY by the sim thread). Seeding the
    // display atomics here is what makes the ladder show the real record the moment the dropdown first
    // opens, instead of 0-0-0 until this session's first match resolves; the render thread pushes them
    // to the view every frame.
    _Scores.Load(*_Store);
    for (int T = 0; T < Rps::AiTierCount; ++T) {
        const Rps::Tally S = _Scores.Ai(T);
        _AiWinsA[T].store(static_cast<int>(S.Wins), std::memory_order_relaxed);
        _AiLossesA[T].store(static_cast<int>(S.Losses), std::memory_order_relaxed);
        _AiDrawsA[T].store(static_cast<int>(S.Draws), std::memory_order_relaxed);
        _View.SetAiScore(T, static_cast<int>(S.Wins), static_cast<int>(S.Losses),
                         static_cast<int>(S.Draws));
    }

    Rps::LockstepPeer* Lp = &_Lp;
    // #160: the message SET is defined once, in Rps/SessionWiring.h, and shared with the Android and
    // desktop mains plus the test harness. It was four hand-maintained copies, and the copies drifted
    // in exactly the way that hurts most: the dev-only cvar-sync slots were registered on ANDROID
    // ONLY, so an iPhone silently dropped the Android's MsgCvarSync, the peers simulated on different
    // Cv and desynced at the first anchor (#147). An unregistered slot fails with no error at either
    // end, so this is not a place to keep a copy.
    Rps::RouteSessionToPeer(_Host.Session(), _Lp);
    // A reconnect rebases the lockstep timeline — RPS's whole use of the link hooks, and why the
    // host's record-sync half is opt-in: chess's resync means "re-adopt and re-send our record",
    // which has no meaning here.
    Lur::App::GameHost::Hooks Hooks;
    Hooks.OnResync = [Lp] { Lp->BeginResync(); };
    _Host.Start(std::move(Hooks));

    // Initialise the cross-thread surface before the sim thread reads any of it. The zero-defaults
    // (all the sim->main flags, false/0) rely on ObjC's zeroed ivar storage; these three are the ones
    // that need a non-zero start.
    _LastConsumedTick = 0xFFFFFFFFu;
    _SimRunning.store(true, std::memory_order_relaxed);
    // #2: open straight into a match vs the Easy AI (the sim thread consumes this one-shot on its first
    // iteration). The player can pick another tier — or the linked opponent — from the selector at any
    // time. Set BEFORE the thread starts so the very first sim iteration brings a live match up.
    _SoloAiTier.store(static_cast<int>(Rps::EAiTier::Easy), std::memory_order_release);

    // ---- SIM thread (#69/#91 parity): owns Session + Lp + the solo sim; pumps BLE, ticks the sim,
    // publishes snapshots — the datagram-driven service loop OFF the CADisplayLink render cadence. The
    // lambda captures self as a raw pointer (no ARC retain in a C++ closure); the VC lives for the app
    // and -dealloc stops + joins the thread before its C++ ivars are destroyed. ----
    _SimThread = std::thread([self] { [self simThreadLoop]; });

    // ---- RENDER thread (#183): owns the renderer + _View + _Cam + _DevGesture; free-runs the frame loop
    // with FIFO as the one vsync clock (the fix for the #103 40fps beat). It waits for MAIN to publish the
    // first sized layer (viewDidLayoutSubviews) before creating the renderer. Same raw-self capture /
    // stop-before-destroy contract as the sim thread. ----
    // #43 section C: RPS is Dedicated — a real render thread owns the renderer, so parking it means
    // waiting for its ack. Declared once here; every call site below is topology-agnostic.
    _RH.Configure(Lur::App::ERenderTopology::Dedicated);
    _RH.Start();

    // #43 section D. Wired on MAIN before the render thread exists, so the render thread never
    // observes a half-initialised router; after this the object is touched only from there.
    // placeLocal is deliberately the sink — it is what already knows whether the solo inbox or the
    // linked peer is the live one, and that decision stays per-main.
    {
        Rps::TouchRouterHooks Hooks;
        __weak RpsViewController* WeakSelf = self;
        Hooks.Emit = [WeakSelf](const Rps::InputEvent& E) {
            RpsViewController* Me = WeakSelf;
            if (Me != nil) [Me placeLocal:E];
        };
        Hooks.PickAiTier = [WeakSelf](int Tier) {
            RpsViewController* Me = WeakSelf;
            if (Me != nil) Me->_SoloAiTier.store(Tier, std::memory_order_release);
        };
        Hooks.PickPeer = [WeakSelf]() {
            RpsViewController* Me = WeakSelf;
            if (Me != nil) Me->_SwitchToLinkedAtomic.store(true, std::memory_order_release);
        };
        _Router.Init(&_View, &_Cam, std::move(Hooks));
    }
    // pthread (not std::thread) so we can hand it a 4MB stack — see the ivar note. The trampoline just
    // re-enters the ObjC loop; self is passed raw (VC outlives the thread, which -dealloc joins first).
    pthread_attr_t RenderAttr;
    pthread_attr_init(&RenderAttr);
    pthread_attr_setstacksize(&RenderAttr, 4u * 1024u * 1024u);   // 4MB, page-aligned
    _RenderThreadStarted =
        (pthread_create(&_RenderThread, &RenderAttr, &RpsRenderThreadTrampoline, (__bridge void*)self) == 0);
    pthread_attr_destroy(&RenderAttr);
    if (!_RenderThreadStarted)
        os_log_error(OS_LOG_DEFAULT, "OnlyRps: render thread create FAILED #183");

    // #183: MAIN-thread housekeeping tick (no rendering) — the #73 heal, the became-active reattach, the
    // render-health heartbeat and the render_scale A/B. Replaces the per-frame lifecycle work the
    // CADisplayLink renderFrame used to carry. 0.5 s cadence: a recovery/diag loop, not the hot path.
    _LifecycleTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self
                                                     selector:@selector(lifecycleTick)
                                                     userInfo:nil repeats:YES];
}

- (void)dealloc {
    [_LifecycleTimer invalidate];
    // Render thread FIRST — it owns the renderer, which references the layer/surface the OS is about to
    // tear down. Clear the pause flag too, so a parked thread wakes to observe !running and exits its loop.
    _RH.Stop();
    _RH.Resume();  // a parked thread wakes, observes !ShouldRun and exits (Stop alone also breaks it)
    if (_RenderThreadStarted) { pthread_join(_RenderThread, nullptr); _RenderThreadStarted = false; }
    _SimRunning.store(false, std::memory_order_release);  // stop the loop, then wait it out
    if (_SimThread.joinable()) _SimThread.join();
}

// Publish the safe-area insets in DRAWABLE PIXELS for the render thread. MUST be called by every
// site that changes contentsScale or drawableSize, because px insets and a px drawable size are only
// comparable when both came from the same scale (#103 misalignment).
- (void)publishInsetsForScale:(CGFloat)Scale {
    const UIEdgeInsets Sa = self.view.safeAreaInsets;
    _InsetTopPx.store(static_cast<int32_t>(Sa.top * Scale), std::memory_order_relaxed);
    _InsetBotPx.store(static_cast<int32_t>(Sa.bottom * Scale), std::memory_order_relaxed);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    // #183: MAIN only sizes the layer and PUBLISHES drawable-size + safe-area insets for the render thread,
    // which owns the renderer + camera clamp. The renderer is created/resized by the render thread — main
    // never calls a renderer method (the single-thread-owns-the-renderer invariant).
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat Scale = Layer.contentsScale;
    Layer.drawableSize = CGSizeMake(self.view.bounds.size.width * Scale,
                                    self.view.bounds.size.height * Scale);
    if (Layer.drawableSize.width == 0 || Layer.drawableSize.height == 0) return;
    const int DrawW = static_cast<int>(Layer.drawableSize.width);
    const int DrawH = static_cast<int>(Layer.drawableSize.height);
    [self publishInsetsForScale:Scale];

    if (!_RH.HasSurface()) {
        // First valid layout: record the #73 precondition (a renderer born while inactive presents into a
        // layer the window server never composites), publish the layer, then let the render thread Init.
        _InitWhileInactive =
            UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
#if !LUR_SHIPPING
        _AppliedRenderScaleRaw = Rps::Fixed::One;  // published at native scale (k=1.0)
#endif
        _RH.PublishSurface((__bridge void*)Layer, DrawW, DrawH);
        os_log(OS_LOG_DEFAULT, "OnlyRps: layer published %dx%d appActive=%d — render thread will init #183",
               DrawW, DrawH, _InitWhileInactive ? 0 : 1);
    } else {
        _RH.RequestResize(DrawW, DrawH);  // whoever owns the renderer resizes it at a safe point
    }
}

- (void)onBecameActive {
    _BecameActive = true;  // the #73 init-while-inactive reattach is handled by the lifecycle tick
    // Resume a render thread parked on resign/background. If it was inited while inactive, the tick's
    // reattach takes over instead (it re-parks + rebuilds), so don't blindly resume in that case.
    if (_RH.IsReady() && !_InitWhileInactive) _RH.Resume();
}

- (void)onWillResign {
    if (_RH.IsReady()) _RH.RequestPark();
}

#if LUR_INTERNAL
// Open a recording for a match that is (re)starting. Mirrors the Android lambda of the same name,
// including the TIMESTAMPED filename: a per-session counter restarted at 1 every launch and silently
// overwrote the previous session's files. The ordinal stays as a suffix purely to be collision-proof,
// since two matches can start inside one second (re-picking a tier restarts instantly).
- (void)soloRecBegin:(int)Tier {
    _SoloRecFile.clear();
    if (!Rps::CvFlightRecorder.Get()) return;
    const std::time_t Now = std::time(nullptr);
    std::tm Tm{};
    localtime_r(&Now, &Tm);
    char Stamp[24];
    std::strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", &Tm);
    _SoloRecFile = _SaveDir + "/rps-match-" + Stamp + "-" + std::to_string(++_SoloMatchNo) + ".rec";
    _SoloRec.Begin(_SoloRecFile.c_str(), _SoloSim, Tier, /*human*/ 0);
    _RecCensusNs = 0;
    os_log(OS_LOG_DEFAULT, "OnlyRps: REC started -> %{public}s", _SoloRecFile.c_str());
}

// One executed tick, straight from LockstepPeer's sink. Hashes go in on the ANCHOR cadence (every
// 10th tick) — the same cadence the netcode cross-checks on, so both peers' files land hashes on
// identical tick numbers and a diff lines up without interpolation.
- (void)recordLinkedTick:(uint32_t)Tick batch:(const Rps::InputEvent*)Batch count:(int)Count
                    hash:(uint64_t)Hash {
    _LinkedRec.Events(Tick, Batch, Count);
    if (Tick % 10 == 0) _LinkedRec.Hash(Tick, Hash);
}

#if LUR_AGENT
// Apply one assistant remote-control command. Render thread, which on iOS is also the sim thread, so
// these may reach straight into _Lp/_SoloSim. Mirrors ApplyAgentCommand in the Android main verb for
// verb — the grammar is shared (Rps/AgentControl.h) precisely so the two cannot drift, which is the
// mistake this batch has already had to fix three times (#147's cvar slots, #151's gesture, #159's
// recording sentinel — iOS the odd one out every time).
// #170: name the misroute instead of letting it pass. Input produced while the app is still in its
// opening AI match goes to the SOLO sim, so the peer sits at stall=1 waiting for a camp that was never
// sent — and every step of that reports success. Mirrors WarnIfSolo in the Android main.

// #43 section E: the verb table is Rps::AgentCommandRouter now, shared with the Android main and
// host-tested (rps_agent_router_tests, built with LUR_AGENT=1 — the only place this code is
// reachable by a test at all). What is left here is the wiring only this main can supply.
- (void)wireAgentRouter {
    Rps::AgentHooks H;
    __weak RpsViewController* W = self;
    H.Emit = [W](const Rps::InputEvent& E) { RpsViewController* M = W; if (M) [M placeLocal:E]; };
    H.Team = [W]() -> uint8_t {
        RpsViewController* M = W;
        return M ? M->_LinkedTeam.load(std::memory_order_relaxed) : 0;
    };
    H.SoloActive = [W]() {
        RpsViewController* M = W;
        return M ? M->_SoloActiveAtomic.load(std::memory_order_acquire) : false;
    };
    H.PeerReady = [W]() { RpsViewController* M = W; return M ? M->_Host.Session().IsReady() : false; };
    H.SoloSim = [W]() -> Rps::Sim* { RpsViewController* M = W; return M ? &M->_SoloSim : nullptr; };
    // PUBLISHED, not applied: _View belongs to the RENDER thread since #183 and DevOverlayOpen_ is a
    // plain bool. The old code wrote it straight from the sim thread — the same mistake `gesture`
    // three cases below it had already been fixed for.
    H.RequestConsole = [W](bool On) {
        RpsViewController* M = W;
        if (M) M->_AgentConsoleReq.store(On ? 1 : 0, std::memory_order_release);
    };
    H.RequestGesture = [W]() {
        RpsViewController* M = W;
        if (M) M->_AgentGestureRequest.store(true, std::memory_order_release);
    };
    H.RequestLinked = [W]() {
        RpsViewController* M = W;
        if (M) M->_SwitchToLinkedAtomic.store(true, std::memory_order_release);
    };
    _AgentRouter.Init(&_Lp, std::move(H));
}

// Poll the agent channel ~10x/s. Reads the whole (tiny) file each time; the sequence number in the
// command is what makes re-reading a level-triggered channel idempotent.
- (void)pollAgentChannel:(uint64_t)ElapsedNs {
    _AgentPollNs += ElapsedNs;
    if (_AgentPollNs < 100'000'000ull) return;
    _AgentPollNs = 0;
    std::FILE* F = std::fopen(_AgentCmdPath.c_str(), "rb");
    if (F == nullptr) return;
    char Buf[128] = {};
    const std::size_t N = std::fread(Buf, 1, sizeof(Buf) - 1, F);
    std::fclose(F);
    Buf[N] = '\0';
    Rps::AgentCommand Cmd;
    if (_AgentCtl.Poll(Buf, Cmd)) _AgentRouter.Apply(Cmd);
}
#endif  // LUR_AGENT

// Open a recording for the LINKED match that is starting (#159). Same shape as the solo one; the
// name carries "-vs-" so a linked capture is never mistaken for a solo one when both are pulled off
// the device together.
- (void)linkedRecBegin {
    // #180: idempotent. The guard moved in here from the call site when the caller became the
    // match-start EDGE rather than a per-frame poll, so opening twice for one match is now a
    // programming error rather than the normal flow it used to be.
    if (_LinkedRecIdx == _Lp.MatchIndex()) return;
    _LinkedRecFile.clear();
    _LinkedRecIdx = _Lp.MatchIndex();
    if (!Rps::CvFlightRecorder.Get()) return;
    const std::time_t Now = std::time(nullptr);
    std::tm Tm{};
    localtime_r(&Now, &Tm);
    char Stamp[24];
    std::strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", &Tm);
    _LinkedRecFile = _SaveDir + "/rps-vs-" + Stamp + "-" + std::to_string(++_LinkedMatchNo) + ".rec";
    // tier -1 = "not an AI match"; the human team is OUR side, which is what orients the file.
    _LinkedRec.Begin(_LinkedRecFile.c_str(), _Lp.GetSim(), /*tier*/ -1,
                     _LinkedTeam.load(std::memory_order_relaxed));
    os_log(OS_LOG_DEFAULT, "OnlyRps: REC linked -> %{public}s", _LinkedRecFile.c_str());
}
#endif

// #73 heal: the renderer was initialised while the app wasn't active, so its
// CAMetalLayer is bound to a window-server surface that is never composited —
// presents succeed, the screen stays black, and nothing errors. A swapchain (or even
// VkSurfaceKHR) recreate against the SAME layer cannot fix that (proven by 898999b),
// so on the first activation we rebuild the whole chain against the now-live window
// server: fresh UIWindow + fresh view/CAMetalLayer + full renderer Shutdown/Init.
//
// #43 section B took the UIKit half into LurRebuildViewHost; section C took the rest of the sequence into
// LurReattachRenderHost, including the park that used to be the reason the two games could not share it.
// What stays here is genuinely per-app: RPS holds the retiring view (its render thread destroys the old
// VkSurfaceKHR after this returns), republishes safe-area insets, and re-arms the #103 render scale.
- (void)reattachForActivation {
    // #43 section C: the scene check, the park + ack wait, the outgoing-view grab, the rebuild and the
    // arm/release are the engine's now — chess's copy of this sequence was the same one. The returned
    // retiring view is the OLD view whose CAMetalLayer the old VkSurfaceKHR wraps; RPS must hold it,
    // because vkDestroySurfaceKHR runs on the render thread AFTER this method returns. Released on the
    // reattach-done ack in lifecycleTick, on MAIN, so the UIView still deallocs on the main thread.
    UIView* Retiring = LurReattachRenderHost(self, LurMetalView.class, _RH);
    if (Retiring == nil) return;   // too early (no scene) — the tick retries; the park was released for us
    _RetiringView = Retiring;

    // Per-app work the engine has no business knowing about. Note this now runs AFTER the reinit is armed
    // rather than before: the render thread's Shutdown+Init takes far longer than these three stores, so
    // the new insets land first in practice, and one frame at the old insets is cosmetic either way.
    LurMetalView* NewView = (LurMetalView*)self.view;
    CAMetalLayer* Layer = (CAMetalLayer*)NewView.layer;
    [self publishInsetsForScale:Layer.contentsScale];
    _InitWhileInactive =
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
#if !LUR_SHIPPING
    _AppliedRenderScaleRaw = Rps::Fixed::One;  // #103: rebuilt at native scale; the tick re-applies any override
#endif
    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: handed reinit to the render thread (drawable %dx%d, appActive=%d)",
           (int)Layer.drawableSize.width, (int)Layer.drawableSize.height, _InitWhileInactive ? 0 : 1);
}

#if !LUR_SHIPPING
// #103: apply rps.dev.render_scale by rescaling the CAMetalLayer's backing store (MAIN, UIKit) and asking
// the render thread to recreate the swapchain — but ONLY when the value actually changed (a swapchain
// recreate is not free). Setting contentsScale AND drawableSize together keeps the render extent and the
// touch-point mapping (touchesBegan/Moved/Ended read contentsScale) in the same coordinate space. Called
// from the MAIN lifecycle tick; the render thread owns the renderer, so RequestResize hands it the size.
- (void)applyRenderScaleIfChanged {
    if (!_RH.IsReady()) return;
    const int32_t Raw = CvRenderScale.Get().Raw;
    if (Raw == _AppliedRenderScaleRaw) return;
    const float K = static_cast<float>(Raw) / static_cast<float>(Rps::Fixed::One);
    const CGFloat Eff = UIScreen.mainScreen.scale * K;   // native retina * multiplier
    CAMetalLayer* Layer = [self metalLayer];
    Layer.contentsScale = Eff;
    Layer.drawableSize = CGSizeMake(self.view.bounds.size.width * Eff,
                                    self.view.bounds.size.height * Eff);
    if (Layer.drawableSize.width == 0 || Layer.drawableSize.height == 0) return;  // not laid out yet — retry next tick
    // Republish the insets at the NEW scale, in the same breath as the new drawable size. Changing
    // contentsScale does NOT trigger viewDidLayoutSubviews, so without this the HUD stays laid out
    // against the previous scale's px insets until something else happens to re-lay-out the view —
    // which at 0.5 is insets twice as tall as they should be, squeezing the world view.
    [self publishInsetsForScale:Eff];
    _RH.RequestResize(static_cast<int>(Layer.drawableSize.width),
                      static_cast<int>(Layer.drawableSize.height));   // swapchain recreated at a safe point
    _AppliedRenderScaleRaw = Raw;
    os_log(OS_LOG_DEFAULT, "OnlyRps: render_scale -> %.3f (drawable %dx%d) #103", K,
           (int)Layer.drawableSize.width, (int)Layer.drawableSize.height);
}
#endif

// #183: the RENDER thread. Owns the renderer + _View + _Cam + _DevGesture + _Snap; free-runs the frame
// loop with FIFO as the single vsync clock (no CADisplayLink). Startup: wait for MAIN to publish the
// first sized layer, then create + Init the renderer HERE (so all Vulkan is single-threaded). Each
// iteration is wrapped in its OWN @autoreleasepool — this is a raw pthread (#183: for a 4MB stack), not
// an NSThread, so nothing drains the CAMetalDrawable MoltenVK retains per frame otherwise. Raw-self
// capture / stop-before-destroy contract identical to simThreadLoop.
- (void)renderThreadLoop {
    while (_RH.ShouldRun() && !_RH.HasSurface())
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    if (!_RH.ShouldRun()) return;
    @autoreleasepool {
        _Renderer = Lur::Render::VulkanRenderer::Create("OnlyRps");
        const bool Ok = _Renderer && _Renderer->Init(_RH.Surface());
        _RH.SetReady(Ok);
        os_log(OS_LOG_DEFAULT, "OnlyRps: Renderer init: %{public}s (drawable %dx%d) [render thread] #183",
               Ok ? "ok" : "failed", _RH.Width(), _RH.Height());
        if (Ok) _View.CreateResources(_Renderer);
    }
    double PrevTime = 0.0;
    while (_RH.ShouldRun()) {
        // ---- Park / reattach handshake. MAIN parks us (background OR #73 reattach); we ack at this SAFE
        // point (no drawable held, renderer idle) and spin until released. TakeWork withholds a pending
        // reinit for exactly as long as the park stands, so on release we get it against the NEW surface —
        // do the Vulkan Shutdown/Init HERE (renderer stays single-threaded), then signal done so MAIN can
        // release the retiring view. ----
        // ONE TakeWork per iteration: it CONSUMES, so calling it twice (or as a bare predicate) silently
        // drops whatever the second call would have returned. That is how a rotation goes missing and the
        // app comes back letterboxed at the old swapchain size.
        Lur::App::RenderWork Work = _RH.TakeWork();
        if (Work.Park) {
            _RH.AckParked(true);
            while (_RH.ParkRequested() && _RH.ShouldRun())
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            _RH.AckParked(false);
            if (!_RH.ShouldRun()) break;
            Work = _RH.TakeWork();   // now released: the reinit (and any resize raised meanwhile) is ours
        }
        // Deliberately OUTSIDE the park branch. MAIN proceeds with the rebuild anyway if we fail to park
        // within its 1 s cap, and on that path the reinit arrives with no park in front of it — handled
        // here, it is applied; handled inside the branch, TakeWork would consume and discard it and the
        // renderer would keep drawing into the dead layer the heal was meant to replace.
        if (Work.Reinit) {
            @autoreleasepool {
                _RH.SetReady(false);
                if (_Renderer) {
                    _Renderer->Shutdown();  // full teardown (device, surface, everything) of the old layer
                    const bool Ok = _Renderer->Init(Work.Surface);
                    _RH.SetReady(Ok);
                    if (Ok) _View.CreateResources(_Renderer);
                    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: re-init %{public}s (drawable %dx%d) [render thread]",
                           Ok ? "ok" : "FAILED", Work.W, Work.H);
                } else {
                    os_log_error(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: no renderer to re-init [render thread]");
                }
            }
            _RH.SignalReinitDone();  // MAIN releases _RetiringView — even on failure; it owns a dead view
            PrevTime = 0.0;
            continue;                // Init already built the swapchain at Work.W/H
        }
        if (!_RH.IsReady()) {  // init failed / mid-reinit — idle, don't spin hot
            // Re-arm anything we consumed but cannot apply. TakeWork already took it, and dropping it
            // here would mean a rotation during a failed init never reaches the swapchain — the app comes
            // back at the wrong size with nothing logged. (Before section C this was accidentally safe:
            // the flag was only exchanged inside renderOneFrame, which this path never reached.)
            if (Work.Resize) _RH.RequestResize(Work.W, Work.H);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        @autoreleasepool { [self renderOneFrame:&PrevTime withWork:Work]; }
    }
}

// One free-running render iteration. RENDER thread only; reads drawable size + insets from the atomics
// MAIN publishes (never the layer directly), and touches _View/_Cam/_DevGesture/_Snap/_Renderer freely
// because it is their sole owner.
- (void)renderOneFrame:(double*)PrevTimePtr withWork:(const Lur::App::RenderWork&)Work {
    const double Now = CACurrentMediaTime();
    const uint64_t ElapsedNs = *PrevTimePtr > 0.0 ? static_cast<uint64_t>((Now - *PrevTimePtr) * 1e9) : 0;
    *PrevTimePtr = Now;

    // #103 render_scale (or a rotation/layout): MAIN resized the layer + published the new drawable size;
    // recreate the swapchain here, where we own the renderer.
    if (Work.Resize) _Renderer->Resize(Work.W, Work.H);

    // Drain the touch queue and replay the exact hit-test/input logic (was in the UIKit handlers pre-#183)
    // at the TOP of the frame, so a placement/tap carries ≤1 frame of added latency.
    [self drainTouches];

#if LUR_AGENT
    // Agent `console`: applied HERE, on the thread that owns _View. The sim thread only publishes the
    // request (#43 section E) — it used to write DevOverlayOpen_, a plain bool, across the boundary.
    if (const int Want = _AgentConsoleReq.exchange(-1, std::memory_order_acquire); Want >= 0)
        _View.SetDevOverlayOpen(Want != 0);
    // Agent `gesture`: feed the SHARED recognizer a synthetic two-finger triple-tap. Now on the RENDER
    // thread (it owns _DevGesture); the sim thread hands the request across via the atomic. Mirrors the
    // Android glue thread; unchanged effect, just relocated with the rest of the input state.
    if (_AgentGestureRequest.exchange(false, std::memory_order_acquire)) {
        const uint64_t T0 = NowNs();
        bool Opened = false;
        for (int Tap = 0; Tap < Rps::AgentGestureTaps; ++Tap) {
            const uint64_t Down = T0 + static_cast<uint64_t>(Tap) * 200'000'000ull;
            // The console's own routing, so the agent path and a real finger share one code path.
            Lur::DevGui::Console& C = _View.DevConsole();
            (void)C.PointerDown(1, 0.0f, 0.0f, Down);
            (void)C.PointerDown(2, 0.0f, 0.0f, Down);
            (void)C.PointerUp(0.0f, 0.0f, Down + 40'000'000ull);
            Opened = _View.DevOverlayOpen();
        }
        os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT gesture -> recognizer says open=%d (console now %d)",
               Opened ? 1 : 0, _View.DevOverlayOpen() ? 1 : 0);
        if (Opened) _View.SetDevOverlayOpen(true);
    }
#endif

    // #103: split GPU-WAIT vs CPU work for the TRACE line. The vsync fence+acquire idle now happens on THIS
    // thread — so frame N+1's CPU work (below) overlaps frame N's drawable wait, which is the whole fix for
    // the 40fps beat. render.view then measures only command recording + submit + present.
    { LUR_TRACE_SCOPE("gpu.wait"); _Renderer->WaitForFrame(); }
    LUR_TRACE_SCOPE("frame.render");  // whole-frame CPU cost from here to return (nests render.view)

    // ---- Reflect the sim thread's published view-state into the HUD. The heavy lifting — session pump,
    // solo/linked auto-switch, the sim ticks, and scoring — is on the sim thread; this loop only applies
    // the atomics it published and renders. ----
    // #2: the Linked-opponent ROW + "opponent link established" blink appear when a real PEER connects.
    // Fire once on the rising edge. GetPeerGuid is set once at handshake, so reading it here on the render
    // thread (after the acquire load establishes happens-before) is safe even though Session is sim-owned.
    if (!_ViewLinkedApplied && _PeerLinkedAtomic.load(std::memory_order_acquire)) {
        _View.SetLinked(true, _Host.Session().GetPeerGuid());   // label the row with the peer's id (#178)
        _View.NotifyPeerLinked();                        // blink the bar
        _ViewLinkedApplied = true;
    }
    // Every frame, not just the link edge: a mismatch is discovered when the peer's fingerprint ARRIVES
    // (which can be after link-up) and clears on reinstall. BuildMismatch() is a monotonic bool the sim
    // thread sets; reading it cross-thread is the same benign read Android's glue does (SetBuildMismatch),
    // and the setter early-outs when unchanged.
    _View.SetBuildMismatch(_Lp.BuildMismatch());
    // The sim switched us to the peer -> point the selector at that row (gated on the row existing, so
    // the flag is never consumed before SetLinked ran).
    if (_ViewLinkedApplied && _SelectLinkedRow.exchange(false, std::memory_order_acq_rel))
        _View.SelectLinkedOpponent();
    // Per-opponent session scores (no-op when unchanged, so the list only rebuilds on a real change).
    for (int T = 0; T < Rps::AiTierCount; ++T)
        _View.SetAiScore(T, _AiWinsA[T].load(std::memory_order_relaxed),
                         _AiLossesA[T].load(std::memory_order_relaxed),
                         _AiDrawsA[T].load(std::memory_order_relaxed));
    _View.SetPeerScore(_PeerWinsA.load(std::memory_order_relaxed),
                       _PeerLossesA.load(std::memory_order_relaxed),
                       _PeerDrawsA.load(std::memory_order_relaxed));
    _View.SetRecovering(_Recovering.load(std::memory_order_relaxed));     // #161 "resyncing with opponent"
    _View.SetLinkHalfOpen(_LinkHalfOpen.load(std::memory_order_relaxed)); // #163 "LINK STALLED" banner

    // Consume the latest published snapshot, but only when the tick CHANGED — between ticks the held
    // snapshot is re-rendered with a fresh interpolation alpha, so there's nothing new to copy. Before
    // the first publish, Consume returns false and _Snap stays the default (empty) sim = the menu.
    const uint32_t Pub = _PublishedTick.load(std::memory_order_acquire);
    if (Pub != _LastConsumedTick && _Mailbox.Consume(_Snap)) {
        _LastConsumedTick = Pub;
        // RE-ANCHOR interpolation to the RENDER clock. The fixed-timestep tween ramps alpha 0->1 over one
        // 100 ms tick; anchoring that ramp to the sim thread's own PublishNs was choppy (that timestamp
        // carries the sim thread's ~2 ms-poll + heavy-tick jitter). Stamping "first seen" with render-thread
        // time here makes every tick start at alpha~0 on the frame it lands, so the ramp is smooth and
        // render-locked. Visual only: PublishNs never crosses the determinism boundary.
        _Snap.PublishNs = NowNs();
    }

    const float W = static_cast<float>(_RH.Width());
    const float H = static_cast<float>(_RH.Height());
    const uint64_t Stamp = NowNs();
    const uint8_t MyTeam = _LinkedTeam.load(std::memory_order_relaxed);
    const float VisibleH = H / Ppu(W);
    const float FieldMax = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
    // OS safe areas (#85 feedback): notch/status bar above the HUD, home indicator below the plates.
    // MAIN reads self.view.safeAreaInsets (main-only) and publishes them in px; we consume the atomics.
    _View.SetInsets(static_cast<float>(_InsetTopPx.load(std::memory_order_relaxed)),
                    static_cast<float>(_InsetBotPx.load(std::memory_order_relaxed)));
    const float MaxCam = FieldMax + _View.TopHudWorldUnits(W);
    const float MinCam = -_View.BottomHudWorldUnits(W);
    if (!_CamInit) { _Cam.Y = MinCam; _CamInit = true; }
    // Camera LOCKED at the baseline until you place your first mining camp (feedback) — free scroll
    // after. Over the CONSUMED snapshot; a fresh match (no camp) re-locks the view here regardless.
    if (!_Snap.HasMinerCamp(MyTeam)) _Cam.Y = MinCam;
    else _Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam, MinCam);  // momentum + clamp
    // #139/feedback: your camp, committed and waiting on the opponent's — it is NOT in the sim yet
    // (both camps become tick 0's input together), so without this the field looks empty right after
    // the drop. Published by the SIM thread via the atomics (never set in solo: there the place applies
    // immediately and the real camp is already in the snapshot).
    {
        const bool Pend = _PendingCampAtomic.load(std::memory_order_acquire);
        constexpr float FixedOne = static_cast<float>(Rps::Fixed::One);
        _View.SetPendingCamp(Pend,
                             static_cast<float>(_PendingCampX.load(std::memory_order_relaxed)) / FixedOne,
                             static_cast<float>(_PendingCampY.load(std::memory_order_relaxed)) / FixedOne);
    }
    {
        LUR_TRACE_SCOPE("render.view");  // #103: BeginFrame..EndFrame — CPU command recording + present
        _View.Render(_Renderer, _Snap, _Snap.AlphaAt(Stamp), _Cam.Y, W, H, MyTeam == 1,
                     static_cast<float>(ElapsedNs) / 1.0e9f);
    }
    // render -> sim/main: the presented-frame count for the LOCKSTEP diag + the MAIN heartbeat (neither of
    // those threads may touch the renderer).
    _PresentedFrames.store(_Renderer->PresentedFrames(), std::memory_order_relaxed);
}

// Drain the MAIN->RENDER touch queue and replay the hit-test logic. swap-on-drain under the lock keeps the
// critical section tiny (the actual replay runs unlocked). RENDER thread.
- (void)drainTouches {
    std::vector<TouchEvent> Batch;
    {
        std::lock_guard<std::mutex> Lk(_TouchMx);
        _TouchQ.swap(Batch);
    }
    Rps::TouchFrame F;
    F.ViewW = static_cast<float>(_RH.Width());
    F.ViewH = static_cast<float>(_RH.Height());
    F.Team  = _LinkedTeam.load(std::memory_order_relaxed);   // sim thread publishes it
    F.Live  = _MatchLive.load(std::memory_order_acquire);    // solo OR peer match live
    for (const TouchEvent& E : Batch) _Router.Route(E, _Snap, F);
}

// #183: the MAIN-thread housekeeping tick (0.5 s). Everything that reads UIKit — window/scene/appState —
// lives HERE, off the render thread: the #73 heal (became-active + persistent orphan), the render-health
// heartbeat, the render_scale A/B apply + its LUR_AGENT autopilot, and releasing the retiring view once the
// render thread finished its Shutdown/Init. No rendering happens on main anymore.
- (void)lifecycleTick {
    // #73 became-active reattach: the renderer was inited while inactive, so its layer is bound to a
    // window-server surface that is never composited. Rebuild against the live window server.
    if (_BecameActive) {
        _BecameActive = false;
        if (_RH.IsReady() && _InitWhileInactive) [self reattachForActivation];
    }
    // #73 persistent orphan: after a DVT relaunch the view can sit in no window / no scene while we think
    // we are ready (presents "succeed" into the orphan layer; screen black). Heal, retried ~2 s.
    static int OrphanTicks = 0;
    if (_RH.IsReady() &&
        (self.view.window == nil || self.view.window.windowScene == nil)) {
        if (++OrphanTicks >= 4) { OrphanTicks = 0; [self reattachForActivation]; }  // ~2 s at 0.5 s/tick
    } else {
        OrphanTicks = 0;
    }
    // Release the old view/layer once the render thread finished Shutdown/Init on it — on MAIN, so the
    // UIView deallocs on the main thread.
    if (_RH.TakeReattachDone()) _RetiringView = nil;

#if LUR_AGENT
    // #103 A/B AUTOPILOT: iOS has no touch injection and no on-device cvars.cfg, so the render_scale sweep
    // can't be driven by hand headlessly. Cycle it here — one agent run yields a labelled fps/TRACE sample
    // per scale (applyRenderScaleIfChanged logs "render_scale -> X"; bucket the TRACE/HEARTBEAT lines that
    // follow until the next such line). LUR_AGENT, not LUR_INTERNAL: auto-forcing render state acts for the
    // player, so it is absent from any handed-over build.
    {
        static const char* const kScaleSweep[] = {"1", "0.7", "0.5"};
        static int SweepIdx   = 0;
        static int SweepTicks = 0;
        if (++SweepTicks >= 24) {  // ~12 s at 0.5 s/tick — ~6 TRACE windows to average over
            SweepTicks = 0;
            SweepIdx = (SweepIdx + 1) % 3;
            CvRenderScale.SetFromString(kScaleSweep[SweepIdx]);  // applyRenderScaleIfChanged picks it up
        }
    }
#endif
#if !LUR_SHIPPING
    [self applyRenderScaleIfChanged];  // #103: pick up a rps.dev.render_scale edit
#endif

#if LUR_INTERNAL
    // Always-on render-health heartbeat (#73) — NOT gated on the link. Reads window/scene/appState (main);
    // the presented count comes from the atomic the render thread publishes (main no longer touches the
    // renderer). The lockstep DIAG + #69 TRACE lines live on the sim thread.
    static int BeatTicks = 0;
    if (++BeatTicks >= 4) {  // ~2 s
        BeatTicks = 0;
        // win/key/scene/host: hunting an in-process signal for the never-composited state (#73). scene:
        // UISceneActivationState (0=fg-active). host: root layer parented into the window's layer tree.
        UIWindow* Win = self.view.window;
        const bool LinkedLive = _MatchLive.load(std::memory_order_relaxed) &&
                                !_SoloActiveAtomic.load(std::memory_order_relaxed);
        os_log(OS_LOG_DEFAULT,
               "OnlyRps: HEARTBEAT presented=%u appActive=%d linked=%d win=%d key=%d scene=%ld "
               "host=%d scenes=%lu",
               _PresentedFrames.load(std::memory_order_relaxed),
               UIApplication.sharedApplication.applicationState == UIApplicationStateActive ? 1 : 0,
               LinkedLive ? 1 : 0, Win != nil ? 1 : 0, Win.isKeyWindow ? 1 : 0,
               (long)(Win.windowScene != nil ? Win.windowScene.activationState : -99),
               self.view.layer.superlayer != nil ? 1 : 0,
               (unsigned long)UIApplication.sharedApplication.connectedScenes.count);
    }
#endif
}

// #69: the SIM/NET thread. Owns Session + Lp + the solo sim; pumps BLE, ticks the sim ~500 Hz, and
// publishes a Snapshot per tick — the datagram-driven service loop OFF the CADisplayLink render
// cadence. A direct port of RpsMain.cpp's SimThread lambda; all the ivars it touches are sim-owned
// after this thread starts (see the partition in the ivar block). The ONLY cross-thread traffic is the
// Mailbox (snapshot out), the SoloIn inbox + Lp.QueueLocalEvent (input in), and the atomics.
- (void)simThreadLoop {
    auto PrevTime = std::chrono::steady_clock::now();
    uint32_t LastPubTick = 0xFFFFFFFFu;
    // Solo (sim-thread) match state — locals, exactly like Android's lambda locals.
    int      SoloTier = -1;
    bool     SoloScored = false;
    uint64_t SoloAccumNs = 0;
    uint64_t SoloPostNs = 0;
    // Linked (sim-thread) match state.
    bool     Started = false;
    bool     Scored = false;
    uint32_t ScoredIdx = 0xFFFFFFFFu;
    bool     PeerEverReady = false;
    bool     PrevPeerReady = false;
#if LUR_INTERNAL
    uint64_t DiagAccumNs = 0;
#endif
    while (_SimRunning.load(std::memory_order_acquire)) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;
#if LUR_AGENT
        // Poll the agent channel FIRST, unconditionally — it must be serviced whatever mode the app is
        // in, and this is the only thread allowed to touch _Lp/_SoloSim (see RpsMain.cpp for why the
        // poll belongs at the TOP of the loop).
        [self pollAgentChannel:ElapsedNs];
#endif
#if LUR_INTERNAL
        // The lockstep/convergence DIAG + the #69 TRACE line, at the TOP (they read sim state). SOLO
        // counts too: the TRACE line rides on this, and solo is the mode you want for a clean perf
        // measurement (no peer to perturb ble.toApply).
        const bool SoloDiag = _SoloActiveAtomic.load(std::memory_order_acquire);
        if (Started || SoloDiag) {
            DiagAccumNs += ElapsedNs;
            if (DiagAccumNs > 2'000'000'000ull) {
                DiagAccumNs = 0;
                const Rps::Sim& DS = SoloDiag ? _SoloSim : _Lp.GetSim();
                const uint8_t Me = _LinkedTeam.load(std::memory_order_relaxed);
                // #204: `desync` is the STICKY per-match count of detected anchor mismatches — the same
                // on both phones — and `dsgate` is the live gate, which the recovery SURVIVOR clears at
                // once. Printing only the gate made one real divergence read as 1 here and 0 there.
                // Keep this identical to the Android main: these two lines have drifted before.
                os_log(OS_LOG_DEFAULT, "OnlyRps: %{public}s tick=%u you=%d foe=%d desync=%u dsgate=%d "
                       "badbuild=%d "
                       "presented=%u hash=%08x gold=%d frontier=%d started=%d gaps=%d gapat=%u stall=%d "
                       "halfopen=%d restarts=%d rollbk=%d resim=%u spec=%lld",
                       SoloDiag ? "SOLO" : "LOCKSTEP", DS.Tick, DS.AliveCount(0), DS.AliveCount(1),
                       SoloDiag ? 0u : _Lp.DesyncsSeen(),
                       (!SoloDiag && _Lp.Desynced()) ? 1 : 0, _Lp.BuildMismatch() ? 1 : 0,
                       _PresentedFrames.load(std::memory_order_relaxed),
                       static_cast<uint32_t>(DS.StateHash() & 0xFFFFFFFFu),
                       DS.Teams[SoloDiag ? 0 : Me].Gold, DS.FrontierT0.ToInt(),
                       SoloDiag ? 1 : (_Lp.MatchStarted() ? 1 : 0),
                       SoloDiag ? 0 : _Lp.InputGaps(), SoloDiag ? 0u : _Lp.LastInputGapTick(),
                       (!SoloDiag && _Lp.PreMatchStalled()) ? 1 : 0,
                       (!SoloDiag && _Host.Session().IsLinkHalfOpen()) ? 1 : 0,
                       SoloDiag ? 0 : _Host.Session().RadioRestartsAttempted(),
                       // Rollback (Docs/Journal/2026-08-03): rewind+resim frequency, ticks re-simulated,
                       // and the head's speculation depth past the confirmed frontier — §correction-cost.
                       SoloDiag ? 0 : _Lp.Rollbacks(), SoloDiag ? 0u : _Lp.ResimTicks(),
                       SoloDiag ? 0LL : static_cast<long long>(static_cast<int64_t>(_Lp.ExecTick()) -
                                                               _Lp.ConfirmedTick()));
                // #69: the perf TRACE line. ble.toApply — datagram-in-EventInbox to drained/applied —
                // is now serviced on THIS ~500 Hz thread, so it should read ~1-2 ms instead of the old
                // ~5-6 ms render-gate. FormatLineAndReset drains what the netcode recorded this window.
                char TraceLine[512];
                if (Lur::Trace::FormatLineAndReset(TraceLine, sizeof(TraceLine)) > 0)
                    os_log(OS_LOG_DEFAULT, "OnlyRps: TRACE %{public}s", TraceLine);
                // #159: the linked recording's periodic census — carries the economy snapshot AND
                // FLUSHES the file (else the capture sits in the stdio buffer until End).
                if (!SoloDiag) _LinkedRec.Census(_Lp.GetSim(), Me, -1, -1);
            }
        }
#endif
        // #2/#127: consume a selector tier pick -> (re)start a solo AI match at once (even mid-match).
        // App-open stored Easy, so a live match comes up on the very first iteration.
        const int NewTier = _SoloAiTier.exchange(-1, std::memory_order_acq_rel);
        if (NewTier >= 0) {
            _SoloSim.Init(kMatchSeed);
            _SoloAi.Init(kMatchSeed, /*AI team*/ 1, static_cast<Rps::EAiTier>(NewTier));
            SoloTier = NewTier; SoloScored = false; SoloPostNs = 0; SoloAccumNs = 0;
            LastPubTick = 0xFFFFFFFFu;
            Started = false;                                       // drop any linked match — solo takes over
            _SoloActiveAtomic.store(true, std::memory_order_release);
            _LinkedTeam.store(0, std::memory_order_relaxed);       // you are team 0 (no view flip)
            _MatchLive.store(true, std::memory_order_release);
            os_log(OS_LOG_DEFAULT, "OnlyRps: solo AI match (re)started (tier %d)", NewTier);
#if LUR_INTERNAL
            [self soloRecBegin:NewTier];
#endif
        }

        // Pump the session ALWAYS (even during solo) so a real peer can complete the handshake — that
        // raises the "opponent link established" notice + the Linked-opponent row.
        _Host.Tick(ElapsedNs);
        const bool PeerReady = _Host.Session().IsReady();
        if (PeerReady && !PeerEverReady) {
            PeerEverReady = true;
            _PeerLinkedAtomic.store(true, std::memory_order_release);   // main: View.SetLinked + blink
        }
        // AUTO-switch solo -> linked on the link-established EDGE, ONLY out of an unstarted AI match.
        // (Full rationale in RpsMain.cpp.) Never out of a started AI match or a linked match.
        const bool LinkEdge = PeerReady && !PrevPeerReady;
        PrevPeerReady = PeerReady;
        // On the link edge the peer's GUID is finally known, so publish the ALL-TIME record vs THIS
        // rival (else the row would read 0-0-0 until the session's first match ended).
        if (LinkEdge) {
            const Rps::Tally T = _Scores.Peer(_Host.Session().GetPeerGuid(), _DeviceId);
            _PeerWinsA.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
            _PeerLossesA.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
            _PeerDrawsA.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
        }
        const bool ManualPick = _SwitchToLinkedAtomic.load(std::memory_order_acquire);
        const bool AutoSwitch = LinkEdge && _SoloSim.IsPreMatch();   // unstarted AI match only
        const bool SoloActiveNow = _SoloActiveAtomic.load(std::memory_order_acquire);
        if (SoloActiveNow && PeerReady && (AutoSwitch || ManualPick)) {
            _SoloActiveAtomic.store(false, std::memory_order_release);
            _SwitchToLinkedAtomic.store(false, std::memory_order_release);
            _SelectLinkedRow.store(true, std::memory_order_release);     // main: name the peer in the HUD
            os_log(OS_LOG_DEFAULT, "OnlyRps: switch solo -> linked (%{public}s)",
                   AutoSwitch ? "auto: link established, AI match not started"
                              : "player picked the linked opponent");
        } else if (ManualPick && !SoloActiveNow) {
            _SwitchToLinkedAtomic.store(false, std::memory_order_release);   // already linked: moot
        }

        if (_SoloActiveAtomic.load(std::memory_order_acquire)) {
            // ---- SOLO path (#139/#149 pre-match hold; parity with the single-threaded original) ----
            if (_SoloSim.Result == Rps::ResultOngoing && _SoloSim.IsPreMatch()) {
                SoloAccumNs = 0;
                Rps::InputEvent Evs[Rps::MaxEventsPerTick];
                const int Drained = _SoloIn.Drain(Evs, Rps::MaxEventsPerTick);
                int Kept = 0;
                for (int I = 0; I < Drained; ++I)
                    if (Evs[I].Kind == Rps::EventPlaceBuilding && Evs[I].Type == Rps::UnitMiner &&
                        _SoloSim.CanPlaceBuilding(0, Rps::UnitMiner, Rps::Fixed{Evs[I].X},
                                                  Rps::Fixed{Evs[I].Y}))
                        Evs[Kept++] = Evs[I];
                if (Kept > 0) {
                    int AiCount = 0;
                    _SoloAi.DecideEvents(_SoloSim, _SoloSim.Tick, Evs + Kept,
                                         Rps::MaxEventsPerTick - Kept, AiCount);
                    _SoloSim.StepEvents(Evs, Kept + AiCount);
#if LUR_INTERNAL
                    _SoloRec.Events(_SoloSim.Tick - 1, Evs, Kept + AiCount);
#endif
                }
            } else {
                SoloAccumNs += ElapsedNs;
                while (SoloAccumNs >= kStepNs) {   // fixed 10 Hz, decoupled from this ~500 Hz loop
                    SoloAccumNs -= kStepNs;
                    Rps::InputEvent Evs[Rps::MaxEventsPerTick];
                    int Count = _SoloIn.Drain(Evs, Rps::MaxEventsPerTick);
                    {
                        int AiCount = 0;
                        _SoloAi.DecideEvents(_SoloSim, _SoloSim.Tick, Evs + Count,
                                             Rps::MaxEventsPerTick - Count, AiCount);
                        Count += AiCount;
                    }
                    _SoloSim.StepEvents(Evs, Count);
#if LUR_INTERNAL
                    _SoloRec.Events(_SoloSim.Tick - 1, Evs, Count);
#endif
                }
#if LUR_INTERNAL
                _RecCensusNs += ElapsedNs;
                if (_RecCensusNs >= 2'000'000'000ull) {
                    _RecCensusNs = 0;
                    _SoloRec.Census(_SoloSim, /*human*/ 0, static_cast<int>(_SoloAi.State()),
                                    static_cast<int>(_SoloAi.CounterEnemy()));
                }
#endif
            }
            if (!SoloScored && _SoloSim.Result != Rps::ResultOngoing && SoloTier >= 0) {
                SoloScored = true;
#if LUR_INTERNAL
                if (_SoloRec.IsOpen()) {
                    _SoloRec.Census(_SoloSim, 0, static_cast<int>(_SoloAi.State()),
                                    static_cast<int>(_SoloAi.CounterEnemy()));
                    _SoloRec.End(_SoloSim);
                    os_log(OS_LOG_DEFAULT, "OnlyRps: REC match finished: result=%u tick=%u -> %{public}s",
                           static_cast<unsigned>(_SoloSim.Result), _SoloSim.Tick, _SoloRecFile.c_str());
                }
#endif
                _Scores.RecordAi(SoloTier, _SoloSim.Result, /*MyTeam*/ 0);
                _Scores.Save(*_Store);
                const Rps::Tally T = _Scores.Ai(SoloTier);
                _AiWinsA[SoloTier].store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                _AiLossesA[SoloTier].store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                _AiDrawsA[SoloTier].store(static_cast<int>(T.Draws), std::memory_order_relaxed);
            }
            if (_SoloSim.Result != Rps::ResultOngoing) {
                SoloPostNs += ElapsedNs;
                if (SoloPostNs >= Rps::PostMatchHoldNs && SoloTier >= 0) {
                    const uint64_t NextSeed = _SoloSim.Seed + 1;
                    _SoloSim.Init(NextSeed);
                    _SoloAi.Init(NextSeed, /*AI team*/ 1, static_cast<Rps::EAiTier>(SoloTier));
                    SoloScored = false; SoloPostNs = 0; SoloAccumNs = 0;
                    os_log(OS_LOG_DEFAULT, "OnlyRps: solo next match begins (tier %d)", SoloTier);
#if LUR_INTERNAL
                    [self soloRecBegin:SoloTier];
#endif
                }
            } else {
                SoloPostNs = 0;
            }
            // Publish the solo tick to the render thread (it draws from the mailbox now). Covers the
            // fresh (tick 0) sim on (re)start/rebuild too, since 0 != LastPubTick after a finished match.
            if (_SoloSim.Tick != LastPubTick) {
                LastPubTick = _SoloSim.Tick;
                _Mailbox.Back().CaptureFrom(_SoloSim, NowNs(), kStepNs);
                _Mailbox.Publish();
                _PublishedTick.store(_SoloSim.Tick, std::memory_order_release);
            }
            _Recovering.store(false, std::memory_order_relaxed);
            _LinkHalfOpen.store(false, std::memory_order_relaxed);
            _PendingCampAtomic.store(false, std::memory_order_release);
        } else {
            // ---- LINKED path ----
            if (!Started && PeerReady) {
                const uint8_t Team = _DeviceId < _Host.Session().GetPeerGuid() ? 0 : 1;
                _LinkedTeam.store(Team, std::memory_order_relaxed);
                _Lp.Init(kMatchSeed, Team, SendViaSession, &_Host.Session());
#if LUR_INTERNAL
                // #147/#112: refuse a mismatched build, exchange our gameplay-CVar override set so both
                // peers converge on ONE merged set before tick 0. iOS has no on-device cvars.cfg today,
                // so the seed loop is normally a no-op — but the SEND is still this peer's half of the
                // exchange, and the loop is in place for the day iOS gets on-device tuning.
                Rps::LockstepPeer* Lp = &_Lp;
                Lp->SendFingerprint();
                Lur::Core::CVarRegistry::ForEach([Lp](Lur::Core::ICVar* C) {
                    if (!C->AffectsGameplay() || !C->Overridden()) return;
                    const int Id = Rps::GameplayIdForName(C->Name());
                    if (Id >= 0) Lp->SeedGameplayCvar(static_cast<uint8_t>(Id), C->RawValue(),
                                                      C->EditWallMs());
                });
                Lp->SendCvarSync();
#endif
                // #148: reconcile on ENTERING the match, not only on a reconnect edge (a freshly
                // launched app never takes that edge). Harmless for a fresh pair; after Init so Init
                // can't wipe it.
                _Lp.BeginResync();
                Started = true; Scored = false; ScoredIdx = _Lp.MatchIndex();
                _MatchLive.store(true, std::memory_order_release);
#if LUR_INTERNAL
                // #159: route every executed tick into the linked recording, and #180: OPEN on the
                // netcode's match-start edge (not a MatchStarted() poll, which lost tick 0). Both sinks
                // fire on THIS thread from inside _Lp.Tick, touching only sim-owned recorder state.
                _Lp.SetTickSink(
                    [](void* C, uint32_t Tick, const Rps::InputEvent* Batch, int Count, uint64_t Hash) {
                        [(__bridge RpsViewController*)C recordLinkedTick:Tick batch:Batch count:Count
                                                                    hash:Hash];
                    },
                    (__bridge void*)self);
                _Lp.SetMatchStartSink(
                    [](void* C) { [(__bridge RpsViewController*)C linkedRecBegin]; },
                    (__bridge void*)self);
#endif
                // Peer's GUID known now -> show the ALL-TIME record vs THIS rival rather than 0-0-0.
                {
                    const Rps::Tally T = _Scores.Peer(_Host.Session().GetPeerGuid(), _DeviceId);
                    _PeerWinsA.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                    _PeerLossesA.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                    _PeerDrawsA.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
                }
                os_log(OS_LOG_DEFAULT, "OnlyRps: linked - lockstep started (team %d)", Team);
            }
            if (Started) _Lp.Tick(ElapsedNs);   // produce+send input, execute (sim.step nests)
            _Recovering.store(Started && _Lp.Recovering(), std::memory_order_relaxed);          // #161 -> HUD
            _LinkHalfOpen.store(Started && _Host.Session().IsLinkHalfOpen(), std::memory_order_relaxed); // #163 -> HUD
            // #139/feedback: publish the committed-but-not-yet-simulated camp so the view can show it
            // while we wait for the opponent. Clears the moment the match starts.
            const bool Pending = Started && _Lp.HasLocalCamp() && !_Lp.MatchStarted();
            if (Pending) {
                _PendingCampX.store(_Lp.LocalCamp().X, std::memory_order_relaxed);
                _PendingCampY.store(_Lp.LocalCamp().Y, std::memory_order_relaxed);
            }
            _PendingCampAtomic.store(Pending, std::memory_order_release);
            // Publish a snapshot only when a NEW exec tick landed (per-tick, 10 Hz).
            if (Started) {
                const uint32_t T = _Lp.ExecTick();
                if (T != LastPubTick) {
                    LastPubTick = T;
                    _Mailbox.Back().CaptureFrom(_Lp.GetSim(), NowNs(), kStepNs);
                    _Mailbox.Publish();
                    _PublishedTick.store(T, std::memory_order_release);
                }
            }
            // #149: the tally latch is per MATCH INDEX — re-armed exactly once per restart.
            if (Started && ScoredIdx != _Lp.MatchIndex()) { ScoredIdx = _Lp.MatchIndex(); Scored = false; }
            // #2: tally the linked result ONCE (you are _LinkedTeam) and publish the W-L-D atomics.
            if (Started && !Scored && _Lp.GetSim().Result != Rps::ResultOngoing) {
                Scored = true;
                const uint8_t R = _Lp.GetSim().Result;
                const uint8_t Me = _LinkedTeam.load(std::memory_order_relaxed);
#if LUR_INTERNAL
                // Close the recording on the RESULT (the end line stamps result + tick); a desync-
                // declared draw lands here too, so the file that captured a divergence is complete.
                if (_LinkedRec.IsOpen()) {
                    _LinkedRec.Census(_Lp.GetSim(), Me, /*no AI*/ -1, -1);
                    _LinkedRec.End(_Lp.GetSim());
                    // #204: the MATCH verdict wants the sticky count; the live gate is usually already
                    // cleared by the time a match ends, so this reported diverged matches as desync=0.
                    os_log(OS_LOG_DEFAULT, "OnlyRps: REC linked match finished: result=%u tick=%u "
                           "desync=%u -> %{public}s", static_cast<unsigned>(R), _Lp.GetSim().Tick,
                           _Lp.DesyncsSeen(), _LinkedRecFile.c_str());
                }
#endif
                // Per-rival and persistent, keyed on their device GUID. RecordPeer refuses a malformed
                // or absent id rather than inventing a rivalry row, so keep the session count then.
                const std::string& PeerGuid = _Host.Session().GetPeerGuid();
                if (_Scores.RecordPeer(PeerGuid, _DeviceId, R, Me)) {
                    _Scores.Save(*_Store);
                    const Rps::Tally T = _Scores.Peer(PeerGuid, _DeviceId);
                    _PeerWinsA.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                    _PeerLossesA.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                    _PeerDrawsA.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
                } else {
                    os_log(OS_LOG_DEFAULT, "OnlyRps: peer result not persisted (peer guid %zuB)",
                           PeerGuid.size());
                    if (R == Rps::ResultDraw) _PeerDrawsA.fetch_add(1, std::memory_order_relaxed);
                    else if ((R == Rps::ResultTeam0Wins && Me == 0) ||
                             (R == Rps::ResultTeam1Wins && Me == 1))
                        _PeerWinsA.fetch_add(1, std::memory_order_relaxed);
                    else _PeerLossesA.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // ~500 Hz service: datagram-to-Step latency stays ~ms without busy-spinning a core.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Route a local place/queue event (produced on the MAIN thread by the drag-place UI, or by the agent
// harness on the sim thread) to whichever match is live: the solo sim's thread-safe SoloIn inbox
// (drained by the sim thread's solo tick) or the linked peer's own glue->sim inbox. BOTH are
// thread-safe (#91), so this is safe to call from either thread. _SoloActiveAtomic implies _MatchLive,
// so it is checked first — mirrors RpsMain.cpp's RouteLocalEvent.
- (void)placeLocal:(Rps::InputEvent)E {
    if (_SoloActiveAtomic.load(std::memory_order_acquire)) _SoloIn.Push(E);
    else if (_MatchLive.load(std::memory_order_acquire)) _Lp.QueueLocalEvent(E);
}
// #183: the UIKit touch handlers are now THIN — they package the raw touch (point in DRAWABLE px, pointer
// count, timestamp) and push it to the render thread's queue. All hit-testing (which reads _View/_Cam/
// _Snap/_DevGesture — render-owned now) happens in the replay* methods below, drained once per render frame.
// The timestamp is captured HERE at the real touch time so the console gesture's hold/chain windows stay
// correct even though the replay runs up to one frame later.
- (void)pushTouch:(Lur::Input::ETouchPhase)Phase touches:(NSSet<UITouch*>*)touches event:(UIEvent*)event {
    if (!_RH.IsReady()) return;
    const CGFloat S = [self metalLayer].contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    TouchEvent E;
    E.Phase = Phase;
    E.XPx = static_cast<float>(P.x * S);
    E.YPx = static_cast<float>(P.y * S);
    E.PointerCount = static_cast<int>(event.allTouches.count);
    E.TimeNs = NowNs();
    std::lock_guard<std::mutex> Lk(_TouchMx);
    _TouchQ.push_back(E);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self pushTouch:Lur::Input::ETouchPhase::Began touches:touches event:event];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self pushTouch:Lur::Input::ETouchPhase::Moved touches:touches event:event];
}
// Only the LAST finger up is an Ended, mirroring Android — which handles ACTION_UP and deliberately
// drops the intermediate ACTION_POINTER_UP. UIKit has no such distinction: it calls this once per
// lifting touch, so a two-finger tap arrives as two separate calls. Forwarding both would run the
// release path twice, and the second one — with no two-finger candidate left to suppress it — would
// fall through to the tap hit-test and poke the HUD under wherever that finger happened to be.
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if ([self remainingTouchesAfter:touches event:event] > 0) return;   // an intermediate lift
    [self pushTouch:Lur::Input::ETouchPhase::Ended touches:touches event:event];
}

// The platform saying "this gesture did not happen" (a call, a system alert, a control-centre
// swipe). It was never implemented, so a cancelled placement drag simply hung until the next touch.
// The router treats Cancelled as end-without-commit, which is the whole point of the phase.
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if ([self remainingTouchesAfter:touches event:event] > 0) return;
    [self pushTouch:Lur::Input::ETouchPhase::Cancelled touches:touches event:event];
}

// How many fingers are still down once `touches` have gone. Counted by PHASE rather than by set
// arithmetic: allTouches still contains the ending touches at this point, so a plain count
// difference would be right only when UIKit batches every lift into one call, which it does not.
- (NSUInteger)remainingTouchesAfter:(NSSet<UITouch*>*)touches event:(UIEvent*)event {
    NSUInteger Remaining = 0;
    for (UITouch* T in event.allTouches) {
        if (T.phase != UITouchPhaseEnded && T.phase != UITouchPhaseCancelled) ++Remaining;
    }
    (void)touches;
    return Remaining;
}

// #43 section D: the ~90 lines of replay* methods that used to sit here are Rps::TouchRouter now,
// shared with the Android and desktop mains. What the render thread still owns is the same as before
// — _View, _Cam, _DevGesture, _Snap — and the router is handed pointers to exactly those, so the
// ownership story is unchanged. Only the decisions moved, into somewhere a host test can reach.
@end

// #43 section B: the delegate, the autorelease pool, UIApplicationMain and the #103 MoltenVK
// pre-instance setting all live in LurIosMain now. The setting moved VERBATIM — including the
// reasoning that it is safe only because the shared backend pipelines with per-slot fences — and
// chess picks it up by adopting the same entry point, which it never had.
int main(int argc, char* argv[]) {
    return LurIosMain(argc, argv, [RpsViewController class]);
}
