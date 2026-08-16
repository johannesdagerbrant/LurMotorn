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
#include <utility>
#include <vector>

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Chess/View/SfxLibrary.h"
#include "Lur/Audio/AudioDevice.h"
#include "Lur/Audio/Mixer.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/App/GameHost.h"   // #43: engine-owned session + persistence choreography
#include "Lur/App/IosApp.h"      // #43 section B: the shared entry point + Metal view + delegate
#include "Lur/App/IosViewHost.h"  // #43 section C: the whole shared #73 heal, renderer included
#include "Lur/App/Platform.h"   // #43 section B: MoltenVK stdio guard + log sink
#include "Lur/App/RenderHandshake.h"  // #43 section C: the platform<->renderer surface/park protocol
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Trace/Trace.h"   // touch->present latency (#192)
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

// The audio device's realtime callback: forward straight to the mixer. Wait-free.
static void MixThunk(void* User, int16_t* Out, uint32_t Frames) {
    static_cast<Lur::Audio::Mixer*>(User)->Render(Out, Frames);
}

// #43 section B: the Metal-backed view (LurMetalView) and the app delegate are the engine's now —
// both games had declared their own, identical apart from the class name.

@interface OnlyChessViewController : UIViewController
@end

@implementation OnlyChessViewController {
    Lur::Render::IRenderer* _Renderer;
    Chess::BoardView _View;
    Lur::Transport::ITransport* _Transport;  // owned by its translation unit
    // #43: the engine owns the session + persistence choreography now. No Session, Store,
    // SyncManager or device id of its own — this main supplies where files live, which radio role
    // and how to log, plus the game's four decisions.
    Lur::App::GameHost _Host;
    Chess::ChessMatchState _Match;           // authoritative game state
    Lur::Audio::Mixer _Mixer;                // wait-free SFX mixer (audio thread reads it)
    Chess::SfxLibrary _Sfx;                  // cooked move sounds, loaded into _Mixer
    Lur::Audio::IAudioDevice* _Audio;        // RemoteIO output stream
    CADisplayLink* _DisplayLink;
    double _PrevFrameTime;  // CACurrentMediaTime() at the last renderFrame (0 = first)
    uint64_t _PendingTouchNs;  // oldest touch sample not yet presented (#192)
    uint64_t _TouchDownNs;     // press time of the gesture in progress (#192)
    // #43 section C: the same platform<->renderer protocol RPS uses, configured Inline — chess renders
    // on the CADisplayLink main thread, so it IS the frame loop and a park needs no ack. Chess has no
    // cross-thread problem to solve here; it adopts the object so the two mains stop expressing the same
    // events in two shapes (chess resized inline, RPS stored a flag), which is what kept the resize and
    // pause/resume handling unabsorbable. The topology is now a value, not a difference in the code.
    Lur::App::RenderHandshake _RH;
    // #73: a DVT launch can initialise the renderer while the app is NOT active — the
    // layer created in that state is never composited (presents "succeed", screen
    // black). Record the state at init; on becoming active, rebuild window+view+layer+
    // renderer (a swapchain recreate is NOT enough — proven by 898999b).
    bool _InitWhileInactive;
    bool _BecameActive;
#if LUR_INTERNAL
    uint64_t _TraceAccumNs;   // #192: latency report — OBSERVATION, so it stays INTERNAL
#endif
// AUTOPLAY IS REMOTE CONTROL, SO IT IS LUR_AGENT, NOT LUR_INTERNAL (issue #195).
// CLAUDE.md draws the line at who is driving: an assistant-driven hook is #if LUR_AGENT and
// is ABSENT from every ordinary build, because *the build a player plays IS Development* —
// which is exactly where LUR_INTERNAL is on. This is armed by a system property and its
// effect is injected input on a device a human is holding; leaving it LUR_INTERNAL shipped a
// hook that could silently take over and play someone's game. That is not hypothetical: the
// same shape bit us on 2026-07-25, when a stale setprop left a LUR_INTERNAL hook armed and it
// fought the player's own gesture.
//
// Armed by a Documents/autoplay marker the dev rig pushes; same-frame instrumentation
// mirrors AndroidMain (issue #57/#58/#69).
#if LUR_AGENT
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
    self.view = [[LurMetalView alloc] initWithFrame:UIScreen.mainScreen.bounds];
}

