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

#include <fcntl.h>   // O_NONBLOCK on stdout/stderr — see UnblockStdio
#include <unistd.h>

#include <atomic>    // #69: the sim<->render cross-thread surface (mirrors Android's AppState atomics)
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>   // setenv: MoltenVK log level, before any vkCreateInstance
#include <ctime>     // the flight recorder's per-match filename stamp
#include <mutex>     // #183: guards the main->render touch-event queue
#include <pthread.h> // #183: the render thread runs on a pthread (not std::thread) for a 4MB stack
#include <string>
#include <thread>    // #69/#183: the sim/net thread and now the render thread, both off the UIKit main thread
#include <vector>    // #183: the touch-event queue backing store

#include "Lur/Core/CVar.h"   // #147: registry walk for the gameplay-CVar sync seed
#include "Lur/Core/Log.h"    // the engine logger — routed into os_log below
#include "Lur/Input/ConsoleGesture.h"  // #151: the ONE dev-console gesture, shared with Android
#include "Rps/AgentControl.h"          // LUR_AGENT: assistant remote-control command grammar
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Trace/Trace.h"  // #69: emit the CPU-scope/latency TRACE line (ble.toApply is the target metric)
#include "Lur/Transport/Ble.h"
#include "Rps/AiController.h"
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
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
float Ppu(float WidthPx) {
    return WidthPx / (static_cast<float>(Rps::WorldWidth.Raw) / static_cast<float>(Rps::Fixed::One));
}
float WorldHeightF() {
    return static_cast<float>(Rps::WorldHeight.Raw) / static_cast<float>(Rps::Fixed::One);
}
void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}
// The engine logger had NO sink on either phone, so every Lur::Log::Info/Error from inside the engine
// and the netcode went to a stderr nobody reads. The shim's own os_log calls were visible, which is
// what hid it — the engine's voice was muted while the app looked chatty. It cost a diagnosis on
// 2026-07-30: the #112 build-fingerprint gate fired, said so via Lur::Log::Error, and the line went
// nowhere. %{public}s is mandatory — a plain %s is redacted to <private> unless Xcode is attached,
// which is never our case (see the iOS notes in CLAUDE.md).
void EngineLogSink(bool Error, const char* Line, void* /*User*/) {
    if (Error) os_log_error(OS_LOG_DEFAULT, "OnlyRps: %{public}s", Line);
    else       os_log(OS_LOG_DEFAULT, "OnlyRps: %{public}s", Line);
}
// View-side world (float) -> Fixed for a place event (#139). The raw int travels into the sim /
// over the wire, so no float crosses the determinism boundary.
Rps::Fixed WorldToFixed(float Wv) {
    if (Wv < 0.0f) Wv = 0.0f;
    return Rps::Fixed{static_cast<int32_t>(Wv * static_cast<float>(Rps::Fixed::One) + 0.5f)};
}
// #183: a raw touch packaged on the MAIN (UIKit) thread and drained on the RENDER thread. The UIKit
// handlers must no longer touch _View/_Cam/_DevGesture (those live on the render thread now), so they
// capture only what UIKit alone can read — the point in DRAWABLE PIXELS (already ×contentsScale), the
// pointer count (event.allTouches.count, for the two-finger console gesture), and a timestamp for the
// gesture's hold/chain windows — and push it. The render thread replays the exact hit-test logic.
struct TouchEvent {
    enum EPhase : uint8_t { Began, Moved, Ended };
    uint8_t  Phase;
    float    X, Y;           // drawable pixels
    int      PointerCount;   // pointers down at this event (drives the dev-console gesture)
    uint64_t Ns;             // NowNs() at the touch — feeds ConsoleGesture's timing windows
};
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

// A Metal-backed view: its backing layer is a CAMetalLayer, which MoltenVK turns into
// a Vulkan surface.
@interface RpsView : UIView
@end
@implementation RpsView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

