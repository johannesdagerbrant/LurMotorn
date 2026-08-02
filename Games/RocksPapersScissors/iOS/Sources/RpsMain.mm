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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>   // setenv: MoltenVK log level, before any vkCreateInstance
#include <ctime>     // the flight recorder's per-match filename stamp
#include <string>
#include <vector>

#include "Lur/Core/CVar.h"   // #147: registry walk for the gameplay-CVar sync seed
#include "Lur/Core/Log.h"    // the engine logger — routed into os_log below
#include "Lur/Input/ConsoleGesture.h"  // #151: the ONE dev-console gesture, shared with Android
#include "Rps/AgentControl.h"          // LUR_AGENT: assistant remote-control command grammar
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Ble.h"
#include "Rps/AiController.h"
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
#include "Rps/LockstepPeer.h"
#include "Rps/MatchRecord.h"   // #144 solo flight recorder (LUR_INTERNAL; parity with Android)
#include "Rps/ScoreBook.h"     // persistent all-time W-L-D per AI tier / per rival
#include "Rps/SessionWiring.h" // the ONE Session->LockstepPeer routing table (#160)
#include "Rps/Snapshot.h"
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
@end

@implementation RpsViewController {
    Lur::Render::IRenderer* _Renderer;
    Rps::GameView _View;
    Lur::Transport::ITransport* _Transport;
    Lur::Net::Session _Session;
    Rps::LockstepPeer _Lp;
    Lur::Save::Store* _Store;
    std::string _SaveDir;      // Application Support — the Store's dir, kept for the .rec paths
    std::string _DeviceId;
    Rps::Snapshot _Snap;
    bool _Started;
    uint32_t _LastTick;
    uint64_t _TickLandedNs;
    Rps::CameraScroll _Cam;
    bool _CamInit;
    float _DownX, _DownY;
    // #151: the dev-console gesture — two-finger triple-tap to open, drag-to-scroll while open. It was
    // simply absent here (the recognizer had never been written for iOS), so the console was
    // unreachable on the iPhone and on-device tuning was Android-only. Shared with the Android and
    // desktop shims rather than hand-written a third time; the three copies had already drifted.
    Lur::Input::ConsoleGesture _DevGesture;
#if LUR_AGENT
    // ---- Assistant remote control (CLAUDE.md's LUR_AGENT axis) ----
    // Channel: Documents/agent.cmd inside the app container, holding "<seq> <verb> [args]". A FILE
    // rather than a system property because iOS has no equivalent, and because the dev rig can already
    // push into the container (`pymobiledevice3 apps push <bundle> <local> Documents/<name>`) — the
    // same mechanism the role/clearsave markers use. Polled on the render thread, which on iOS is also
    // the sim thread, so a command may touch _Lp/_SoloSim directly.
    //
    // This is the half that most needs it: there is no touch injection for iOS at all, so without this
    // every two-phone scenario needs a person tapping the iPhone. Absent from every ordinary build;
    // clear the file when handing the phone back.
    Rps::AgentControl _AgentCtl;
    std::string _AgentCmdPath;
    uint64_t _AgentPollNs;
#endif
    uint8_t _Team;
    CADisplayLink* _DisplayLink;
    double _PrevFrameTime;
    bool _Ready;
    // #73: a DVT launch can initialise the renderer while the app is NOT active —
    // the layer created in that state is never composited (presents "succeed" into
    // the void; screen black). Record the state at init; on becoming active, rebuild
    // window+view+layer+renderer from scratch (a swapchain recreate is NOT enough —
    // proven by 898999b).
    bool _InitWhileInactive;
    bool _BecameActive;
    // Score vs the linked peer + a one-shot tally latch. These four are the DISPLAY copy; the
    // all-time record behind them is _Scores, persisted through _Store (iOS is single-threaded here,
    // so there is no handoff to arrange — the render loop owns all of it).
    int _PeerW, _PeerL, _PeerD;
    bool _Scored;
    Rps::ScoreBook _Scores;
    // #2/#127 solo-vs-AI on iOS (single-threaded — the solo sim ticks in renderFrame). Mirrors the
    // Android session: auto-start Easy, a selector pick (re)starts a tier, a peer link offers a switch.
    Rps::Sim _SoloSim;
    Rps::AiController _SoloAi;
    bool _SoloActive;
    int _SoloTier;
    uint64_t _SoloAccumNs;
    bool _SoloScored;
    uint64_t _SoloPostNs;      // #149 wall time held on the solo win/lose screen
    uint32_t _ScoredIdx;       // #149 which Lp match index _Scored refers to
    std::vector<Rps::InputEvent> _SoloPending;  // human events queued (main thread), drained per tick
    int _PendingTier;          // one-shot selector pick -> (re)start solo (-1 = none)
    bool _SwitchToLinked;      // selector: switch from solo to the linked peer
    bool _PeerEverReady;       // rising-edge latch for the peer-link notice
    bool _PrevPeerReady;       // feedback: link-ESTABLISHED edge for the solo->linked auto-switch
    int _AiW[Rps::AiTierCount], _AiL[Rps::AiTierCount], _AiD[Rps::AiTierCount];
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
    _LastTick = 0xFFFFFFFFu;
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

    // #73: note every activation; renderFrame reattaches if the renderer was born
    // while the app wasn't active (the black-screen precondition).
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(onBecameActive)
                                                 name:UIApplicationDidBecomeActiveNotification
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
    // All-time W-L-D per AI tier / per rival. Seeded into the view's rows here so the dropdown shows
    // the real record on its first open, not 0-0-0 until this session's first match resolves.
    _Scores.Load(*_Store);
    for (int T = 0; T < Rps::AiTierCount; ++T) {
        const Rps::Tally S = _Scores.Ai(T);
        _AiW[T] = static_cast<int>(S.Wins);
        _AiL[T] = static_cast<int>(S.Losses);
        _AiD[T] = static_cast<int>(S.Draws);
        _View.SetAiScore(T, _AiW[T], _AiL[T], _AiD[T]);
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

    // #2: open straight into a match vs the Easy AI (renderFrame consumes this on its first tick).
    // The player can pick another tier — or the linked opponent — from the selector at any time.
    _PendingTier = static_cast<int>(Rps::EAiTier::Easy);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat Scale = Layer.contentsScale;
    Layer.drawableSize = CGSizeMake(self.view.bounds.size.width * Scale,
                                    self.view.bounds.size.height * Scale);
    if (Layer.drawableSize.width == 0 || Layer.drawableSize.height == 0) return;

    if (!_Ready) {
        _Renderer = Lur::Render::VulkanRenderer::Create();
        _Ready = _Renderer && _Renderer->Init((__bridge void*)Layer);
        // #73 precondition check: a renderer initialised while the app is NOT active
        // ends up presenting into a layer the window server never composites.
        _InitWhileInactive =
            UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
        os_log(OS_LOG_DEFAULT, "OnlyRps: Renderer init: %{public}s (drawable %dx%d, appActive=%d)",
               _Ready ? "ok" : "failed", (int)Layer.drawableSize.width,
               (int)Layer.drawableSize.height, _InitWhileInactive ? 0 : 1);
        if (_Ready) {
            _View.CreateResources(_Renderer);
            _DisplayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(renderFrame)];
            [_DisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode];
        }
    } else {
        _Renderer->Resize(static_cast<int>(Layer.drawableSize.width),
                          static_cast<int>(Layer.drawableSize.height));
    }
}

