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
    float _DownX, _DownY;
    uint8_t _Team;
    CADisplayLink* _DisplayLink;
    double _PrevFrameTime;
    bool _Ready;
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
        os_log(OS_LOG_DEFAULT, "OnlyRps: Renderer init: %{public}s (drawable %dx%d)",
               _Ready ? "ok" : "failed", (int)Layer.drawableSize.width, (int)Layer.drawableSize.height);
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

- (void)renderFrame {
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
        os_log(OS_LOG_DEFAULT, "OnlyRps: linked - lockstep started (team %d)", Team);
    }
    if (_Started) _Lp.Tick(ElapsedNs);

#if LUR_INTERNAL
    // Dev build: auto-press random soldiers so the cross-platform match plays itself,
    // and log the lockstep tick/desync every ~2 s so sync is observable from syslog.
    static Lur::Sim::SplitMix64 Rng(0xA11CE);
    static uint64_t AutoAccumNs = 0, DiagAccumNs = 0;
    if (_Started) {
        AutoAccumNs += ElapsedNs;
        if (AutoAccumNs > 700'000'000ull) {
            AutoAccumNs = 0;
            _Lp.SetLocalMask(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
        }
        DiagAccumNs += ElapsedNs;
        if (DiagAccumNs > 2'000'000'000ull) {
            DiagAccumNs = 0;
            os_log(OS_LOG_DEFAULT, "OnlyRps: LOCKSTEP tick=%u you=%d foe=%d desync=%d", _Lp.ExecTick(),
                   _Lp.GetSim().AliveCount(0), _Lp.GetSim().AliveCount(1), _Lp.Desynced() ? 1 : 0);
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
    const float MaxCam = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
    _Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam);  // momentum + clamp
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
    const bool Tap = (X - _DownX) * (X - _DownX) + (Y - _DownY) * (Y - _DownY) < (24.0f * 24.0f);
    if (Tap && Y >= H * 0.85f && _Started) {
        int Btn = static_cast<int>(X / (W / 4.0f));
        if (Btn < 0) Btn = 0;
        if (Btn > 3) Btn = 3;
        _Lp.SetLocalMask(static_cast<uint8_t>(1u << Btn));
    }
}
@end

@interface RpsAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
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