// Declared ahead of the view controller: the #73 reattach hands the delegate a fresh
// UIWindow (the old one may be bound to a dead window-server surface).
@interface RpsAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

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
    Lur::Save::Store* _Store;  // main uses it once (device id + score load) BEFORE the sim thread; sim-only after
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
    Rps::CameraScroll _Cam;
    bool _CamInit;
    float _DownX, _DownY;        // touch-down point (drawable px) for the tap test — render-thread replay state
    // #151: the dev-console gesture — two-finger triple-tap to open, drag-to-scroll while open. It now
    // runs on the RENDER thread (which owns _View), replayed from the touch queue; the MAIN handlers only
    // capture pointer counts + timestamps into TouchEvent. Shared recognizer (Lur::Input::ConsoleGesture).
    Lur::Input::ConsoleGesture _DevGesture;

    // ---- SIM thread only (after _SimThread starts) ----
    Lur::Net::Session _Session;
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
#endif

    // ---- MAIN <-> RENDER cross-thread surface (#183). The render thread owns all Vulkan; MAIN owns UIKit.
    //      Init/Shutdown are handed to the render thread (via _ReinitReq) so the renderer is single-threaded
    //      even across a #73 rebuild. The pause/ack pair parks the render thread at a safe point for BOTH a
    //      background transition (a free-running loop must not vend drawables off-screen) and a reattach. ----
    // #183: a raw pthread, NOT std::thread — the render call chain (GameView::Render -> Dropdown::Draw ->
    // Text) needs ~1MB of stack, which it got for free while it ran on the MAIN thread. A std::thread gets
    // the OS default secondary-thread stack (512KB on iOS) with NO way to enlarge it, and the chain
    // overflowed the guard page on launch (SIGBUS in ___chkstk_darwin). pthread_attr_setstacksize gives it
    // a generous 4MB. The sim thread stays std::thread — its path is shallow.
    pthread_t            _RenderThread;
    bool                 _RenderThreadStarted;
    std::atomic<bool>    _RenderRunning;   // main -> render: keep looping (cleared + joined on teardown)
    std::atomic<bool>    _Ready;           // render -> main: renderer inited (main's touch gate + lifecycle read it)
    std::atomic<bool>    _LayerReady;      // main -> render: the first CAMetalLayer is published; Init may run
    std::atomic<void*>   _LayerPtr;        // main -> render: the current CAMetalLayer (bridged void*)
    std::atomic<bool>    _ReinitReq;       // main -> render: #73 reattach — Shutdown + Init against _LayerPtr
    std::atomic<bool>    _ReinitDone;      // render -> main: reinit finished (main may release _RetiringView)
    std::atomic<bool>    _RenderPauseReq;  // main -> render: park at a safe point (background OR reattach)
    std::atomic<bool>    _RenderPausedAck; // render -> main: parked, not touching the renderer
    std::atomic<bool>    _ResizeReq;       // main -> render: drawable size changed -> call Resize at a safe point
    std::atomic<int32_t> _DrawW, _DrawH;   // main -> render: drawable size in pixels
    std::atomic<int32_t> _InsetTopPx, _InsetBotPx;  // main -> render: safe-area insets (px)
    std::mutex           _TouchMx;         // guards _TouchQ
    std::vector<TouchEvent> _TouchQ;       // main pushes raw touches, the render thread drains once/frame
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
    self.view = [[RpsView alloc] initWithFrame:UIScreen.mainScreen.bounds];
}
- (CAMetalLayer*)metalLayer { return (CAMetalLayer*)self.view.layer; }

// A BLOCKING WRITE TO STDERR CAN KILL THE APP. Diagnosed from a crash report 2026-08-01: the main
// thread stopped in
//     __write_nocancel <- fprintf <- MVKBaseObject::reportMessage <- MVKInstance::logVersions
//                      <- vkCreateInstance <- VulkanRendererImpl::Init <- reattachForActivation
// and FrontBoard killed the process with 0x8BADF00D — "scene-update watchdog transgression,
// exhausted real (wall clock) time allowance of 10.00 seconds".
//
// MoltenVK announces its version through plain fprintf. Nothing on the device drains the app's
// stdio after a `dvt launch`, so once that pipe's buffer fills, write(2) blocks — forever, on the
// thread that also runs the render loop and the sim. The visible symptom is the nastiest kind: a
// BLACK SCREEN with NO log output at all, because os_log never gets a turn either, and the process
// is still alive in proclist so every "is it running?" probe says yes. The #73 heal makes it more
// likely, not less: reattachForActivation creates a SECOND Vulkan instance, so it emits the banner
// a second time.
//
// Two guards, cheapest first. The env var stops MoltenVK writing the chatty lines at all (errors
// still get through); O_NONBLOCK makes any write that would block fail with EAGAIN and DROP the
// bytes instead. Losing a diagnostic line is always better than wedging the app: the log is
// os_log's job here, and stdio has no reader by construction.
static void UnblockStdio() {
    setenv("MVK_CONFIG_LOG_LEVEL", "1", /*overwrite*/ 0);   // 1 = errors only; 0 respects an explicit override
    for (int Fd : {STDOUT_FILENO, STDERR_FILENO}) {
        const int Flags = fcntl(Fd, F_GETFL, 0);
        if (Flags != -1) fcntl(Fd, F_SETFL, Flags | O_NONBLOCK);
    }
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // BEFORE anything else, including the logger: this is what stops a full stdio pipe from
    // freezing the main thread and getting us watchdog-killed with a black screen.
    UnblockStdio();
    // Then give the engine logger a home, before anything can try to report a problem.
    Lur::Log::Init(&EngineLogSink, "OnlyRps");
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

    NSArray<NSString*>* Dirs =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* Dir = Dirs.firstObject ?: NSTemporaryDirectory();
    _SaveDir = std::string(Dir.UTF8String);
    _Store = new Lur::Save::Store(_SaveDir);
#if LUR_AGENT
    {
        // Documents/, not Application Support: Documents is what the dev rig can push into.
        NSArray<NSString*>* Docs =
            NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* D = Docs.firstObject ?: NSTemporaryDirectory();
        _AgentCmdPath = std::string(D.UTF8String) + "/agent.cmd";
        _AgentPollNs = 0;
        os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT CONTROL COMPILED IN (LUR_AGENT=1) — this build must not "
               "be handed to a player. Channel: %{public}s", _AgentCmdPath.c_str());
    }