- (void)onBecameActive { _BecameActive = true; }  // handled on the next renderFrame

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
    if (!_SoloActive) return;
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
            const Rps::InputEvent E = Rps::InputEvent::Place(
                _Team, static_cast<uint8_t>(Cmd.C & 3), Rps::F(Cmd.A), Rps::F(Cmd.B));
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT place type=%d at (%d,%d) team=%u", Cmd.C, Cmd.A,
                   Cmd.B, static_cast<unsigned>(_Team));
            [self warnIfSolo:"place"];
            [self placeLocal:E];
            break;
        }
        case Rps::EAgentCmd::Queue:
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT queue slot=%d count=%d", Cmd.A, Cmd.B);
            [self warnIfSolo:"queue"];
            [self placeLocal:Rps::InputEvent::Queue(_Team, Cmd.A, Cmd.B)];
            break;
        case Rps::EAgentCmd::Stress: {
            Rps::Sim& Sm = _SoloActive ? _SoloSim : const_cast<Rps::Sim&>(_Lp.GetSim());
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
        case Rps::EAgentCmd::Gesture: {
            // #151's real subject: drive the SHARED recognizer with a synthetic two-finger triple-tap.
            // It proves the recognizer plus its wiring to SetDevOverlayOpen — the piece that was simply
            // absent on iOS — without needing touch injection, which iOS does not offer at all.
            const uint64_t T0 = NowNs();
            bool Opened = false;
            for (int Tap = 0; Tap < Rps::AgentGestureTaps; ++Tap) {
                const uint64_t Down = T0 + static_cast<uint64_t>(Tap) * 200'000'000ull;
                _DevGesture.PointersDown(1, Down);
                _DevGesture.PointersDown(2, Down);
                Opened = _DevGesture.LiftAndShouldOpen(Down + 40'000'000ull);
            }
            os_log(OS_LOG_DEFAULT, "OnlyRps: AGENT gesture -> recognizer says open=%d", Opened ? 1 : 0);
            if (Opened) _View.SetDevOverlayOpen(true);
            break;
        }
        case Rps::EAgentCmd::KillOwn: {
            // #160 setup: free the ground under our own camp so it can be rebuilt on the same square —
            // the only route to a produced placement whose coordinates equal the opening camp's.
            Rps::Sim& Sm = const_cast<Rps::Sim&>(_Lp.GetSim());
            for (int32_t I = 0; I < Sm.Count; ++I) {
                if (!Sm.IsAlive(I) || Sm.Team[I] != _Team) continue;
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
            _SwitchToLinked = true;
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
    _LinkedRec.Begin(_LinkedRecFile.c_str(), _Lp.GetSim(), /*tier*/ -1, _Team);
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
    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: view unhosted - rebuilding "
                           "window+view+layer+renderer on scene state=%ld",
           (long)Scene.activationState);
    // Detach the OLD window FIRST: it still holds rootViewController == self, and its
    // later dealloc tears the root VC's view out of whatever window now hosts it —
    // which re-unhosted the fresh view and made this reattach loop every 2 s.
    RpsAppDelegate* Delegate = (RpsAppDelegate*)UIApplication.sharedApplication.delegate;
    UIWindow* Old = Delegate.window;
    Old.hidden = YES;
    Old.rootViewController = nil;
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

    _Renderer->Shutdown();  // full teardown (device, surface, everything)
    _Ready = _Renderer->Init((__bridge void*)Layer);
    _InitWhileInactive =
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
    os_log(OS_LOG_DEFAULT, "OnlyRps: #73 reattach: re-init %{public}s (drawable %dx%d, appActive=%d)",
           _Ready ? "ok" : "FAILED", (int)Layer.drawableSize.width,
           (int)Layer.drawableSize.height, _InitWhileInactive ? 0 : 1);
    if (_Ready) _View.CreateResources(_Renderer);
}

- (void)renderFrame {
    // #73, measured 2026-07-19: after a DVT kill-existing relaunch the app can run
    // its render loop with the VIEW IN NO WINDOW (view.window == nil, layer parented
    // nowhere — HEARTBEAT win=0 host=0) while presents "succeed" into the orphan
    // layer. On iOS 13+ a window made with initWithFrame relies on legacy adoption
    // into the implicit UIWindowScene, and this launch path never adopts it. The
    // condition below is precise (never true in health) and the heal is scene-aware,
    // so it is always-on, retried until a scene exists to attach to.
    static uint32_t FramesSinceAttach = 0;
    if (_Ready && (self.view.window == nil || self.view.window.windowScene == nil)) {
        if (++FramesSinceAttach >= 120) {  // retry every ~2 s, not every frame
            FramesSinceAttach = 0;
            [self reattachForActivation];
        }
    } else {
        FramesSinceAttach = 0;
    }
    if (_BecameActive) {
        _BecameActive = false;
        if (_Ready && _InitWhileInactive) [self reattachForActivation];
    }
    if (!_Ready) return;
    const double Now = CACurrentMediaTime();
    const uint64_t ElapsedNs = _PrevFrameTime > 0.0 ? static_cast<uint64_t>((Now - _PrevFrameTime) * 1e9) : 0;
    _PrevFrameTime = Now;
#if LUR_AGENT
    // Poll the agent channel FIRST, and unconditionally — it must be serviced whatever mode the app is
    // in. Putting it inside the linked branch (as this first was) makes it unreachable in solo, which
    // is the mode the app OPENS in, so a scenario could not be driven until a peer appeared.
    [self pollAgentChannel:ElapsedNs];
#endif

    // #2/#127: consume a selector tier pick -> (re)start a solo AI match at once (even mid-match).
    // App-open sets Easy, so a match is live immediately (the empty pre-match sim that showed no
    // base/mines/frontier is gone).
    if (_PendingTier >= 0) {
        _SoloSim.Init(kMatchSeed);
        _SoloAi.Init(kMatchSeed, /*AI team*/ 1, static_cast<Rps::EAiTier>(_PendingTier));
        _SoloActive = true; _SoloTier = _PendingTier; _SoloScored = false; _SoloPostNs = 0;
        _SoloAccumNs = 0; _LastTick = 0xFFFFFFFFu; _Started = false; _Team = 0; _CamInit = false;
        _SoloPending.clear();
        os_log(OS_LOG_DEFAULT, "OnlyRps: solo AI match (re)started (tier %d)", _PendingTier);
#if LUR_INTERNAL
        [self soloRecBegin:_PendingTier];   // one recording per match, opened as the match opens
#endif
        _PendingTier = -1;
    }

    // Pump the session ALWAYS (even during solo) so a real peer can complete the handshake — that
    // raises the "opponent link established" notice + the Linked-opponent row.
    _Session.Tick(ElapsedNs);
    const bool PeerReady = _Session.IsReady();
    if (PeerReady && !_PeerEverReady) {
        _PeerEverReady = true;
        // Peer's device id as the row label (#178) — see the matching note in RpsMain.cpp.
        _View.SetLinked(true, _Session.GetPeerGuid());
        _View.NotifyPeerLinked();    // blink the bar
    }
    // Every frame, not just the link edge — see the matching note in RpsMain.cpp.
    _View.SetBuildMismatch(_Lp.BuildMismatch());
    _View.SetLinkHalfOpen(_Session.IsLinkHalfOpen());  // #163: "LINK STALLED" banner (single-threaded here)
    // AUTO-switch solo -> linked, on the EDGE where the link establishes and ONLY out of an AI match
    // the player has not started (no mine camp dragged in yet). Two freshly opened phones therefore
    // pair and front-load the match with zero taps, and because both peers switch on the same
    // Session-ready edge they enter lockstep within ~a frame of each other — the proven direct-link
    // timing (a manual tap drifted the two Lp.Init calls seconds apart, so the peer that switched
    // first ran lockstep alone and desynced at tick 10, #147).
    //
    // Never auto-switch out of a STARTED AI match (that would destroy a game in progress), and never
    // out of a linked match, started or not: _SoloActive is false there, and a re-link after a blip
    // must resync rather than re-Init. The selector stays the deliberate way across, from ANY state.
    const bool LinkEdge = PeerReady && !_PrevPeerReady;
    _PrevPeerReady = PeerReady;
    // #170: the manual pick LATCHES until it can be honoured. It used to be cleared every frame
    // regardless, so a pick that arrived while the link was still coming up was silently dropped —
    // invisible to a human (the row only exists once a peer is linked) but the normal case for a
    // harness, which sends `linked` the moment it launches the app. The flag is only cleared when the
    // switch actually happens, or when there is nothing left to switch out of.
    const bool ManualPick = _SwitchToLinked;
    const bool AutoSwitch = LinkEdge && !_SoloSim.HasMinerCamp(0);   // unstarted AI match only
    if (_SoloActive && PeerReady && (AutoSwitch || ManualPick)) {
        _SoloActive = false;
        _SwitchToLinked = false;
        _Team = 0;
        _View.SelectLinkedOpponent();   // name the peer in the HUD instead of the AI tier
        os_log(OS_LOG_DEFAULT, "OnlyRps: switch solo -> linked (%{public}s)",
               AutoSwitch ? "auto: link established, AI match not started"
                          : "player picked the linked opponent");
    } else if (ManualPick && !_SoloActive) {
        _SwitchToLinked = false;   // already linked: moot
    }

    if (_SoloActive) {
        // PRE-MATCH HOLD, mirroring the linked path (#139/#149): until you place your opening camp
        // the clock does not run, and your camp and the AI's land in the SAME tick — as two peers
        // both apply their camps at tick 0. Elapsed time while held is dropped, not banked.
        //
        // Gated on the match being ONGOING as well: after a loss the player's camps are usually gone,
        // and without that test this branch would sit there trying to open a match that is already
        // decided. (On Android the same confusion was worse — the equivalent gate `continue`d past the
        // result handling entirely and stuck the player on "you lose".)
        if (_SoloSim.Result == Rps::ResultOngoing && !_SoloSim.HasMinerCamp(0)) {
            _SoloAccumNs = 0;
            Rps::InputEvent Evs[Rps::MaxEventsPerTick];
            int Kept = 0;
            for (const Rps::InputEvent& E : _SoloPending)
                if (Kept < Rps::MaxEventsPerTick && E.Kind == Rps::EventPlaceBuilding &&
                    E.Type == Rps::UnitMiner &&
                    _SoloSim.CanPlaceBuilding(0, Rps::UnitMiner, Rps::Fixed{E.X}, Rps::Fixed{E.Y}))
                    Evs[Kept++] = E;
            _SoloPending.clear();   // pre-match only the camp counts; the rest is dropped
            if (Kept > 0) {
                int AiCount = 0;
                _SoloAi.DecideEvents(_SoloSim, _SoloSim.Tick, Evs + Kept,
                                     Rps::MaxEventsPerTick - Kept, AiCount);
                _SoloSim.StepEvents(Evs, Kept + AiCount);
#if LUR_INTERNAL
                // The opening camps are ONE tick's batch (yours and the AI's together) and it is the
                // tick the whole replay is anchored on — dropping it would leave a recording whose
                // first event is unexplained.
                _SoloRec.Events(_SoloSim.Tick - 1, Evs, Kept + AiCount);
#endif
            }
        } else {
        _SoloAccumNs += ElapsedNs;
        while (_SoloAccumNs >= kStepNs) {   // fixed 10 Hz
            _SoloAccumNs -= kStepNs;
            Rps::InputEvent Evs[Rps::MaxEventsPerTick];
            int Count = 0;
            for (const Rps::InputEvent& E : _SoloPending)
                if (Count < Rps::MaxEventsPerTick) Evs[Count++] = E;
            _SoloPending.clear();
            {
                int AiCount = 0;
                _SoloAi.DecideEvents(_SoloSim, _SoloSim.Tick, Evs + Count, Rps::MaxEventsPerTick - Count, AiCount);
                Count += AiCount;
            }
            _SoloSim.StepEvents(Evs, Count);
#if LUR_INTERNAL
            // #144: the COMBINED batch (yours + the AI's), recorded at the tick it was applied on, so
            // a dev machine replays this match bit-for-bit (Rps::ReplayMatch).
            _SoloRec.Events(_SoloSim.Tick - 1, Evs, Count);
#endif
        }
#if LUR_INTERNAL
        // #144 telemetry: a census every 2 s into the recording AND the syslog, so the match is
        // readable live and replayable afterwards. The AI's own state + countered type go in it — a
        // recording that shows only what it BUILT cannot tell "mis-countered" from "production-bound".
        _RecCensusNs += ElapsedNs;
        if (_RecCensusNs >= 2'000'000'000ull) {
            _RecCensusNs = 0;
            _SoloRec.Census(_SoloSim, /*human*/ 0, static_cast<int>(_SoloAi.State()),
                            static_cast<int>(_SoloAi.CounterEnemy()));
        }
#endif
        }
        if (!_SoloScored && _SoloSim.Result != Rps::ResultOngoing && _SoloTier >= 0) {
            _SoloScored = true;
#if LUR_INTERNAL
            // Finalise BEFORE the tally, exactly as Android does: a recording without its `end` line
            // replays as an abandoned match, and the last census is what makes the final state
            // readable without replaying at all.
            if (_SoloRec.IsOpen()) {
                _SoloRec.Census(_SoloSim, 0, static_cast<int>(_SoloAi.State()),
                                static_cast<int>(_SoloAi.CounterEnemy()));
                _SoloRec.End(_SoloSim);
                os_log(OS_LOG_DEFAULT, "OnlyRps: REC match finished: result=%u tick=%u -> %{public}s",
                       static_cast<unsigned>(_SoloSim.Result), _SoloSim.Tick, _SoloRecFile.c_str());
            }
#endif
            // Persist first, then show what was persisted (so the row and the disk cannot disagree).
            _Scores.RecordAi(_SoloTier, _SoloSim.Result, /*MyTeam*/ 0);
            _Scores.Save(*_Store);
            const Rps::Tally T = _Scores.Ai(_SoloTier);
            _AiW[_SoloTier] = static_cast<int>(T.Wins);
            _AiL[_SoloTier] = static_cast<int>(T.Losses);
            _AiD[_SoloTier] = static_cast<int>(T.Draws);
            _View.SetAiScore(_SoloTier, _AiW[_SoloTier], _AiL[_SoloTier], _AiD[_SoloTier]);
        }
        // #149: hold the win/lose screen, then begin a FRESH match at the same tier from Seed+1 —
        // back in the pre-match state (the AI waits for your camp; the camera re-locks itself).
        if (_SoloSim.Result != Rps::ResultOngoing) {
            _SoloPostNs += ElapsedNs;
            if (_SoloPostNs >= Rps::PostMatchHoldNs && _SoloTier >= 0) {
                const uint64_t NextSeed = _SoloSim.Seed + 1;
                _SoloSim.Init(NextSeed);
                _SoloAi.Init(NextSeed, /*AI team*/ 1, static_cast<Rps::EAiTier>(_SoloTier));
                _SoloScored = false; _SoloPostNs = 0; _SoloAccumNs = 0;
                _SoloPending.clear();
                os_log(OS_LOG_DEFAULT, "OnlyRps: solo next match begins (tier %d)", _SoloTier);
#if LUR_INTERNAL
                [self soloRecBegin:_SoloTier];   // the auto-restart is a new match: a new recording
#endif
            }
        } else {
            _SoloPostNs = 0;
        }
    } else {
        if (!_Started && PeerReady) {
            const uint8_t Team = _DeviceId < _Session.GetPeerGuid() ? 0 : 1;
            _Team = Team;  // per-player view flip
            _Lp.Init(kMatchSeed, Team, SendViaSession, &_Session);
#if LUR_INTERNAL
            // #147/#112: refuse a mismatched build, and exchange our gameplay-CVar override set
            // so both peers converge on ONE merged set (and one Init-derived state) before tick 0.
            // iOS has no on-device cvars.cfg today, so the seed loop is normally a no-op — but the
            // SEND is not: an empty set is still this peer's half of the exchange, and the loop is
            // in place for the day iOS gets on-device tuning.
            Rps::LockstepPeer* Lp = &_Lp;   // ObjC method: no `this` to capture, take the ivar's address
            Lp->SendFingerprint();
            Lur::Core::CVarRegistry::ForEach([Lp](Lur::Core::ICVar* C) {
                if (!C->AffectsGameplay() || !C->Overridden()) return;
                const int Id = Rps::GameplayIdForName(C->Name());
                if (Id >= 0) Lp->SeedGameplayCvar(static_cast<uint8_t>(Id), C->RawValue(), C->EditWallMs());
            });
            Lp->SendCvarSync();
#endif
            // #148: reconcile on ENTERING the match, not only on a reconnect edge — a freshly
            // launched app never takes that edge, so it never offered its frontier and the peer
            // that kept running sat in Awaiting forever. Harmless for a fresh pair (empty
            // histories, marker F=0 both ways). After Init so Init can't wipe it.
            _Lp.BeginResync();
            _Started = true; _Scored = false; _ScoredIdx = _Lp.MatchIndex();
#if LUR_INTERNAL
            // #159: route every executed tick into the linked recording. The sink is app wiring and
            // survives match restarts, so it is armed once here; the C function pointer takes `self`
            // as its context (the ivars are C++ members of this object).
            _Lp.SetTickSink(
                [](void* C, uint32_t Tick, const Rps::InputEvent* Batch, int Count, uint64_t Hash) {
                    RpsViewController* Vc = (__bridge RpsViewController*)C;
                    [Vc recordLinkedTick:Tick batch:Batch count:Count hash:Hash];
                },
                (__bridge void*)self);
            // #180: and the OPEN is driven by the netcode's match-start edge, not by this class
            // watching MatchStarted() from renderFrame. The poll lost tick 0 whenever the match
            // started while DELIVERING the peer's camp rather than during our own Tick, and tick 0 is
            // the tick carrying both camps — so the file diffed as "EVENTS differ at tick 0 ... look
            // at the transport" on a clean match. Still not at Lp.Init: the header would then snapshot
            // this peer's pre-merge CVar set (see linkedRecBegin).
            _Lp.SetMatchStartSink(
                [](void* C) { [(__bridge RpsViewController*)C linkedRecBegin]; },
                (__bridge void*)self);
#endif
            // The peer's GUID is known now, so show the ALL-TIME record against THIS rival rather
            // than 0-0-0 until the first match of the session ends.
            {
                const Rps::Tally T = _Scores.Peer(_Session.GetPeerGuid(), _DeviceId);
                _PeerW = static_cast<int>(T.Wins);
                _PeerL = static_cast<int>(T.Losses);
                _PeerD = static_cast<int>(T.Draws);
                _View.SetPeerScore(_PeerW, _PeerL, _PeerD);
            }
            os_log(OS_LOG_DEFAULT, "OnlyRps: linked - lockstep started (team %d)", Team);
        }
        if (_Started) _Lp.Tick(ElapsedNs);
        // #161: tell the player a desync repair is in flight — the match holds and may rewind a second
        // of play, which is worse than the freeze it replaced if it happens without explanation.
        _View.SetRecovering(_Started && !_SoloActive && _Lp.Recovering());
        // #149: one Lp spans many matches now (it holds the win screen, then rebuilds), so the
        // tally latch is keyed on the match INDEX — re-armed exactly once per restart.
        if (_Started && _ScoredIdx != _Lp.MatchIndex()) { _ScoredIdx = _Lp.MatchIndex(); _Scored = false; }
        // #2: tally the linked result ONCE (you are _Team) and show the session W-L-D on the peer row.
        if (_Started && !_Scored && _Lp.GetSim().Result != Rps::ResultOngoing) {
            _Scored = true;
            const uint8_t R = _Lp.GetSim().Result;
#if LUR_INTERNAL
            // Close the recording on the RESULT, not at the next Begin: the end line stamps the
            // result and tick, and by the next Begin the sim has been rebuilt for the match after
            // this one. A desync-declared draw (e6d6abf) lands here too, so the file that captured a
            // divergence is always complete.
            if (_LinkedRec.IsOpen()) {
                _LinkedRec.Census(_Lp.GetSim(), _Team, /*no AI*/ -1, -1);
                _LinkedRec.End(_Lp.GetSim());
                os_log(OS_LOG_DEFAULT, "OnlyRps: REC linked match finished: result=%u tick=%u "
                       "desync=%d -> %{public}s", static_cast<unsigned>(R), _Lp.GetSim().Tick,
                       _Lp.Desynced() ? 1 : 0, _LinkedRecFile.c_str());
            }
#endif
            // Per-rival and persistent, keyed on their device GUID. RecordPeer refuses a malformed
            // or absent id rather than inventing a rivalry row, so keep the session count in that case.
            const std::string& PeerGuid = _Session.GetPeerGuid();
            if (_Scores.RecordPeer(PeerGuid, _DeviceId, R, _Team)) {
                _Scores.Save(*_Store);
                const Rps::Tally T = _Scores.Peer(PeerGuid, _DeviceId);
                _PeerW = static_cast<int>(T.Wins);
                _PeerL = static_cast<int>(T.Losses);
                _PeerD = static_cast<int>(T.Draws);
            } else {
                os_log(OS_LOG_DEFAULT, "OnlyRps: peer result not persisted (peer guid %zuB)",
                       PeerGuid.size());
                if (R == Rps::ResultDraw) ++_PeerD;
                else if ((R == Rps::ResultTeam0Wins && _Team == 0) || (R == Rps::ResultTeam1Wins && _Team == 1)) ++_PeerW;
                else ++_PeerL;
            }
            _View.SetPeerScore(_PeerW, _PeerL, _PeerD);
        }
    }

#if LUR_INTERNAL
    // Dev build: log the lockstep tick/desync every ~2 s so sync is observable from
    // syslog. (The bring-up autoplay is gone — phones are for HUMAN playtesting now;
    // the desktop's --auto flag covers soak needs.)
    static uint64_t DiagAccumNs = 0;
    // Always-on render-health heartbeat (#73) — deliberately NOT gated on the link:
    // today's diagnosis was blinded because every periodic line needed _Started.
    static uint64_t BeatAccumNs = 0;
    BeatAccumNs += ElapsedNs;
    if (BeatAccumNs > 2'000'000'000ull) {
        BeatAccumNs = 0;
        // win/key/scene/host: hunting an in-process signal for the never-composited
        // state (#73). scene: UISceneActivationState (0=fg-active). host: root layer
        // parented into the window's layer tree.
        UIWindow* Win = self.view.window;
        os_log(OS_LOG_DEFAULT,
               "OnlyRps: HEARTBEAT presented=%u appActive=%d linked=%d win=%d key=%d scene=%ld "
               "host=%d scenes=%lu",
               _Renderer != nullptr ? _Renderer->PresentedFrames() : 0u,
               UIApplication.sharedApplication.applicationState == UIApplicationStateActive ? 1 : 0,
               _Started ? 1 : 0, Win != nil ? 1 : 0, Win.isKeyWindow ? 1 : 0,
               (long)(Win.windowScene != nil ? Win.windowScene.activationState : -99),
               self.view.layer.superlayer != nil ? 1 : 0,
               (unsigned long)UIApplication.sharedApplication.connectedScenes.count);
    }
    if (_Started || _SoloActive) {
        DiagAccumNs += ElapsedNs;
        if (DiagAccumNs > 2'000'000'000ull) {
            DiagAccumNs = 0;
            // presented= distinguishes "rendering but invisible" from a dead swapchain
            // (issue #73): black screen + advancing count = compositor problem; stuck
            // count = the renderer itself isn't presenting.
            // #147: hash + gold + frontier are the CONVERGENCE readout — pre-match they MUST match
            // the peer's line exactly. The anchor cross-check only begins once the match does, so
            // before either camp is placed a divergence was otherwise invisible.
            const Rps::Sim& DS = _SoloActive ? _SoloSim : _Lp.GetSim();
            // badbuild= mirrors Android's: #112 detects a build-fingerprint mismatch and sets
            // BuildMismatch(), but nothing read it and its own log line had no sink — so when this
            // pair desynced on 2026-07-30, "were the two builds even the same?" was unanswerable.
            // #163: gaps/gapat/stall, matching Android's line field for field — the pair is only
            // readable side by side, and this direction (peripheral -> central) is the one that went
            // half-open, so the iPhone's copy is the more important of the two. gaps>0 names a frame
            // the link dropped without reporting an error; stall=1 names the pre-match hang that
            // otherwise looks exactly like a frozen app.
            os_log(OS_LOG_DEFAULT, "OnlyRps: %{public}s tick=%u you=%d foe=%d desync=%d badbuild=%d presented=%u "
                   "hash=%08x gold=%d frontier=%d started=%d gaps=%d gapat=%u stall=%d halfopen=%d restarts=%d",
                   _SoloActive ? "SOLO" : "LOCKSTEP", DS.Tick, DS.AliveCount(0), DS.AliveCount(1),
                   _SoloActive ? 0 : (_Lp.Desynced() ? 1 : 0),
                   _Lp.BuildMismatch() ? 1 : 0,
                   _Renderer != nullptr ? _Renderer->PresentedFrames() : 0u,
                   static_cast<uint32_t>(DS.StateHash() & 0xFFFFFFFFu),
                   DS.Teams[_Team].Gold, DS.FrontierT0.ToInt(),
                   _SoloActive ? 0 : (_Lp.MatchStarted() ? 1 : 0),
                   _SoloActive ? 0 : _Lp.InputGaps(), _SoloActive ? 0u : _Lp.LastInputGapTick(),
                   (!_SoloActive && _Lp.PreMatchStalled()) ? 1 : 0,
                   // #163: half-open verdict, matching Android field-for-field (the pair is read side
                   // by side). This peripheral->central direction is the one that wedges, so the
                   // iPhone's halfopen= is the more important of the two.
                   (!_SoloActive && _Session.IsLinkHalfOpen()) ? 1 : 0,
                   // #182: hard radio restarts fired this half-open episode (capped). The iPhone is the
                   // peripheral whose notify path wedges, so THIS is the side where the escalation runs
                   // — a climbing restarts= here is the on-device proof the fix fired.
                   _SoloActive ? 0 : _Session.RadioRestartsAttempted());
            // #159: the linked recording's periodic census rides this same 2 s beat. It carries the
            // economy snapshot AND it is what FLUSHES the file — without it the capture sits in the
            // stdio buffer until End, so a killed app or a match that never resolves leaves nothing
            // on disk, which is the exact failure this recorder exists to survive.
            if (!_SoloActive) _LinkedRec.Census(_Lp.GetSim(), _Team, -1, -1);
        }
    }
#endif

    CAMetalLayer* Layer = [self metalLayer];
    const float W = static_cast<float>(Layer.drawableSize.width);
    const float H = static_cast<float>(Layer.drawableSize.height);
    const uint64_t Stamp = NowNs();
    const Rps::Sim& ActiveSim = _SoloActive ? _SoloSim : _Lp.GetSim();  // #127 solo or #76 linked
    if (ActiveSim.Tick != _LastTick) { _LastTick = ActiveSim.Tick; _TickLandedNs = Stamp; }
    _Snap.CaptureFrom(ActiveSim, _TickLandedNs, kStepNs);
    const float VisibleH = H / Ppu(W);
    const float FieldMax = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
    // OS safe areas (#85 feedback): notch/status bar above the HUD, home indicator
    // below the plates. Points -> pixels via the layer scale.
    const CGFloat SaScale = [self metalLayer].contentsScale;
    const UIEdgeInsets Sa = self.view.safeAreaInsets;
    _View.SetInsets(static_cast<float>(Sa.top * SaScale), static_cast<float>(Sa.bottom * SaScale));
    const float MaxCam = FieldMax + _View.TopHudWorldUnits(W);
    const float MinCam = -_View.BottomHudWorldUnits(W);
    if (!_CamInit) { _Cam.Y = MinCam; _CamInit = true; }
    // Camera LOCKED at the baseline until you place your first mining camp (feedback) — free scroll after.
    if (!_Snap.HasMinerCamp(_Team)) _Cam.Y = MinCam;
    else _Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam, MinCam);  // momentum + clamp
    // #139/feedback: your camp, committed and waiting on the opponent's — it is NOT in the sim yet
    // (both camps become tick 0's input together), so without this the field looked empty right
    // after the drop. Single-threaded here, so read the peer directly. Never in solo: there the
    // place applies immediately and the real camp is in the snapshot.
    {
        const bool Pend = !_SoloActive && _Started && _Lp.HasLocalCamp() && !_Lp.MatchStarted();
        constexpr float FixedOne = static_cast<float>(Rps::Fixed::One);
        _View.SetPendingCamp(Pend,
                             static_cast<float>(_Lp.LocalCamp().X) / FixedOne,
                             static_cast<float>(_Lp.LocalCamp().Y) / FixedOne);
    }
    _View.Render(_Renderer, _Snap, _Snap.AlphaAt(Stamp), _Cam.Y, W, H, _Team == 1,
                 static_cast<float>(ElapsedNs) / 1.0e9f);
}

