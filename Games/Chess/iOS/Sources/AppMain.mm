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
    double _PrevFrameTime;  // CACurrentMediaTime() at the last renderFrame (0 = first)
    bool _Ready;
#if LUR_INTERNAL
    // Dev-only autoplayer (issue #57/#58/#69), armed by a marker file so the normal
    // .ipa stays untouched. Same-frame instrumentation mirrors AndroidMain.
    bool _AutoEnabled;
    uint32_t _Rng;
    uint64_t _Frame, _PeerReplies, _SameFrame, _NewGameOpens, _DelayedReplies;
    uint64_t _AutoCheckAccumNs, _ReportAccumNs;
    // Net-ms RTT (mirrors AndroidMain): our move leaves -> the peer's reply arrives,
    // measured on this device's clock alone (2x transit + <=1 frame each side).
    uint64_t _ClockNs, _MoveSentNs, _RttCount, _RttSumMs, _RttMinMs, _RttMaxMs;
#endif
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
#if LUR_INTERNAL
    // Dev clear-history (rig-pushed Documents/clearsave): wipe opponent records,
    // their meta sidecars, and the cached peer-id BEFORE the store opens — a fresh
    // pairing state for role/matrix tests. The device-id is KEPT (stable identity).
    // One-shot: the marker is consumed. No pm-clear equivalent exists on iOS.
    {
        NSString* Docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
        NSString* Marker = [Docs stringByAppendingPathComponent:@"clearsave"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:Marker]) {
            NSRegularExpression* Guid =
                [NSRegularExpression regularExpressionWithPattern:@"^[0-9a-fA-F]{32}$" options:0 error:nil];
            int Removed = 0;
            for (NSString* Name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:Dir error:nil]) {
                const bool IsRecord = [Guid numberOfMatchesInString:Name options:0
                                                              range:NSMakeRange(0, Name.length)] > 0;
                if (IsRecord || [Name hasPrefix:@"meta-"] || [Name isEqualToString:@"peer-id"]) {
                    if ([[NSFileManager defaultManager] removeItemAtPath:[Dir stringByAppendingPathComponent:Name]
                                                                   error:nil]) ++Removed;
                }
            }
            [[NSFileManager defaultManager] removeItemAtPath:Marker error:nil];
            os_log(OS_LOG_DEFAULT, "OnlyChess: dev clearsave: removed %d file(s), device-id kept", Removed);
        }
    }
#endif
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

    auto* Sync = _Sync;
    auto* Session = &_Session;
    auto* View = &_View;
    auto SendRecord = [View, Sync, Session] {
        // Only share OUR game with the peer we're actually playing (hijack rule, #38).
        if (View->ActiveOpponentGuid() != Session->GetPeerGuid()) return;
        const std::vector<uint8_t> Snap = Sync->Snapshot();
        Session->Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
    };
    // The view applies the hijack rule and sets identity + loads the record for the
    // adopted peer; we send our record only when it adopted this peer. Both the
    // initial link (ReadyHandler) AND a reconnect (ResyncHandler) go through it, so a
    // peer that (re)joins is adopted per the hijack rule — not just on first contact.
    auto OnLive = [View, Session, SendRecord] {
        if (View->OnPeerLinked(Session->GetPeerGuid())) SendRecord();
    };
    Session->SetReadyHandler(OnLive);
    Session->SetResyncHandler(OnLive);                         // reconnect: re-adopt + re-sync
    Session->SetStateHashFn([M = &_Match] { return M->PositionHash(); });  // desync detection (#72)
    Session->SetHandler(Lur::Net::EMsgType::Sync,
                        [View, Sync, Session](const uint8_t* D, std::size_t N) {
                            if (View->ActiveOpponentGuid() == Session->GetPeerGuid())
                                Sync->OnSync(D, N);
                        });

    _View.SetState(&_Match);
    _View.AttachSession(&_Session);
    _View.AttachPersistence(_Store, _Sync, _DeviceId);     // selector + match switching
    _View.SetLogger([](const char* M) {
        os_log(OS_LOG_DEFAULT, "OnlyChess: View: %{public}s", M);
    });
    _Session.Start(_Transport, _DeviceId);
#if LUR_INTERNAL
    _Rng = 0xC0FFEEu ^ static_cast<uint32_t>(_DeviceId.size());  // per-device autoplay seed
    _RttMinMs = ~0ull;                                           // other RTT ivars zero-init