- (CAMetalLayer*)metalLayer {
    return (CAMetalLayer*)self.view.layer;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // #43 section B: give the engine logger a home before anything can report a problem. Chess had
    // no sink on either phone, so engine-side messages went nowhere on device.
    Lur::App::Platform::InstallLogSink();
    Lur::App::Platform::UnblockStdio();   // before the renderer: a full stdio pipe must never freeze the main thread

    // #43 section C: chess renders on the CADisplayLink main thread. Stated rather than left to the
    // default, because every rule in RenderHandshake keys off it — a wrong value here is a self-deadlock
    // on the #73 heal, not a subtle behaviour change.
    _RH.Configure(Lur::App::ERenderTopology::Inline);
    _RH.Start();

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
    const std::string SaveDirPath = Lur::App::Platform::SaveDir();   // #43: one answer, shared
// RIG-PUSHED FORCED STATE, SO LUR_AGENT (issue #196). This one is DESTRUCTIVE: a marker left
// in a player's container silently deletes their saved games and stats at startup. At
// LUR_INTERNAL it shipped in every ordinary build, because *the build a player plays IS
// Development*. The 2026-07-25 stale-setprop incident was merely annoying; this loses data by
// the same "left behind by accident" mechanism.
#if LUR_AGENT
    // Dev clear-history (rig-pushed Documents/clearsave): wipe opponent records,
    // their meta sidecars, and the cached peer-id BEFORE the store opens — a fresh
    // pairing state for role/matrix tests. The device-id is KEPT (stable identity).
    // One-shot: the marker is consumed. No pm-clear equivalent exists on iOS.
    {
        NSString* const Dir = @(SaveDirPath.c_str());   // the shared save dir, as an NSString
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
    // #43: Init now, Start after the view is attached — the view is handed Store/Sync/DeviceId and
    // must hold them before a peer can go ready, because the ready handler calls straight back into
    // the view's adopt rule.
    {
        Lur::App::GameHost::Config HostCfg;
        HostCfg.SaveDir = SaveDirPath;
        HostCfg.Log = [](const char* M) { os_log(OS_LOG_DEFAULT, "OnlyChess: %{public}s", M); };
        // The BLE transport. Hello exchanges the device GUIDs; colour comes from the two GUIDs,
        // not from the radio role.
        _Transport = Lur::Transport::CreateBleTransport();
        HostCfg.Transport = _Transport;
        _Host.Init(HostCfg);
    }

    Lur::App::GameHost::RecordSync Rec;
    // The view applies the #38 hijack rule and sets identity + loads the record for the adopted
    // peer; the host sends our record only when it adopted. Both the initial link and a reconnect
    // route through this one hook now, so they cannot drift apart.
    Rec.OnPeerAdopted = [View = &_View](const std::string& Peer) { return View->OnPeerLinked(Peer); };
    // Only share OUR game with the peer we are actually playing.
    Rec.IsActiveOpponent = [View = &_View](const std::string& Peer) {
        return View->ActiveOpponentGuid() == Peer;
    };
    Rec.Summarize = [M = &_Match] {
        Lur::App::GameHost::RecordSync::MatchSummary S;
        S.Result     = static_cast<int>(M->LastResult());
        S.WinsLower  = M->Record().WinsLower;
        S.WinsHigher = M->Record().WinsHigher;
        S.Draws      = M->Record().Draws;
        S.Total      = M->Record().TotalMatches();
        return S;
    };
    _Host.EnableRecordSync(_Match, std::move(Rec));

    _Match.SetOnMatchEnd([H = &_Host] { H->OnMatchEnded(); });  // persist + report, one seam
    // Persist the in-progress match when backgrounded, so it survives a close.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(persistState)
                                                 name:UIApplicationDidEnterBackgroundNotification
                                               object:nil];
    // Recreate the swapchain whenever the app (re)activates (issue #73): a DVT
    // kill-existing relaunch can bring the window up against a stale/detached
    // window-server surface, leaving MoltenVK presenting to nothing — the app runs
    // but the screen stays black. Forcing NeedsRecreate on activation rebuilds the
    // swapchain against the *live* surface. Harmless when nothing was wrong.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(recreateSwapchain)
                                                 name:UIApplicationDidBecomeActiveNotification
                                               object:nil];

    // The shared BoardView renders + mutates _Match and ships moves via MoveCodec.
    _View.SetState(&_Match);
    _View.AttachSession(&_Host.Session());
    _View.AttachPersistence(&_Host.Store(), &_Host.Sync(),
                            _Host.DeviceId());  // selector + match switching
    _View.SetLogger([](const char* M) {
        os_log(OS_LOG_DEFAULT, "OnlyChess: View: %{public}s", M);
    });

    // #43: the ~35 lines of session choreography that used to sit here — the hijack-guarded record
    // send, the ready/resync/state-hash/Sync handlers, Session::Start — now live once in
    // Lur::App::GameHost. What is left is the game's four decisions, and they must read IDENTICALLY
    // to the Android main's: that they did not is what this extraction is for.

    // Chess hashes its board so the session can catch a divergence (#72).
    Lur::App::GameHost::Hooks Hooks;
    Hooks.StateHash = [M = &_Match] { return M->PositionHash(); };
    _Host.Start(std::move(Hooks));