// Touch (#139/#140, mirror of the desktop/Android mains): a touch-down on a build plate starts a
// drag-to-place (the ghost follows the finger, lifted up-left of it by ~its size so the thumb
// doesn't hide it; a valid release emits a Place event); any other drag pans the camera; a tap on
// a building's x1/x5 button queues units. Single-threaded here, so Lp.QueueLocalEvent is called
// directly. Placement is gated on _Started (a live match). You play _Team.
- (float)ghostOffPxForWidth:(float)W {
    return static_cast<float>(_Snap.Cv.BuildingFootprint.Raw) / static_cast<float>(Rps::Fixed::One) * 0.5f * Ppu(W);
}
// Route a local place/queue event to whichever match is live: the solo sim's pending queue (drained
// in renderFrame's solo tick) or the linked peer's inbox. Single-threaded, so both are safe here.
- (void)placeLocal:(Rps::InputEvent)E {
    if (_SoloActive) _SoloPending.push_back(E);
    else if (_Started) _Lp.QueueLocalEvent(E);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat S = Layer.contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    const float X = static_cast<float>(P.x * S), Y = static_cast<float>(P.y * S);
    _DownX = X; _DownY = Y;
#if !LUR_SHIPPING
    // #151: the console. It was never wired here at all — the two-finger triple-tap that opens it on
    // Android did nothing on the iPhone, so on-device tuning was Android-only and the iPhone half of a
    // two-phone playtest was un-tunable (playtest 2026-07-25). The recognizer is the SHARED one
    // (Lur::Input::ConsoleGesture), not a third hand-written copy: the existing copies had already
    // drifted three ways, which is how this one came to be missing.
    _DevGesture.PointersDown(static_cast<int>(event.allTouches.count), NowNs());
    // While the console is open it OWNS the pointer — a drag anywhere scrolls the cvar list and a
    // still release is a DevTap. Swallowing the gesture is the point: the panel sits over a LIVE
    // match, so a scroll must not leak through and pan the camera or start a building drag.
    if (_View.DevOverlayOpen()) { _DevGesture.DragBegin(Y); return; }
#endif
    const float W = static_cast<float>(Layer.drawableSize.width);
    const float Off = [self ghostOffPxForWidth:W];
    const bool Live = _SoloActive || _Started;
    const int Plate = Live ? _View.PlateAt(X, Y) : -1;  // plate hit-test at the real finger
    if (Plate >= 0) {
        _View.BeginPlaceDrag(Plate, X - Off, Y - Off);  // sets the ghost type; seed at the offset spot
        const float H = static_cast<float>(Layer.drawableSize.height);
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        const bool V = _View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, _Team == 1, _Snap, _Team, Wx, Wy, Gsx, Gsy);
        // Finger point AND snapped point: ghost on the finger, snap eased (visual only).
        _View.UpdatePlaceDrag(X - Off, Y - Off, Gsx, Gsy, V);
    } else {
        // #107: a press on an x1/x5 button lights up NOW; the enqueue still commits on release
        // (touchesEnded), so a press that turns into a camera pan queues nothing.
        _View.PressProductionButton(X, Y);
        _Cam.Begin(Y);
    }
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat S = Layer.contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    const float X = static_cast<float>(P.x * S), Y = static_cast<float>(P.y * S);
    const float W = static_cast<float>(Layer.drawableSize.width), H = static_cast<float>(Layer.drawableSize.height);