#endif
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
    const double Now = CACurrentMediaTime();  // monotonic seconds
    const uint64_t ElapsedNs =
        _PrevFrameTime > 0.0 ? static_cast<uint64_t>((Now - _PrevFrameTime) * 1e9) : 0;
    _PrevFrameTime = Now;
#if LUR_INTERNAL
    const bool WasMyTurn = _Match.IsMyTurn();
    const uint32_t MatchesBefore = _Match.Record().TotalMatches();
#endif
    _Session.Tick(ElapsedNs);  // real-time-denominated: drives handshake + liveness (applies peer move)
#if LUR_INTERNAL
    {
        // Arm on first sight of Documents/autoplay (pushed via pymobiledevice3), then
        // play a random legal move the SAME frame it becomes our turn. All tallying is
        // behind _AutoEnabled so a normal (unarmed) build plays by hand with no overhead.
        _AutoCheckAccumNs += ElapsedNs;
        if (!_AutoEnabled && (_Frame == 0 || _AutoCheckAccumNs > 1000000000ull)) {
            _AutoCheckAccumNs = 0;
            NSString* Dir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
            if ([[NSFileManager defaultManager] fileExistsAtPath:[Dir stringByAppendingPathComponent:@"autoplay"]]) {
                _AutoEnabled = true;
                os_log(OS_LOG_DEFAULT, "OnlyChess: autoplay ENABLED (Documents/autoplay present)");
            }
        }
        if (_AutoEnabled) {
            const bool NowMyTurn = _Match.IsMyTurn();
            const uint32_t MatchesAfter = _Match.Record().TotalMatches();
            const bool GotPeerMove = !WasMyTurn && NowMyTurn;
            const bool PeerEndedGame = MatchesAfter != MatchesBefore;
            _ClockNs += ElapsedNs;
            if (GotPeerMove && _MoveSentNs != 0) {      // reply to our outstanding move
                const uint64_t Ms = (_ClockNs - _MoveSentNs) / 1'000'000ull;
                ++_RttCount; _RttSumMs += Ms;
                if (Ms < _RttMinMs) _RttMinMs = Ms;
                if (Ms > _RttMaxMs) _RttMaxMs = Ms;
                _MoveSentNs = 0;
            }
            const bool Played = (_Session.IsReady() && NowMyTurn) ? _View.AutoPlayRandomLegalMove(_Rng) : false;
            if (Played) _MoveSentNs = _ClockNs;         // our move is on the wire; await reply
            if (GotPeerMove) {
                ++_PeerReplies;
                if (PeerEndedGame)   ++_NewGameOpens;
                else if (Played)     ++_SameFrame;
                else               { ++_DelayedReplies;
                    os_log(OS_LOG_DEFAULT, "OnlyChess: WARN our turn, no same-frame reply @%llu", (unsigned long long)_Frame); }
            }
            _ReportAccumNs += ElapsedNs;
            if (_ReportAccumNs > 2000000000ull) {
                _ReportAccumNs = 0;
                // turn/ply/hash/gate: enough context to diagnose a stall from the log
                // alone (#72) — mirrors the Android diag line.
                os_log(OS_LOG_DEFAULT, "OnlyChess: AUTOPLAY game=%u sameFrame=%llu/%llu opens=%llu delayed=%llu "
                       "myTurn=%d ply=%zu hash=%08x gate=%d rtt(n=%llu avg=%llums min=%llums max=%llums)",
                       MatchesAfter, (unsigned long long)_SameFrame, (unsigned long long)_PeerReplies,
                       (unsigned long long)_NewGameOpens, (unsigned long long)_DelayedReplies,
                       _Match.IsMyTurn() ? 1 : 0, _Match.Record().Moves.size(),
                       (unsigned)(_Match.PositionHash() & 0xFFFFFFFFu),
                       _Session.IsAwaitingResync() ? 1 : 0,
                       (unsigned long long)_RttCount,
                       (unsigned long long)(_RttCount ? _RttSumMs / _RttCount : 0),
                       (unsigned long long)(_RttCount ? _RttMinMs : 0),
                       (unsigned long long)_RttMaxMs);
            }
        }
        ++_Frame;
    }
#endif
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