#endif
    _DeviceId = Lur::Save::LoadOrCreateDeviceId(*_Store);
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

    _Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Peripheral);
    _Session.SetLogger([](const char* M) { os_log(OS_LOG_DEFAULT, "OnlyRps: Net: %{public}s", M); });

    Rps::LockstepPeer* Lp = &_Lp;
    // #160: the message SET is defined once, in Rps/SessionWiring.h, and shared with the Android and
    // desktop mains plus the test harness. It was four hand-maintained copies, and the copies drifted
    // in exactly the way that hurts most: the dev-only cvar-sync slots were registered on ANDROID
    // ONLY, so an iPhone silently dropped the Android's MsgCvarSync, the peers simulated on different
    // Cv and desynced at the first anchor (#147). An unregistered slot fails with no error at either
    // end, so this is not a place to keep a copy.
    Rps::RouteSessionToPeer(_Session, _Lp);
    _Session.SetResyncHandler([Lp] { Lp->BeginResync(); });
    _Session.Start(_Transport, _DeviceId);
    os_log(OS_LOG_DEFAULT, "OnlyRps: session started (device id %zuB)", _DeviceId.size());

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
    _RenderRunning.store(true, std::memory_order_relaxed);
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
    _RenderRunning.store(false, std::memory_order_release);
    _RenderPauseReq.store(false, std::memory_order_release);
    if (_RenderThreadStarted) { pthread_join(_RenderThread, nullptr); _RenderThreadStarted = false; }
    _SimRunning.store(false, std::memory_order_release);  // stop the loop, then wait it out
    if (_SimThread.joinable()) _SimThread.join();
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
    _DrawW.store(static_cast<int32_t>(Layer.drawableSize.width), std::memory_order_relaxed);
    _DrawH.store(static_cast<int32_t>(Layer.drawableSize.height), std::memory_order_relaxed);
    const UIEdgeInsets Sa = self.view.safeAreaInsets;
    _InsetTopPx.store(static_cast<int32_t>(Sa.top * Scale), std::memory_order_relaxed);
    _InsetBotPx.store(static_cast<int32_t>(Sa.bottom * Scale), std::memory_order_relaxed);

    if (!_LayerReady.load(std::memory_order_acquire)) {
        // First valid layout: record the #73 precondition (a renderer born while inactive presents into a
        // layer the window server never composites), publish the layer, then let the render thread Init.
        _InitWhileInactive =
            UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
#if !LUR_SHIPPING
        _AppliedRenderScaleRaw = Rps::Fixed::One;  // published at native scale (k=1.0)
#endif
        _LayerPtr.store((__bridge void*)Layer, std::memory_order_release);
        _LayerReady.store(true, std::memory_order_release);
        os_log(OS_LOG_DEFAULT, "OnlyRps: layer published %dx%d appActive=%d — render thread will init #183",
               (int)Layer.drawableSize.width, (int)Layer.drawableSize.height, _InitWhileInactive ? 0 : 1);
    } else {
        _ResizeReq.store(true, std::memory_order_release);  // render thread calls Resize at a safe point
    }
}

- (void)onBecameActive {
    _BecameActive = true;  // the #73 init-while-inactive reattach is handled by the lifecycle tick
    // Resume a render thread parked on resign/background. If it was inited while inactive, the tick's
    // reattach takes over instead (it re-parks + rebuilds), so don't blindly resume in that case.
    if (_Ready.load(std::memory_order_acquire) && !_InitWhileInactive)
        _RenderPauseReq.store(false, std::memory_order_release);
}