#if LUR_AGENT
    _Rng = 0xC0FFEEu ^ static_cast<uint32_t>(_Host.DeviceId().size());  // per-device autoplay seed
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

    const int DrawW = static_cast<int>(Layer.drawableSize.width);
    const int DrawH = static_cast<int>(Layer.drawableSize.height);

    if (!_RH.IsReady()) {
        _RH.PublishSurface((__bridge void*)Layer, DrawW, DrawH);
        _Renderer = Lur::Render::VulkanRenderer::Create("OnlyChess");
        const bool Ok = _Renderer && _Renderer->Init(_RH.Surface());
        _RH.SetReady(Ok);
        // #73 precondition check: a renderer initialised while the app is NOT active
        // ends up presenting into a layer the window server never composites.
        _InitWhileInactive =
            UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
        os_log(OS_LOG_DEFAULT, "OnlyChess: Renderer init: %{public}s (drawable %dx%d, appActive=%d)",
               Ok ? "ok" : "failed", DrawW, DrawH, _InitWhileInactive ? 0 : 1);
        if (Ok) {
            _View.CreateResources(_Renderer);
            // #188: make the frame's idle wait feed the radio — see the Android note. The
            // callback runs on this same thread, inside the wait, so nothing is shared.
            _Renderer->SetIdleWaitCallback(
                [](void* U) { static_cast<Lur::App::GameHost*>(U)->PumpInbox(); }, &_Host);
            _DisplayLink = [CADisplayLink displayLinkWithTarget:self
                                                       selector:@selector(renderFrame)];
            [_DisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode];

            // Bring up audio once the view is live: load the cooked SFX into the mixer, wire
            // each move to the sound for its KIND (move / capture / check / checkmate — the
            // view classifies, SfxLibrary picks the clip and its variation), then open the
            // RemoteIO stream. Sfx.Load must finish before the audio thread starts pulling
            // (Mixer::Add is not thread-safe).
            if (_Audio == nullptr) {
                _Mixer.Init(Lur::Audio::Mixer::DefaultRate);
                _Sfx.Load(_Mixer);
                Chess::SfxLibrary* Sfx = &_Sfx;
                Lur::Audio::Mixer* Mixer = &_Mixer;
                _View.SetMovePlayed([Sfx, Mixer](Chess::EMoveSound S) { Sfx->Play(*Mixer, S); });
                _Audio = Lur::Audio::CreateAudioDevice();
                const bool AudioOk = _Audio && _Audio->Start(MixThunk, &_Mixer);
                os_log(OS_LOG_DEFAULT, "OnlyChess: Audio init: %{public}s", AudioOk ? "ok" : "failed");
            }
        }
    } else {
        // Was an inline _Renderer->Resize here. Deferring it to the top of the next frame is the same
        // safe point RPS uses, and it is what lets one call site serve both topologies.
        _RH.RequestResize(DrawW, DrawH);
    }
}

