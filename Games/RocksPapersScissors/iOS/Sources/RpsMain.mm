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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Lur/Core/CVar.h"   // #147: registry walk for the gameplay-CVar sync seed
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
    std::string _DeviceId;
    Rps::Snapshot _Snap;
    bool _Started;
    uint32_t _LastTick;
    uint64_t _TickLandedNs;
    Rps::CameraScroll _Cam;
    bool _CamInit;
    float _DownX, _DownY;
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
    // #2 session score vs the linked peer (session-scoped) + a one-shot tally latch.
    int _PeerW, _PeerL, _PeerD;
    bool _Scored;
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
    int _AiW[3], _AiL[3], _AiD[3];
}

- (void)loadView {
    self.view = [[RpsView alloc] initWithFrame:UIScreen.mainScreen.bounds];
}
- (CAMetalLayer*)metalLayer { return (CAMetalLayer*)self.view.layer; }

- (void)viewDidLoad {
    [super viewDidLoad];
    _LastTick = 0xFFFFFFFFu;

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
    _Store = new Lur::Save::Store(std::string(Dir.UTF8String));
    _DeviceId = Lur::Save::LoadOrCreateDeviceId(*_Store);

    _Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Peripheral);
    _Session.SetLogger([](const char* M) { os_log(OS_LOG_DEFAULT, "OnlyRps: Net: %{public}s", M); });

    Rps::LockstepPeer* Lp = &_Lp;
    _Session.SetHandler(Rps::MsgInput,
                        [Lp](const uint8_t* D, std::size_t N) { Lp->OnMessage(Rps::MsgInput, D, N); });
    _Session.SetHandler(Rps::MsgAnchor,
                        [Lp](const uint8_t* D, std::size_t N) { Lp->OnMessage(Rps::MsgAnchor, D, N); });
    _Session.SetHandler(Rps::MsgResyncChunk,
                        [Lp](const uint8_t* D, std::size_t N) { Lp->OnMessage(Rps::MsgResyncChunk, D, N); });
#if LUR_INTERNAL
    // #147: the dev-only gameplay-CVar sync + build fingerprint (#112). These were wired on the
    // ANDROID peer only, so an iPhone silently DROPPED the Android's MsgCvarSync — the two peers
    // then simulated on different Cv (and different Cv-derived initial state) and desynced at the
    // first anchor. A sync is worthless one-sided; every peer must both send and accept it.
    _Session.SetHandler(Rps::MsgCvar,
                        [Lp](const uint8_t* D, std::size_t N) { Lp->OnMessage(Rps::MsgCvar, D, N); });
    _Session.SetHandler(Rps::MsgCvarSync,
                        [Lp](const uint8_t* D, std::size_t N) { Lp->OnMessage(Rps::MsgCvarSync, D, N); });
    _Session.SetHandler(Rps::MsgFingerprint,
                        [Lp](const uint8_t* D, std::size_t N) { Lp->OnMessage(Rps::MsgFingerprint, D, N); });
#endif
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
        _PendingTier = -1;
    }

    // Pump the session ALWAYS (even during solo) so a real peer can complete the handshake — that
    // raises the "opponent link established" notice + the Linked-opponent row.
    _Session.Tick(ElapsedNs);
    const bool PeerReady = _Session.IsReady();
    if (PeerReady && !_PeerEverReady) {
        _PeerEverReady = true;
        _View.SetLinked(true);       // adds the Linked-opponent row (green dot)
        _View.NotifyPeerLinked();    // blink the bar
    }
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
    const bool ManualPick = _SwitchToLinked;
    _SwitchToLinked = false;
    const bool AutoSwitch = LinkEdge && !_SoloSim.HasMinerCamp(0);   // unstarted AI match only
    if (_SoloActive && PeerReady && (AutoSwitch || ManualPick)) {
        _SoloActive = false;
        _Team = 0;
        _View.SelectLinkedOpponent();   // name the peer in the HUD instead of the AI tier
        os_log(OS_LOG_DEFAULT, "OnlyRps: switch solo -> linked (%{public}s)",
               AutoSwitch ? "auto: link established, AI match not started"
                          : "player picked the linked opponent");
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
        }
        }
        if (!_SoloScored && _SoloSim.Result != Rps::ResultOngoing && _SoloTier >= 0) {
            _SoloScored = true;
            if (_SoloSim.Result == Rps::ResultTeam0Wins) ++_AiW[_SoloTier];
            else if (_SoloSim.Result == Rps::ResultTeam1Wins) ++_AiL[_SoloTier];
            else ++_AiD[_SoloTier];
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
            os_log(OS_LOG_DEFAULT, "OnlyRps: linked - lockstep started (team %d)", Team);
        }
        if (_Started) _Lp.Tick(ElapsedNs);
        // #149: one Lp spans many matches now (it holds the win screen, then rebuilds), so the
        // tally latch is keyed on the match INDEX — re-armed exactly once per restart.
        if (_Started && _ScoredIdx != _Lp.MatchIndex()) { _ScoredIdx = _Lp.MatchIndex(); _Scored = false; }
        // #2: tally the linked result ONCE (you are _Team) and show the session W-L-D on the peer row.
        if (_Started && !_Scored && _Lp.GetSim().Result != Rps::ResultOngoing) {
            _Scored = true;
            const uint8_t R = _Lp.GetSim().Result;
            if (R == Rps::ResultDraw) ++_PeerD;
            else if ((R == Rps::ResultTeam0Wins && _Team == 0) || (R == Rps::ResultTeam1Wins && _Team == 1)) ++_PeerW;
            else ++_PeerL;
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
            os_log(OS_LOG_DEFAULT, "OnlyRps: %{public}s tick=%u you=%d foe=%d desync=%d presented=%u "
                   "hash=%08x gold=%d frontier=%d started=%d",
                   _SoloActive ? "SOLO" : "LOCKSTEP", DS.Tick, DS.AliveCount(0), DS.AliveCount(1),
                   _SoloActive ? 0 : (_Lp.Desynced() ? 1 : 0),
                   _Renderer != nullptr ? _Renderer->PresentedFrames() : 0u,
                   static_cast<uint32_t>(DS.StateHash() & 0xFFFFFFFFu),
                   DS.Teams[_Team].Gold, DS.FrontierT0.ToInt(),
                   _SoloActive ? 0 : (_Lp.MatchStarted() ? 1 : 0));
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
    if (_View.IsPlacing()) {
        const float Off = [self ghostOffPxForWidth:W];
        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
        const bool V = _View.ResolvePlacement(X - Off, Y - Off, _Cam.Y, W, H, _Team == 1, _Snap, _Team, Wx, Wy, Gsx, Gsy);
        _View.UpdatePlaceDrag(X - Off, Y - Off, Gsx, Gsy, V);
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
        return;
    }
    _Cam.End();
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