- (void)onWillResign {
    if (_Ready.load(std::memory_order_acquire)) _RenderPauseReq.store(true, std::memory_order_release);
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
- (void)warnIfSolo:(const char*)What {
    if (!_SoloActiveAtomic.load(std::memory_order_acquire)) return;
    if (_Session.IsReady())
        os_log_error(OS_LOG_DEFAULT, "OnlyRps: AGENT %{public}s -> the SOLO sim, not the linked peer "
                     "(a peer IS linked). The other phone will wait forever. Send `linked` first, "
                     "then re-send this.", What);
    else
        os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT %{public}s -> the solo sim (no peer linked yet)", What);
}

- (void)applyAgentCommand:(const Rps::AgentCommand&)Cmd {
    switch (Cmd.Kind) {
        case Rps::EAgentCmd::Place: {
            // EXACT coordinates, which the touch UI cannot produce: drag-to-place snaps to the nearest
            // VALID square and emits nothing on an invalid drop, so a human cannot place onto the
            // square an existing camp occupies — and that is exactly the #160 collision.
            const uint8_t Team = _LinkedTeam.load(std::memory_order_relaxed);
            const Rps::InputEvent E = Rps::InputEvent::Place(
                Team, static_cast<uint8_t>(Cmd.C & 3), Rps::F(Cmd.A), Rps::F(Cmd.B));
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT place type=%d at (%d,%d) team=%u", Cmd.C, Cmd.A,
                   Cmd.B, static_cast<unsigned>(Team));
            [self warnIfSolo:"place"];
            [self placeLocal:E];
            break;
        }
        case Rps::EAgentCmd::Queue:
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT queue slot=%d count=%d", Cmd.A, Cmd.B);
            [self warnIfSolo:"queue"];
            [self placeLocal:Rps::InputEvent::Queue(_LinkedTeam.load(std::memory_order_relaxed),
                                                    Cmd.A, Cmd.B)];
            break;
        case Rps::EAgentCmd::Stress: {
            Rps::Sim& Sm = _SoloActiveAtomic.load(std::memory_order_acquire)
                               ? _SoloSim : const_cast<Rps::Sim&>(_Lp.GetSim());
            Sm.StressFill(Cmd.A, static_cast<uint8_t>(Cmd.B));
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT stress %d/team type %d -> count=%d", Cmd.A, Cmd.B,
                   Sm.Count);
            break;
        }
        case Rps::EAgentCmd::Corrupt:
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT corrupt gold %+d — forcing a divergence (#161)", Cmd.A);
            _Lp.AgentCorruptState(Cmd.A);
            break;
        case Rps::EAgentCmd::DropTx:
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT drop next %d produced frame(s) (#163)", Cmd.A);
            _Lp.AgentDropOutgoing(Cmd.A);
            break;
        case Rps::EAgentCmd::Console:
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT console %d", Cmd.A);
            _View.SetDevOverlayOpen(Cmd.A != 0);
            break;
        case Rps::EAgentCmd::Gesture:
            // #151's real subject: drive the SHARED recognizer with a synthetic two-finger triple-tap.
            // The recognizer lives on the MAIN thread (it owns the touch stream), so this command — on
            // the SIM thread — hands the request across via the atomic instead of driving _DevGesture
            // directly, exactly as Android's ApplyAgentCommand does (it can't touch the glue-owned
            // recognizer either).
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT gesture: synthetic two-finger triple-tap requested");
            _AgentGestureRequest.store(true, std::memory_order_release);
            break;
        case Rps::EAgentCmd::KillOwn: {
            // #160 setup: free the ground under our own camp so it can be rebuilt on the same square —
            // the only route to a produced placement whose coordinates equal the opening camp's.
            const uint8_t Team = _LinkedTeam.load(std::memory_order_relaxed);
            Rps::Sim& Sm = const_cast<Rps::Sim&>(_Lp.GetSim());
            for (int32_t I = 0; I < Sm.Count; ++I) {
                if (!Sm.IsAlive(I) || Sm.Team[I] != Team) continue;
                if (!Sm.IsBuilding(I) || Sm.IsHomeBase(I)) continue;
                if (Sm.Type[I] != static_cast<uint8_t>(Cmd.A & 3)) continue;
                os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT killown slot=%d at (%d,%d)", I,
                       Sm.PosX[I].ToInt(), Sm.PosY[I].ToInt());
                Sm.Hp[I] = 0;
                break;
            }
            break;
        }
        case Rps::EAgentCmd::Linked:
            // #170: the ONE route into the linked session a harness can rely on. It sets the same flag
            // the selector's "Linked opponent" row sets, and that route is deliberately exempt from the
            // `!HasMinerCamp(0)` gate the AUTO-switch carries — so it still works after a stray `place`
            // has put a camp in the solo sim, which is the state the auto-switch can never leave.
            // The flag is LATCHED (see the switch site): sending this before the link is up is fine,
            // it takes effect on the frame the peer becomes ready.
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT linked -> requesting the switch to the linked opponent");
            _SwitchToLinkedAtomic.store(true, std::memory_order_release);
            break;
        case Rps::EAgentCmd::None:
            break;
    }
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
    if (_AgentCtl.Poll(Buf, Cmd)) [self applyAgentCommand:Cmd];
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
- (void)reattachForActivation {
    // Scene-aware (#73 round 5): a window made with initWithFrame relies on legacy
    // adoption into the implicit scene — exactly what the broken launch never does.
    // Attach EXPLICITLY to a connected UIWindowScene; without one, any new window is
    // another orphan, so skip and let the render-loop retry when a scene exists.
    UIWindowScene* Scene = nil;
    for (UIScene* S in UIApplication.sharedApplication.connectedScenes) {
        if (![S isKindOfClass:UIWindowScene.class]) continue;
        Scene = (UIWindowScene*)S;
        if (S.activationState == UISceneActivationStateForegroundActive) break;  // best pick
    }
    if (Scene == nil) {
        os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach SKIPPED: no connected UIWindowScene "
                               "(scenes=%lu) - will retry",
               (unsigned long)UIApplication.sharedApplication.connectedScenes.count);
        return;
    }
    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: view unhosted - pausing render thread + rebuilding "
                           "window+view+layer on scene state=%ld", (long)Scene.activationState);
    // #183: the render thread owns the renderer, so PARK it before touching the layer it renders into.
    // Bounded busy-wait on MAIN — this is a rare heal, not the hot loop; the ack normally lands within one
    // frame (~16 ms). Proceed best-effort if it somehow doesn't park (better than wedging the heal).
    _RenderPauseReq.store(true, std::memory_order_release);
    for (int I = 0; I < 250 && !_RenderPausedAck.load(std::memory_order_acquire); ++I)
        [NSThread sleepForTimeInterval:0.004];  // ~1 s cap
    if (!_RenderPausedAck.load(std::memory_order_acquire))
        os_log_error(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: render thread did not park in time — proceeding");

    // Detach the OLD window FIRST: it still holds rootViewController == self, and its
    // later dealloc tears the root VC's view out of whatever window now hosts it —
    // which re-unhosted the fresh view and made this reattach loop every 2 s.
    RpsAppDelegate* Delegate = (RpsAppDelegate*)UIApplication.sharedApplication.delegate;
    UIWindow* Old = Delegate.window;
    Old.hidden = YES;
    Old.rootViewController = nil;
    // Hold the OLD view (and its CAMetalLayer) alive until the render thread finishes Shutdown/Init on it —
    // the old VkSurfaceKHR wraps that layer, and vkDestroySurfaceKHR runs on the render thread AFTER this
    // method returns. Released on the render thread's _ReinitDone ack (in lifecycleTick), on MAIN, so the
    // UIView still deallocs on the main thread.
    _RetiringView = self.view;
    RpsView* NewView = [[RpsView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.view = NewView;
    CAMetalLayer* Layer = (CAMetalLayer*)NewView.layer;
    Layer.device = MTLCreateSystemDefaultDevice();
    Layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    Layer.contentsScale = UIScreen.mainScreen.scale;
    Layer.drawableSize = CGSizeMake(NewView.bounds.size.width * Layer.contentsScale,
                                    NewView.bounds.size.height * Layer.contentsScale);

    UIWindow* NewWindow = [[UIWindow alloc] initWithWindowScene:Scene];
    NewWindow.frame = Scene.coordinateSpace.bounds;
    NewWindow.rootViewController = self;
    [NewWindow makeKeyAndVisible];
    Delegate.window = NewWindow;

    // Publish the fresh layer + size + insets, record the precondition, then hand the Shutdown/Init to the
    // render thread and RESUME it — it re-inits against _LayerPtr and signals _ReinitDone.
    _DrawW.store(static_cast<int32_t>(Layer.drawableSize.width), std::memory_order_relaxed);
    _DrawH.store(static_cast<int32_t>(Layer.drawableSize.height), std::memory_order_relaxed);
    const UIEdgeInsets Sa = NewView.safeAreaInsets;
    _InsetTopPx.store(static_cast<int32_t>(Sa.top * Layer.contentsScale), std::memory_order_relaxed);
    _InsetBotPx.store(static_cast<int32_t>(Sa.bottom * Layer.contentsScale), std::memory_order_relaxed);
    _LayerPtr.store((__bridge void*)Layer, std::memory_order_release);
    _InitWhileInactive =
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
#if !LUR_SHIPPING
    _AppliedRenderScaleRaw = Rps::Fixed::One;  // #103: rebuilt at native scale; the tick re-applies any override
#endif
    _ReinitReq.store(true, std::memory_order_release);
    _RenderPauseReq.store(false, std::memory_order_release);
    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: handed reinit to the render thread (drawable %dx%d, appActive=%d)",
           (int)Layer.drawableSize.width, (int)Layer.drawableSize.height, _InitWhileInactive ? 0 : 1);
}

#if !LUR_SHIPPING
// #103: apply rps.dev.render_scale by rescaling the CAMetalLayer's backing store (MAIN, UIKit) and asking
// the render thread to recreate the swapchain — but ONLY when the value actually changed (a swapchain
// recreate is not free). Setting contentsScale AND drawableSize together keeps the render extent and the
// touch-point mapping (touchesBegan/Moved/Ended read contentsScale) in the same coordinate space. Called
// from the MAIN lifecycle tick; the render thread owns the renderer, so it does the Resize via _ResizeReq.
- (void)applyRenderScaleIfChanged {
    if (!_Ready.load(std::memory_order_acquire)) return;
    const int32_t Raw = CvRenderScale.Get().Raw;
    if (Raw == _AppliedRenderScaleRaw) return;
    const float K = static_cast<float>(Raw) / static_cast<float>(Rps::Fixed::One);
    const CGFloat Eff = UIScreen.mainScreen.scale * K;   // native retina * multiplier
    CAMetalLayer* Layer = [self metalLayer];
    Layer.contentsScale = Eff;
    Layer.drawableSize = CGSizeMake(self.view.bounds.size.width * Eff,
                                    self.view.bounds.size.height * Eff);
    if (Layer.drawableSize.width == 0 || Layer.drawableSize.height == 0) return;  // not laid out yet — retry next tick
    _DrawW.store(static_cast<int32_t>(Layer.drawableSize.width), std::memory_order_relaxed);
    _DrawH.store(static_cast<int32_t>(Layer.drawableSize.height), std::memory_order_relaxed);
    _ResizeReq.store(true, std::memory_order_release);   // render thread recreates the swapchain
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
    while (_RenderRunning.load(std::memory_order_acquire) &&
           !_LayerReady.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    if (!_RenderRunning.load(std::memory_order_acquire)) return;
    @autoreleasepool {
        void* Layer = _LayerPtr.load(std::memory_order_acquire);
        _Renderer = Lur::Render::VulkanRenderer::Create();
        const bool Ok = _Renderer && _Renderer->Init(Layer);
        _Ready.store(Ok, std::memory_order_release);
        os_log(OS_LOG_DEFAULT, "OnlyRps: Renderer init: %{public}s (drawable %dx%d) [render thread] #183",
               Ok ? "ok" : "failed", _DrawW.load(std::memory_order_relaxed),
               _DrawH.load(std::memory_order_relaxed));
        if (Ok) _View.CreateResources(_Renderer);
    }
    double PrevTime = 0.0;
    while (_RenderRunning.load(std::memory_order_acquire)) {
        // ---- Pause / reattach handshake. MAIN parks us (background OR #73 reattach) via _RenderPauseReq;
        // we ack at this SAFE point (no drawable held, renderer idle) and spin until released. On release,
        // a pending _ReinitReq means MAIN rebuilt the layer -> do the Vulkan Shutdown/Init HERE (renderer
        // stays single-threaded), then signal _ReinitDone so MAIN can release the retiring view. ----
        if (_RenderPauseReq.load(std::memory_order_acquire)) {
            _RenderPausedAck.store(true, std::memory_order_release);
            while (_RenderPauseReq.load(std::memory_order_acquire) &&
                   _RenderRunning.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            _RenderPausedAck.store(false, std::memory_order_release);
            if (!_RenderRunning.load(std::memory_order_acquire)) break;
            if (_ReinitReq.exchange(false, std::memory_order_acq_rel)) {
                @autoreleasepool {
                    _Ready.store(false, std::memory_order_release);
                    _Renderer->Shutdown();  // full teardown (device, surface, everything) of the old layer
                    void* Layer = _LayerPtr.load(std::memory_order_acquire);
                    const bool Ok = _Renderer->Init(Layer);
                    _Ready.store(Ok, std::memory_order_release);
                    if (Ok) _View.CreateResources(_Renderer);
                    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: re-init %{public}s (drawable %dx%d) [render thread]",
                           Ok ? "ok" : "FAILED", _DrawW.load(std::memory_order_relaxed),
                           _DrawH.load(std::memory_order_relaxed));
                }
                _ReinitDone.store(true, std::memory_order_release);  // MAIN releases _RetiringView
                PrevTime = 0.0;
            }
            continue;
        }
        if (!_Ready.load(std::memory_order_acquire)) {  // init failed / mid-reinit — idle, don't spin hot
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        @autoreleasepool { [self renderOneFrame:&PrevTime]; }
    }
}

// One free-running render iteration. RENDER thread only; reads drawable size + insets from the atomics
// MAIN publishes (never the layer directly), and touches _View/_Cam/_DevGesture/_Snap/_Renderer freely
// because it is their sole owner.
- (void)renderOneFrame:(double*)PrevTimePtr {
    const double Now = CACurrentMediaTime();
    const uint64_t ElapsedNs = *PrevTimePtr > 0.0 ? static_cast<uint64_t>((Now - *PrevTimePtr) * 1e9) : 0;
    *PrevTimePtr = Now;

    // #103 render_scale (or a rotation/layout): MAIN resized the layer + published the new drawable size;
    // recreate the swapchain here, where we own the renderer.
    if (_ResizeReq.exchange(false, std::memory_order_acq_rel))
        _Renderer->Resize(_DrawW.load(std::memory_order_relaxed), _DrawH.load(std::memory_order_relaxed));

    // Drain the touch queue and replay the exact hit-test/input logic (was in the UIKit handlers pre-#183)
    // at the TOP of the frame, so a placement/tap carries ≤1 frame of added latency.
    [self drainTouches];

#if LUR_AGENT
    // Agent `gesture`: feed the SHARED recognizer a synthetic two-finger triple-tap. Now on the RENDER
    // thread (it owns _DevGesture); the sim thread hands the request across via the atomic. Mirrors the
    // Android glue thread; unchanged effect, just relocated with the rest of the input state.
    if (_AgentGestureRequest.exchange(false, std::memory_order_acquire)) {
        const uint64_t T0 = NowNs();
        bool Opened = false;
        for (int Tap = 0; Tap < Rps::AgentGestureTaps; ++Tap) {
            const uint64_t Down = T0 + static_cast<uint64_t>(Tap) * 200'000'000ull;
            _DevGesture.PointersDown(1, Down);
            _DevGesture.PointersDown(2, Down);
            Opened = _DevGesture.LiftAndShouldOpen(Down + 40'000'000ull);
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
        _View.SetLinked(true, _Session.GetPeerGuid());   // label the row with the peer's id (#178)
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

    const float W = static_cast<float>(_DrawW.load(std::memory_order_relaxed));
    const float H = static_cast<float>(_DrawH.load(std::memory_order_relaxed));
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
    for (const TouchEvent& E : Batch) {
        switch (E.Phase) {
            case TouchEvent::Began: [self replayTouchBegan:E]; break;
            case TouchEvent::Moved: [self replayTouchMoved:E]; break;
            case TouchEvent::Ended: [self replayTouchEnded:E]; break;
        }
    }
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
        if (_Ready.load(std::memory_order_acquire) && _InitWhileInactive) [self reattachForActivation];
    }
    // #73 persistent orphan: after a DVT relaunch the view can sit in no window / no scene while we think
    // we are ready (presents "succeed" into the orphan layer; screen black). Heal, retried ~2 s.
    static int OrphanTicks = 0;
    if (_Ready.load(std::memory_order_acquire) &&
        (self.view.window == nil || self.view.window.windowScene == nil)) {
        if (++OrphanTicks >= 4) { OrphanTicks = 0; [self reattachForActivation]; }  // ~2 s at 0.5 s/tick
    } else {
        OrphanTicks = 0;
    }
    // Release the old view/layer once the render thread finished Shutdown/Init on it — on MAIN, so the
    // UIView deallocs on the main thread.
    if (_ReinitDone.exchange(false, std::memory_order_acq_rel)) _RetiringView = nil;

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
                os_log(OS_LOG_DEFAULT, "OnlyRps: %{public}s tick=%u you=%d foe=%d desync=%d badbuild=%d "
                       "presented=%u hash=%08x gold=%d frontier=%d started=%d gaps=%d gapat=%u stall=%d "
                       "halfopen=%d restarts=%d rollbk=%d resim=%u spec=%lld",
                       SoloDiag ? "SOLO" : "LOCKSTEP", DS.Tick, DS.AliveCount(0), DS.AliveCount(1),
                       (!SoloDiag && _Lp.Desynced()) ? 1 : 0, _Lp.BuildMismatch() ? 1 : 0,
                       _PresentedFrames.load(std::memory_order_relaxed),
                       static_cast<uint32_t>(DS.StateHash() & 0xFFFFFFFFu),
                       DS.Teams[SoloDiag ? 0 : Me].Gold, DS.FrontierT0.ToInt(),
                       SoloDiag ? 1 : (_Lp.MatchStarted() ? 1 : 0),
                       SoloDiag ? 0 : _Lp.InputGaps(), SoloDiag ? 0u : _Lp.LastInputGapTick(),
                       (!SoloDiag && _Lp.PreMatchStalled()) ? 1 : 0,
                       (!SoloDiag && _Session.IsLinkHalfOpen()) ? 1 : 0,
                       SoloDiag ? 0 : _Session.RadioRestartsAttempted(),
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
        _Session.Tick(ElapsedNs);
        const bool PeerReady = _Session.IsReady();
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
            const Rps::Tally T = _Scores.Peer(_Session.GetPeerGuid(), _DeviceId);
            _PeerWinsA.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
            _PeerLossesA.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
            _PeerDrawsA.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
        }
        const bool ManualPick = _SwitchToLinkedAtomic.load(std::memory_order_acquire);
        const bool AutoSwitch = LinkEdge && !_SoloSim.HasMinerCamp(0);   // unstarted AI match only
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
            if (_SoloSim.Result == Rps::ResultOngoing && !_SoloSim.HasMinerCamp(0)) {
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
                const uint8_t Team = _DeviceId < _Session.GetPeerGuid() ? 0 : 1;
                _LinkedTeam.store(Team, std::memory_order_relaxed);
                _Lp.Init(kMatchSeed, Team, SendViaSession, &_Session);
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
                    const Rps::Tally T = _Scores.Peer(_Session.GetPeerGuid(), _DeviceId);
                    _PeerWinsA.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                    _PeerLossesA.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                    _PeerDrawsA.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
                }
                os_log(OS_LOG_DEFAULT, "OnlyRps: linked - lockstep started (team %d)", Team);
            }
            if (Started) _Lp.Tick(ElapsedNs);   // produce+send input, execute (sim.step nests)
            _Recovering.store(Started && _Lp.Recovering(), std::memory_order_relaxed);          // #161 -> HUD
            _LinkHalfOpen.store(Started && _Session.IsLinkHalfOpen(), std::memory_order_relaxed); // #163 -> HUD
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
                    os_log(OS_LOG_DEFAULT, "OnlyRps: REC linked match finished: result=%u tick=%u "
                           "desync=%d -> %{public}s", static_cast<unsigned>(R), _Lp.GetSim().Tick,
                           _Lp.Desynced() ? 1 : 0, _LinkedRecFile.c_str());
                }
#endif
                // Per-rival and persistent, keyed on their device GUID. RecordPeer refuses a malformed
                // or absent id rather than inventing a rivalry row, so keep the session count then.
                const std::string& PeerGuid = _Session.GetPeerGuid();
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

// Touch (#139/#140, mirror of the desktop/Android mains): a touch-down on a build plate starts a
// drag-to-place (the ghost follows the finger, lifted up-left of it by ~its size so the thumb
// doesn't hide it; a valid release emits a Place event); any other drag pans the camera; a tap on
// a building's x1/x5 button queues units. MAIN thread: unit input crosses to the sim thread via
// placeLocal (the thread-safe SoloIn inbox / Lp.QueueLocalEvent). Placement is gated on _MatchLive (a
// live solo or peer match). You play _LinkedTeam (published by the sim thread; 0 in solo).
- (float)ghostOffPxForWidth:(float)W {
    return static_cast<float>(_Snap.Cv.BuildingFootprint.Raw) / static_cast<float>(Rps::Fixed::One) * 0.5f * Ppu(W);
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
- (void)pushTouch:(TouchEvent::EPhase)Phase touches:(NSSet<UITouch*>*)touches event:(UIEvent*)event {
    if (!_Ready.load(std::memory_order_acquire)) return;
    const CGFloat S = [self metalLayer].contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    TouchEvent E;
    E.Phase = static_cast<uint8_t>(Phase);
    E.X = static_cast<float>(P.x * S);
    E.Y = static_cast<float>(P.y * S);
    E.PointerCount = static_cast<int>(event.allTouches.count);
    E.Ns = NowNs();
    std::lock_guard<std::mutex> Lk(_TouchMx);
    _TouchQ.push_back(E);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self pushTouch:TouchEvent::Began touches:touches event:event];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self pushTouch:TouchEvent::Moved touches:touches event:event];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self pushTouch:TouchEvent::Ended touches:touches event:event];
}

// ---- RENDER-thread replay of the exact hit-test logic (was inline in the UIKit handlers pre-#183). Reads
// the published drawable size (_DrawW/_DrawH), not the layer, and touches _View/_Cam/_DevGesture/_Snap
// freely as their owner. Behaviour is verbatim — only the thread and the coordinate source changed. ----
- (void)replayTouchBegan:(const TouchEvent&)E {
    const float X = E.X, Y = E.Y;
    _DownX = X; _DownY = Y;
#if !LUR_SHIPPING
    // #151: the SHARED console recognizer (Lur::Input::ConsoleGesture). Pointer count + timestamp came
    // from the UIKit handler; feed them straight in.
    _DevGesture.PointersDown(E.PointerCount, E.Ns);
    // While the console is open it OWNS the pointer — a drag scrolls the cvar list and a still release is a
    // DevTap. Swallowing the gesture is the point: the panel sits over a LIVE match.
    if (_View.DevOverlayOpen()) { _DevGesture.DragBegin(Y); return; }
#endif
    const float W = static_cast<float>(_DrawW.load(std::memory_order_relaxed));
    const float Off = [self ghostOffPxForWidth:W];
    const uint8_t MyTeam = _LinkedTeam.load(std::memory_order_relaxed);   // sim thread publishes it
    const bool Live = _MatchLive.load(std::memory_order_acquire);          // solo OR peer match live
    const int Plate = Live ? _View.PlateAt(X, Y) : -1;  // plate hit-test at the real finger
    if (Plate >= 0) {
        _View.BeginPlaceDrag(Plate, X - Off, Y - Off);  // sets the ghost type; seed at the offset spot
        const float H = static_cast<float>(_DrawH.load(std::memory_order_relaxed));
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        const bool V = _View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, MyTeam == 1, _Snap, MyTeam, Wx, Wy, Gsx, Gsy);
        // Finger point AND snapped point: ghost on the finger, snap eased (visual only).
        _View.UpdatePlaceDrag(X - Off, Y - Off, Gsx, Gsy, V);
    } else {
        // #107 revised (feedback 2026-08-04): units queue on PRESS-DOWN, not release, for immediacy —
        // the queue fires the instant you touch the button. A hit on an x1/x5 button enqueues NOW (and
        // lights up); a miss just primes a pan. Cam.Begin runs regardless, so a drag that starts on a
        // button still scrolls (harmless — already queued). Drops the old "a press that turns into a
        // pan queues nothing" guard: a button press IS a queue. (Mirror of the Android main.)
        int32_t Slot = -1;
        const int Cnt = Live ? _View.OnProductionButton(X, Y, Slot) : 0;
        if (Cnt > 0) {
            [self placeLocal:Rps::InputEvent::Queue(MyTeam, Slot, Cnt)];
            _View.PressProductionButton(X, Y);  // visual flash on the pressed button
        }
        _Cam.Begin(Y);
    }
}
- (void)replayTouchMoved:(const TouchEvent&)E {
    const float X = E.X, Y = E.Y;
    const float W = static_cast<float>(_DrawW.load(std::memory_order_relaxed));
    const float H = static_cast<float>(_DrawH.load(std::memory_order_relaxed));
#if !LUR_SHIPPING
    if (_View.DevOverlayOpen()) { _View.DevScroll(_DevGesture.DragMove(Y)); return; }  // #151
#endif
    if (_View.IsPlacing()) {
        const float Off = [self ghostOffPxForWidth:W];
        const uint8_t MyTeam = _LinkedTeam.load(std::memory_order_relaxed);
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        const bool V = _View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, MyTeam == 1, _Snap, MyTeam, Wx, Wy, Gsx, Gsy);
        _View.UpdatePlaceDrag(X - Off, Y - Off, Gsx, Gsy, V);
        _DevGesture.Cancel();  // #151: a placement drag is not a console tap
    } else {
        _Cam.Move(Y, Ppu(W));  // content-drag pans the camera
    }
}
- (void)replayTouchEnded:(const TouchEvent&)E {
    const float X = E.X, Y = E.Y;
    const float W = static_cast<float>(_DrawW.load(std::memory_order_relaxed));
    const float H = static_cast<float>(_DrawH.load(std::memory_order_relaxed));
#if !LUR_SHIPPING
    // #151: the console owns the gesture while it is open — a still release is a tap it hit-tests
    // (rows, numpad, the top-left X that closes it), anything else was a scroll already applied.
    if (_View.DevOverlayOpen()) {
        if (_DevGesture.DragEndIsTap()) _View.DevTap(X, Y);
        return;
    }
#endif
    if (_View.IsPlacing()) {
        const float Off = [self ghostOffPxForWidth:W];
        const uint8_t MyTeam = _LinkedTeam.load(std::memory_order_relaxed);
        bool Placed = false;
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        if (_View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, MyTeam == 1, _Snap, MyTeam, Wx, Wy, Gsx, Gsy)) {
            [self placeLocal:Rps::InputEvent::Place(MyTeam, static_cast<uint8_t>(_View.PlacingType()),
                                                    WorldToFixed(Wx), WorldToFixed(Wy))];
            Placed = true;
        }
        _View.EndPlaceDrag(Placed);  // valid -> the real building takes over; else slide back
        _DevGesture.Cancel();        // #151
        return;
    }
    _Cam.End();