- (void)renderFrame {
    // #73 (round 5, ported from RPS): the render loop can run with the view hosted in
    // no window / no scene (a DVT-relaunch state; never true in health). Precise
    // condition, retried every ~2 s until a scene exists to attach to.
    static uint32_t FramesSinceAttach = 0;
    if (_RH.IsReady() && (self.view.window == nil || self.view.window.windowScene == nil)) {
        if (++FramesSinceAttach >= 120) {
            FramesSinceAttach = 0;
            [self reattachForActivation];
        }
    } else {
        FramesSinceAttach = 0;
    }
    if (_BecameActive) {
        _BecameActive = false;
        if (_RH.IsReady() && _InitWhileInactive) [self reattachForActivation];
    }
    if (!_RH.IsReady()) return;
    // The safe point: same place in the frame as RPS's render thread takes it. One TakeWork per frame —
    // it consumes, so a second call would silently discard whatever the first left behind.
    const Lur::App::RenderWork Work = _RH.TakeWork();
    if (Work.Resize) _Renderer->Resize(Work.W, Work.H);
    const double Now = CACurrentMediaTime();  // monotonic seconds
    const uint64_t ElapsedNs =
        _PrevFrameTime > 0.0 ? static_cast<uint64_t>((Now - _PrevFrameTime) * 1e9) : 0;
    _PrevFrameTime = Now;
#if LUR_INTERNAL
    // Always-on render-health heartbeat (#73) — deliberately NOT gated on autoplay or
    // the link: diagnosis was blinded whenever every periodic line needed a live match.
    static uint64_t BeatAccumNs = 0;
    BeatAccumNs += ElapsedNs;
    if (BeatAccumNs > 2'000'000'000ull) {
        BeatAccumNs = 0;
        UIWindow* Win = self.view.window;
        os_log(OS_LOG_DEFAULT,
               "OnlyChess: HEARTBEAT presented=%u appActive=%d win=%d key=%d scene=%ld "
               "host=%d scenes=%lu",
               _Renderer != nullptr ? _Renderer->PresentedFrames() : 0u,
               UIApplication.sharedApplication.applicationState == UIApplicationStateActive ? 1 : 0,
               Win != nil ? 1 : 0, Win.isKeyWindow ? 1 : 0,
               (long)(Win.windowScene != nil ? Win.windowScene.activationState : -99),
               self.view.layer.superlayer != nil ? 1 : 0,
               (unsigned long)UIApplication.sharedApplication.connectedScenes.count);
    }
#endif
#if LUR_AGENT
    const bool WasMyTurn = _Match.IsMyTurn();
    const uint32_t MatchesBefore = _Match.Record().TotalMatches();
#endif
    _Host.Tick(ElapsedNs);  // real-time-denominated: drives handshake + liveness (applies peer move)
#if LUR_AGENT
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
            const bool Played = (_Host.Session().IsReady() && NowMyTurn) ? _View.AutoPlayRandomLegalMove(_Rng) : false;
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
                       "myTurn=%d ply=%zu hash=%08x gate=%d rtt(n=%llu avg=%llums min=%llums max=%llums) "
                       "presented=%u",  // stuck at 0 = dead swapchain (#73)
                       MatchesAfter, (unsigned long long)_SameFrame, (unsigned long long)_PeerReplies,
                       (unsigned long long)_NewGameOpens, (unsigned long long)_DelayedReplies,
                       _Match.IsMyTurn() ? 1 : 0, _Match.Record().Moves.size(),
                       (unsigned)(_Match.PositionHash() & 0xFFFFFFFFu),
                       _Host.Session().IsAwaitingResync() ? 1 : 0,
                       (unsigned long long)_RttCount,
                       (unsigned long long)(_RttCount ? _RttSumMs / _RttCount : 0),
                       (unsigned long long)(_RttCount ? _RttMinMs : 0),
                       (unsigned long long)_RttMaxMs,
                       _Renderer != nullptr ? _Renderer->PresentedFrames() : 0u);
            }
        }
        ++_Frame;
    }