#if !LUR_SHIPPING
    if (_View.DevOverlayOpen()) { _View.DevScroll(_DevGesture.DragMove(Y)); return; }  // #151
#endif
    if (_View.IsPlacing()) {
        const float Off = [self ghostOffPxForWidth:W];
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        const bool V = _View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, _Team == 1, _Snap, _Team, Wx, Wy, Gsx, Gsy);
        _View.UpdatePlaceDrag(X - Off, Y - Off, Gsx, Gsy, V);
        _DevGesture.Cancel();  // #151: a placement drag is not a console tap
    } else {
        _Cam.Move(Y, Ppu(W));  // content-drag pans the camera
    }
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat S = Layer.contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    const float X = static_cast<float>(P.x * S), Y = static_cast<float>(P.y * S);
    const float W = static_cast<float>(Layer.drawableSize.width), H = static_cast<float>(Layer.drawableSize.height);
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
        bool Placed = false;
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        if (_View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, _Team == 1, _Snap, _Team, Wx, Wy, Gsx, Gsy)) {
            [self placeLocal:Rps::InputEvent::Place(_Team, static_cast<uint8_t>(_View.PlacingType()),
                                                    WorldToFixed(Wx), WorldToFixed(Wy))];
            Placed = true;
        }
        _View.EndPlaceDrag(Placed);  // valid -> the real building takes over; else slide back
        _DevGesture.Cancel();        // #151
        return;
    }
    _Cam.End();
#if !LUR_SHIPPING
    // #151: two-finger triple-tap OPENS the console, with the same windows Android uses because it is
    // the same recognizer. `event.allTouches` is empty by the time the last touch ends, so the count
    // was captured in touchesBegan — the candidate is armed there and merely resolved here.
    const bool WasTwoFinger = _DevGesture.TwoFingerActive();
    if (_DevGesture.LiftAndShouldOpen(NowNs())) {
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
            _PendingTier = Tier;                                   // (re)start solo at this tier (#2)
        } else if (_View.TakePeerPick()) {
            _SwitchToLinked = true;                                // switch to the linked peer (#2)
        } else if ((_SoloActive || _Started) && Hit == -1) {
            int32_t Slot = -1;
            const int Cnt = _View.OnProductionButton(X, Y, Slot);
            if (Cnt > 0) [self placeLocal:Rps::InputEvent::Queue(_Team, Slot, Cnt)];
        }
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
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([RpsAppDelegate class]));
    }
}