#if !LUR_SHIPPING
    // #151: two-finger triple-tap OPENS the console, with the same windows Android uses because it is the
    // same recognizer. The lift timestamp is E.Ns — the real touch time captured in the UIKit handler.
    const bool WasTwoFinger = _DevGesture.TwoFingerActive();
    if (_DevGesture.LiftAndShouldOpen(E.Ns)) {
        _View.SetDevOverlayOpen(true);
        os_log(OS_LOG_DEFAULT, "OnlyRps: dev console opened (two-finger triple-tap, #151)");
        return;
    }
    if (WasTwoFinger) return;   // a tap in the chain: do not also hit the HUD underneath
#endif
    const bool Tap = (X - _DownX) * (X - _DownX) + (Y - _DownY) * (Y - _DownY) < (24.0f * 24.0f);
    if (Tap) {
        // The opponent selector consumes its own taps; an AI row (re)starts solo, the linked row
        // switches to the peer; a plate tap does nothing (drag to place); a world tap may hit a
        // building's x1/x5 queue button.
        const int Hit = _View.OnTap(X, Y);
        const int Tier = _View.TakeAiTier();
        if (Tier >= 0) {
            _SoloAiTier.store(Tier, std::memory_order_release);          // (re)start solo at this tier (#2)
        } else if (_View.TakePeerPick()) {
            _SwitchToLinkedAtomic.store(true, std::memory_order_release);  // switch to the linked peer (#2)
        }
        // (Per-building x1/x5 queue buttons now fire on touch-DOWN — see replayTouchBegan — not here.)
    }
}
@end

@implementation RpsAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[RpsViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    // #103: let vkQueueSubmit return WITHOUT blocking on Metal command-buffer scheduling. MoltenVK
    // defaults this ON, so every submit stalled a full vsync in nextDrawable (measured es.submit
    // ~16ms) — the CPU could never run ahead even with 2 frames in flight. Turning it off is safe
    // ONLY because the backend now pipelines N frames with per-slot fences + per-image semaphores
    // (VulkanBackend.cpp), which order the GPU work that async submit no longer serializes. Set here,
    // before UIApplicationMain, so it lands before the first vkCreateInstance. Env-var form (not the
    // vk_mvk_moltenvk API) keeps the shared backend free of MoltenVK headers.
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", /*overwrite*/ 1);
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([RpsAppDelegate class]));
    }
}