#endif
    // #189: one more inbox drain, immediately before we draw. A peer move that landed
    // after the Tick above would otherwise wait for the next CADisplayLink callback —
    // and on this device that beat is 40 Hz, so it is ~25 ms of sitting still.
    _Host.PumpInbox();
    CAMetalLayer* Layer = [self metalLayer];
    _View.Render(_Renderer, static_cast<float>(Layer.drawableSize.width),
                 static_cast<float>(Layer.drawableSize.height));
    // #192: touch sample -> frame handed to the presentation engine. See the Android note
    // for what this does and does not include.
    if (_PendingTouchNs != 0) {
        LUR_TRACE_LATENCY("input.toPresent", _PendingTouchNs);
        _PendingTouchNs = 0;
    }
#if LUR_INTERNAL
    // #192: the latency report, NOT gated on autoplay or the link — it exists to be read
    // while a HUMAN taps, which is exactly when the autoplay diag above is silent.
    _TraceAccumNs += ElapsedNs;
    if (_TraceAccumNs > 2000000000ull) {
        _TraceAccumNs = 0;
        char TraceLine[512];
        if (Lur::Trace::FormatLineAndReset(TraceLine, sizeof(TraceLine)) > 0)
            os_log(OS_LOG_DEFAULT, "OnlyChess: TRACE %{public}s", TraceLine);
    }
#endif
}

// Backgrounded: persist the in-progress match so it survives a close.
- (void)persistState {
    _Host.OnBackground();   // #43: persist the in-progress record
    if (_Audio != nullptr) _Audio->Stop();   // release the audio stream while backgrounded
}

// App became active: force a swapchain recreate against the now-live surface (#73's
// benign half — harmless when nothing was wrong), and flag the render loop, which
// performs the FULL reattach when the renderer was born inactive (the real heal —
// a swapchain recreate alone was proven insufficient by 898999b).
- (void)recreateSwapchain {
    _BecameActive = true;  // renderFrame decides whether the full #73 reattach is due
    if (!_RH.IsReady() || _Renderer == nullptr) return;
    CAMetalLayer* Layer = [self metalLayer];
    os_log(OS_LOG_DEFAULT, "OnlyChess: active -> swapchain recreate (drawable %dx%d)",
           (int)Layer.drawableSize.width, (int)Layer.drawableSize.height);
    _RH.RequestResize(static_cast<int>(Layer.drawableSize.width),
                      static_cast<int>(Layer.drawableSize.height));
    if (_Audio != nullptr) _Audio->Start(MixThunk, &_Mixer);  // resume audio on foreground
}

// #73 heal: the renderer was initialised while the app wasn't active, so its
// CAMetalLayer is bound to a window-server surface that is never composited. Rebuild
// the whole chain against the now-live window server: fresh UIWindow + fresh
// view/CAMetalLayer + full renderer Shutdown/Init (+ re-created view resources).
//
// #43 section C: the scene pick, the park, the window/view/layer rebuild and the two
// ordering fixes that took rounds to find are all LurReattachRenderHost now, shared with
// RPS. What stays here is the one thing the games still disagree on: chess owns its
// renderer on this thread, so it applies the re-init inline.
- (void)reattachForActivation {
    // #43 section C: the scene check, the park, the outgoing-view grab, the rebuild and the arm/release
    // are the engine's now — the sequence was identical in both games once the topology stopped being
    // implicit. Chess ignores the returned retiring view: it applies the rebuild before returning, so
    // there is no window in which the old layer must be kept alive.
    if (LurReattachRenderHost(self, LurMetalView.class, _RH) == nil) return;  // too early; the loop retries

    // What stays here is the part that genuinely differs: WHO applies the reinit. Chess owns the renderer
    // on this thread, so it is this thread.
    const Lur::App::RenderWork Work = _RH.TakeWork();
    if (!Work.Reinit) return;   // cannot happen today; not worth a silent Shutdown if it ever does
    _RH.SetReady(false);
    _Renderer->Shutdown();      // full teardown (device, surface, everything)
    const bool Ok = _Renderer->Init(Work.Surface);
    _RH.SetReady(Ok);
    _InitWhileInactive =
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
    os_log(OS_LOG_DEFAULT, "OnlyChess: #73 reattach: re-init %{public}s (drawable %dx%d, appActive=%d)",
           Ok ? "ok" : "FAILED", Work.W, Work.H, _InitWhileInactive ? 0 : 1);
    if (Ok) _View.CreateResources(_Renderer);
    // No SignalReinitDone here: that ack exists to tell another thread's owner it may release the
    // retiring view, and chess has no retiring view to hold — the rebuild finished before this method
    // returned. Setting a flag nothing consumes would be dead state dressed as symmetry.
}

