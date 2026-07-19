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

#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Ble.h"
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
    _Session.SetResyncHandler([Lp] { Lp->BeginResync(); });
    _Session.Start(_Transport, _DeviceId);
    os_log(OS_LOG_DEFAULT, "OnlyRps: session started (device id %zuB)", _DeviceId.size());
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

    _Session.Tick(ElapsedNs);  // pump the BLE inbox + handshake/liveness
    if (!_Started && _Session.IsReady()) {
        const uint8_t Team = _DeviceId < _Session.GetPeerGuid() ? 0 : 1;
        _Team = Team;  // per-player view flip
        _Lp.Init(kMatchSeed, Team, SendViaSession, &_Session);
        _Started = true;
        _View.SetLinked(true);  // opponent selector: green dot (#85)
        os_log(OS_LOG_DEFAULT, "OnlyRps: linked - lockstep started (team %d)", Team);
    }
    if (_Started) _Lp.Tick(ElapsedNs);

#if LUR_INTERNAL
    // Dev build: auto-press random soldiers so the cross-platform match plays itself,
    // and log the lockstep tick/desync every ~2 s so sync is observable from syslog.
    static Lur::Sim::SplitMix64 Rng(0xA11CE);
    static uint64_t AutoAccumNs = 0, DiagAccumNs = 0;
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
    if (_Started) {
        AutoAccumNs += ElapsedNs;
        if (AutoAccumNs > 700'000'000ull) {
            AutoAccumNs = 0;
            _Lp.SetLocalMask(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
        }
        DiagAccumNs += ElapsedNs;
        if (DiagAccumNs > 2'000'000'000ull) {
            DiagAccumNs = 0;
            // presented= distinguishes "rendering but invisible" from a dead swapchain
            // (issue #73): black screen + advancing count = compositor problem; stuck
            // count = the renderer itself isn't presenting.
            os_log(OS_LOG_DEFAULT, "OnlyRps: LOCKSTEP tick=%u you=%d foe=%d desync=%d presented=%u",
                   _Lp.ExecTick(), _Lp.GetSim().AliveCount(0), _Lp.GetSim().AliveCount(1),
                   _Lp.Desynced() ? 1 : 0, _Renderer != nullptr ? _Renderer->PresentedFrames() : 0u);
        }
    }
#endif

    CAMetalLayer* Layer = [self metalLayer];
    const float W = static_cast<float>(Layer.drawableSize.width);
    const float H = static_cast<float>(Layer.drawableSize.height);
    const uint64_t Stamp = NowNs();
    if (_Lp.ExecTick() != _LastTick) { _LastTick = _Lp.ExecTick(); _TickLandedNs = Stamp; }
    _Snap.CaptureFrom(_Lp.GetSim(), _TickLandedNs, kStepNs);
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
    _Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam, MinCam);  // momentum + clamp
    _View.Render(_Renderer, _Snap, _Snap.AlphaAt(Stamp), _Cam.Y, W, H, _Team == 1);
}

// Touch: the bottom strip is the 4 production buttons; a drag above pans the camera (§9).
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    const CGFloat S = [self metalLayer].contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    _Cam.Begin(static_cast<float>(P.y * S));
    _DownX = static_cast<float>(P.x * S); _DownY = static_cast<float>(P.y * S);
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat S = Layer.contentsScale;
    const float Y = static_cast<float>([touches.anyObject locationInView:self.view].y * S);
    _Cam.Move(Y, Ppu(static_cast<float>(Layer.drawableSize.width)));  // content-drag
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    _Cam.End();
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat S = Layer.contentsScale;
    const CGPoint P = [touches.anyObject locationInView:self.view];
    const float X = static_cast<float>(P.x * S), Y = static_cast<float>(P.y * S);
    const float W = static_cast<float>(Layer.drawableSize.width), H = static_cast<float>(Layer.drawableSize.height);
    (void)W; (void)H;
    const bool Tap = (X - _DownX) * (X - _DownX) + (Y - _DownY) * (Y - _DownY) < (24.0f * 24.0f);
    if (Tap && _Started) {
        // HUD first (#85): production plates press units; the opponent selector
        // consumes its own taps; world taps do nothing (drag pans).
        const int Plate = _View.OnTap(X, Y);
        if (Plate >= 0) _Lp.SetLocalMask(static_cast<uint8_t>(1u << Plate));
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
