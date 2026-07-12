// iOS entry point — the counterpart of Games/Chess/Android/.../AndroidMain.cpp.
//
// A thin UIKit shim: a Metal-backed view (CAMetalLayer) hosts the shared Vulkan
// renderer (via MoltenVK) and drives the shared Chess::BoardView for both drawing
// and touch. All game/render logic lives in the engine + chess::view — identical
// to Android. BLE is brought up too (engine seam), unchanged from the skeleton.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <os/log.h>

#include <string>
#include <vector>

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

// A Metal-backed view: its backing layer is a CAMetalLayer, which MoltenVK turns
// into a Vulkan surface.
@interface OnlyChessView : UIView
@end
@implementation OnlyChessView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface OnlyChessViewController : UIViewController
@end

@implementation OnlyChessViewController {
    Lur::Render::IRenderer* _Renderer;
    Chess::BoardView _View;
    Lur::Transport::ITransport* _Transport;  // owned by its translation unit
    Lur::Net::Session _Session;
    Chess::ChessMatchState _Match;           // authoritative game state
    Lur::Save::Store* _Store;                // heap: lives for the app
    Lur::Save::SyncManager* _Sync;
    std::string _DeviceId;
    CADisplayLink* _DisplayLink;
    bool _Ready;
}

- (void)loadView {
    self.view = [[OnlyChessView alloc] initWithFrame:UIScreen.mainScreen.bounds];
}

- (CAMetalLayer*)metalLayer {
    return (CAMetalLayer*)self.view.layer;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    CAMetalLayer* Layer = [self metalLayer];
    Layer.device = MTLCreateSystemDefaultDevice();
    Layer.pixelFormat = MTLPixelFormatBGRA8Unorm;  // matches the swapchain format
    Layer.contentsScale = UIScreen.mainScreen.scale;

    // Smoke test: the shared, perft-verified C++ core runs on iOS.
    Chess::Board Board = Chess::Board::StartPosition();
    Chess::MoveList Moves;
    Chess::GenerateLegalMoves(Board, Moves);
    os_log(OS_LOG_DEFAULT,
           "OnlyChess: Chess core alive: %d legal moves from the start position", Moves.Count);

    // Persistent device identity (issue #17/#18): the same GUID the BLE role uses,
    // from Application Support. Drives colour + the per-opponent stats key.
    NSArray<NSString*>* Dirs =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* Dir = Dirs.firstObject ?: NSTemporaryDirectory();
    _Store = new Lur::Save::Store(std::string(Dir.UTF8String));
    _DeviceId = Lur::Save::LoadOrCreateDeviceId(*_Store);
    _Sync = new Lur::Save::SyncManager(*_Store, _Match);
    _Match.SetOnMatchEnd([SyncPtr = _Sync, M = &_Match] {
        SyncPtr->Persist();                                          // durable stats on game end
        os_log(OS_LOG_DEFAULT, "OnlyChess: Net: MATCH END result=%d WLD=%d/%d/%d total=%d",
               static_cast<int>(M->LastResult()), static_cast<int>(M->Record().WinsLower),
               static_cast<int>(M->Record().WinsHigher), static_cast<int>(M->Record().Draws),
               static_cast<int>(M->Record().TotalMatches()));
    });
    // Persist the in-progress match when backgrounded, so it survives a close.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(persistState)
                                                 name:UIApplicationDidEnterBackgroundNotification
                                               object:nil];

    // Wire the BLE transport into the net session. Hello exchanges the device GUIDs;
    // the shared BoardView renders + mutates _Match and ships moves via MoveCodec.
    // Colour comes from the two GUIDs, and the per-opponent record syncs once per link.
    _Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Peripheral);
    _Session.SetLogger([](const char* M) {
        os_log(OS_LOG_DEFAULT, "OnlyChess: Net: %{public}s", M);
    });

    auto* Match = &_Match;
    auto* Sync = _Sync;
    auto* Session = &_Session;
    std::string DeviceId = _DeviceId;
    auto SendRecord = [Sync, Session] {
        const std::vector<uint8_t> Snap = Sync->Snapshot();
        Session->Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
    };
    Session->SetReadyHandler([Match, Sync, Session, DeviceId, SendRecord] {
        Match->SetIdentity(DeviceId, Session->GetPeerGuid());  // colour + anchor
        Sync->OnLink(Session->GetPeerGuid());                  // load our stored record
        SendRecord();                                          // let the peer reconcile
    });
    Session->SetResyncHandler(SendRecord);                     // reconnect: re-sync records
    Session->SetHandler(Lur::Net::EMsgType::Sync,
                        [Sync](const uint8_t* D, std::size_t N) { Sync->OnSync(D, N); });

    _View.SetState(&_Match);
    _View.AttachSession(&_Session);
    _Session.Start(_Transport, _DeviceId);
}

// The renderer needs the layer's drawable size, which is only known after layout.
// Initialise lazily here on first valid layout; recreate the swapchain on resize.
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
        os_log(OS_LOG_DEFAULT, "OnlyChess: Renderer init: %{public}s", _Ready ? "ok" : "failed");
        if (_Ready) {
            _View.CreateResources(_Renderer);
            _DisplayLink = [CADisplayLink displayLinkWithTarget:self
                                                       selector:@selector(renderFrame)];
            [_DisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode];
        }
    } else {
        _Renderer->Resize(static_cast<int>(Layer.drawableSize.width),
                          static_cast<int>(Layer.drawableSize.height));
    }
}

- (void)renderFrame {
    if (!_Ready) return;
    _Session.Tick();  // drive the Hello handshake until it completes
    CAMetalLayer* Layer = [self metalLayer];
    _View.Render(_Renderer, static_cast<float>(Layer.drawableSize.width),
                 static_cast<float>(Layer.drawableSize.height));
}

// Backgrounded: persist the in-progress match so it survives a close.
- (void)persistState {
    if (_Sync != nullptr) _Sync->Persist();
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_Ready) return;
    UITouch* Touch = touches.anyObject;
    const CGPoint P = [Touch locationInView:self.view];
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat Scale = Layer.contentsScale;  // points -> pixels (drawable space)
    _View.OnTap(static_cast<float>(P.x * Scale), static_cast<float>(P.y * Scale),
                static_cast<float>(Layer.drawableSize.width),
                static_cast<float>(Layer.drawableSize.height));
}

@end

@interface OnlyChessAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation OnlyChessAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[OnlyChessViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass([OnlyChessAppDelegate class]));
    }
}