// #192: stamp a touch sample for the latency traces. UITouch.timestamp is seconds on the
// same monotonic base as CACurrentMediaTime, which on Apple platforms is also what
// steady_clock (and so Lur::Trace::NowNs) reads — so the two are directly comparable.
- (uint64_t)stampTouch:(UITouch*)touch {
    const uint64_t EventNs = static_cast<uint64_t>(touch.timestamp * 1.0e9);
    const uint64_t Now = Lur::Trace::NowNs();
    // Guard the subtraction rather than trusting the two clocks to agree: if a future OS
    // changes UITouch's base, a bogus huge sample would poison the aggregate silently.
    if (EventNs != 0 && EventNs <= Now && Now - EventNs < 1'000'000'000ull) {
        LUR_TRACE_LATENCY("input.dispatch", EventNs);
        // Oldest unpresented sample wins — see the Android note; overwriting would measure
        // the last sample to squeak in before the present rather than what the finger saw.
        if (_PendingTouchNs == 0) _PendingTouchNs = EventNs;
        return EventNs;
    }
    return 0;
}

// #187: the move is committed HERE, on the press. It used to wait for touchesEnded, which
// spent the whole input.downToUp span — 140 ms measured on the Galaxy, the largest single
// cost in the chain by an order of magnitude — doing nothing at all.
//
// It also removes the ability to change your mind mid-press, and that is deliberate:
// chess's own TOUCH-MOVE RULE says that once you touch a piece you must move it. This makes
// the app MORE faithful to over-the-board play, not less. (There is no drag gesture here
// either, so in practice nothing is lost.)
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_RH.IsReady()) return;
    UITouch* Touch = touches.anyObject;
    _TouchDownNs = [self stampTouch:Touch];
    [self dispatchTap:Touch];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_RH.IsReady()) return;
    UITouch* Touch = touches.anyObject;
    (void)[self stampTouch:Touch];
    // The move already went out on the press (#187). All that is left here is closing the
    // measurement of the dead time we no longer spend.
    if (_TouchDownNs != 0) {
        LUR_TRACE_LATENCY("input.downToUp", _TouchDownNs);
        _TouchDownNs = 0;
    }
}

// Map a UITouch into drawable pixels and hand it to the board. Points -> pixels via the
// layer's contentsScale, which is the space BoardView lays out in.
- (void)dispatchTap:(UITouch*)touch {
    const CGPoint P = [touch locationInView:self.view];
    CAMetalLayer* Layer = [self metalLayer];
    const CGFloat Scale = Layer.contentsScale;
    _View.OnTap(static_cast<float>(P.x * Scale), static_cast<float>(P.y * Scale),
                static_cast<float>(Layer.drawableSize.width),
                static_cast<float>(Layer.drawableSize.height));
}

@end

// #43 section B: the delegate, the autorelease pool, UIApplicationMain and the MoltenVK
// pre-instance configuration all live in LurIosMain. Chess never set
// MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0 (RPS learned it in #103); adopting the shared entry point
// is what gives it that, and it is the one behaviour change here — see IosApp.mm.
int main(int argc, char* argv[]) {
    return LurIosMain(argc, argv, [OnlyChessViewController class]);
}
