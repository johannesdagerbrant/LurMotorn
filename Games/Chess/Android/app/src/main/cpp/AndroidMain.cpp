// Chess's Android main. What is left of it after #43: the FRAME LOOP — whose cadence is a latency
// decision this game owns (#188/#189) — plus the four points where chess has an opinion about the
// platform, the BLE transport, and the glue to the shared Chess::BoardView (which owns all render +
// touch logic; the iOS app drives the same BoardView from its UIKit shim, one source of truth).
//
// The NativeActivity ceremony around that loop is Lur::App::AndroidApp, and the session +
// persistence choreography is Lur::App::GameHost. Neither is chess's business, and both were being
// re-derived in the RPS main alongside this one.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#if LUR_AGENT
#include <sys/system_properties.h>  // read debug.lur.autoplay (agent-driven soak, #195)
#include "Lur/DevGui/Console.h"     // the console routes its own pointer events (#201)
#endif

#include "Chess/Board.h"
#include "Chess/App/ChessGame.h"
#include "Chess/View/BoardView.h"
#include "Chess/View/SfxLibrary.h"
#include "Lur/Core/CVar.h"
#include "Lur/Audio/AudioDevice.h"
#include "Lur/Audio/Mixer.h"
#include "Lur/App/AndroidApp.h"  // #43 section B: the NativeActivity ceremony, engine-owned
#include "Lur/App/GameHost.h"   // #43: engine-owned session + persistence choreography
#include "Lur/Net/Session.h"
#include "Lur/Trace/Trace.h"   // touch->present latency (#192)
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

struct AppState {
    // #43 section B: the window/renderer/looper ceremony is the engine's now. This main no longer
    // owns a renderer pointer, a Ready flag, an ALooper drain or an internalDataPath lookup — all
    // of which it had been re-deriving alongside RPS, comment for comment.
    Lur::App::AndroidApp Plat;
    // #45: the game's state AND its wiring live in Chess::ChessGame now. This main used to declare
    // View + Match here and then connect them to the host by hand — the same connecting the iOS and
    // desktop mains each wrote out separately.
    Chess::ChessGame Game;
    // #43: the session + persistence choreography lives in the engine now. This main holds no
    // Session, Store or SyncManager of its own — the host owns them, and what used to be ~50 lines
    // of wiring duplicated with the iOS main is now a Config plus four game hooks.
    Lur::App::GameHost Host;
    uint64_t PendingTouchNs = 0;             // oldest touch sample not yet presented (#192)
    uint64_t TouchDownNs = 0;                // press time of the gesture in progress (#192)
    Lur::Audio::Mixer Mixer;                 // wait-free SFX mixer (audio thread reads it)
    Chess::SfxLibrary Sfx;                   // cooked move sounds, loaded into Mixer
    Lur::Audio::IAudioDevice* Audio = nullptr;  // AAudio output stream
};

// The audio device's realtime callback: forward straight to the mixer. Wait-free.
void MixThunk(void* User, int16_t* Out, uint32_t Frames) {
    static_cast<Lur::Audio::Mixer*>(User)->Render(Out, Frames);
}

// The window and the Vulkan renderer are up. What is left here is genuinely chess's: GPU
// resources, the radio's piggy-back on the idle wait, audio, and the core smoke test.
//
// ONE DELIBERATE BEHAVIOUR CHANGE, in a failure path: audio startup and the smoke test used to sit
// inside INIT_WINDOW but OUTSIDE its `if (Ready)` guard, so a renderer that failed to initialise
// still opened an AAudio stream and still logged a movegen count. Nothing could be drawn in that
// state — the loop parks on a -1 poll timeout forever — so those two were serving an app that was
// already dead. "Surface ready" now means ready.
void OnSurfaceReady(AppState& State) {
    State.Game.View().CreateResources(State.Plat.Renderer());
    // #188: make the frame's idle wait feed the radio. At one frame in flight the CPU
    // parks ~15 ms per frame inside vkWaitForFences, and that park sits between the
    // loop's two inbox drains (#189) and the next iteration — which is exactly where a
    // peer's move was waiting.
    State.Plat.Renderer()->SetIdleWaitCallback(
        [](void* U) { static_cast<AppState*>(U)->Host.PumpInbox(); }, &State);

    // Bring up audio: load the cooked SFX into the mixer, wire each move to the
    // sound for its KIND (move / capture / check / checkmate — the view
    // classifies, SfxLibrary picks the clip and its variation), then open the
    // low-latency stream. Order matters — Sfx.Load (which calls Mixer::Add) must
    // finish before the device thread starts pulling.
    if (State.Audio == nullptr) {
        State.Mixer.Init(Lur::Audio::Mixer::DefaultRate);
        State.Sfx.Load(State.Mixer);
        AppState* St = &State;
        State.Game.View().SetMovePlayed([St](Chess::EMoveSound S) { St->Sfx.Play(St->Mixer, S); });
        State.Audio = Lur::Audio::CreateAudioDevice();
        const bool AudioOk = State.Audio && State.Audio->Start(MixThunk, &State.Mixer);
        LOGI("Audio init: %s", AudioOk ? "ok" : "failed");
    }

    // Smoke test: the shared, perft-verified C++ core runs on-device.
    Chess::Board Board = Chess::Board::StartPosition();
    Chess::MoveList Moves;
    Chess::GenerateLegalMoves(Board, Moves);
    LOGI("Chess core alive: %d legal moves from the start position", Moves.Count);
}

// The window is going away. The audio device must stop BEFORE the renderer teardown that
// follows this callback — the engine runs them in that order for exactly this reason.
void OnSurfaceLost(AppState& State) {
    if (State.Audio != nullptr) {
        State.Audio->Stop();
        delete State.Audio;
        State.Audio = nullptr;
    }
}

bool HandleInput(AppState& State, AInputEvent* Event) {
    // Readiness and the window are the engine's precondition now — it drops events before
    // they reach here, so what remains is chess's own filter.
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return false;

    // #192 (ported from RPS #185): how long the OS took to hand us this sample. Everything
    // before this point is digitiser + input dispatch — a floor we do not control — so
    // measuring it separately is what says how much of the move latency is ours to fix.
    // AMotionEvent_getEventTime and Lur::Trace::NowNs are both CLOCK_MONOTONIC ns.
    const uint64_t EventNs = static_cast<uint64_t>(AMotionEvent_getEventTime(Event));
    LUR_TRACE_LATENCY("input.dispatch", EventNs);
    // Keep the OLDEST unpresented sample: several samples can arrive between two frames,
    // and overwriting would measure the last one to squeak in before the present —
    // flattering, and not what the finger saw.
    if (State.PendingTouchNs == 0) State.PendingTouchNs = EventNs;

    // The dead time this epic exists to reclaim (#187): how long the finger sits on the
    // glass before we do anything with it. Committing on UP spends all of it. A synthetic
    // `adb shell input tap` sends DOWN and UP together and reads ~0 here, so this number is
    // only meaningful under a REAL finger — which is the point: it is the human's gesture,
    // not the machine's, that we are paying for.
    const int32_t Action = AMotionEvent_getAction(Event) & AMOTION_EVENT_ACTION_MASK;
    if (Action == AMOTION_EVENT_ACTION_DOWN) State.TouchDownNs = EventNs;
    if (Action == AMOTION_EVENT_ACTION_UP && State.TouchDownNs != 0) {
        LUR_TRACE_LATENCY("input.downToUp", State.TouchDownNs);
        State.TouchDownNs = 0;
    }

    const float X = AMotionEvent_getX(Event, 0), Y = AMotionEvent_getY(Event, 0);

#if !LUR_SHIPPING
    // The dev console gets first refusal on every pointer event, and answers for itself: it owns the
    // two-finger triple-tap that opens it and, once open, the whole pointer. This shim contributes
    // plumbing only — pointer count, position, timestamp — because the decisions were per-platform
    // copies once (#151: Android could open the console, iOS could not open it AT ALL) and are not
    // going to be again.
    const int Pointers = static_cast<int>(AMotionEvent_getPointerCount(Event));
    Lur::DevGui::Console& Con = State.Game.View().DevConsole();
    switch (Action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (Con.PointerDown(Pointers, X, Y, EventNs)) return true;
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            if (Con.PointerMove(X, Y, EventNs)) return true;
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (Con.PointerUp(X, Y, EventNs)) return true;
            break;
        case AMOTION_EVENT_ACTION_CANCEL: Con.CancelGesture(); return true;
        default: break;
    }
#endif

    // #187: commit on DOWN, not UP. This used to wait for ACTION_UP, which spent the whole
    // input.downToUp span — 140 ms measured, and the largest single cost in the chain by an
    // order of magnitude — doing nothing at all.
    //
    // It also removes the ability to change your mind mid-press, and that is deliberate:
    // chess's own TOUCH-MOVE RULE says that once you touch a piece you must move it. This
    // makes the app MORE faithful to over-the-board play, not less. (There is no drag
    // gesture here either, so in practice nothing is lost.)
    //
    // NOTE on the console gesture: commit-on-DOWN means the FIRST finger of a two-finger chain
    // still lands on the board if that is where you put it, so perform the gesture in the margin
    // above or below the board — an off-board tap hits no square (SquareAt returns NoSquare) and
    // does nothing. The console consumes every SUBSEQUENT finger of the chain, so only that first
    // touch can reach the board at all.
    if (Action != AMOTION_EVENT_ACTION_DOWN) return false;
    State.Game.View().OnTap(X, Y, State.Plat.Width(), State.Plat.Height());
    return true;
}

} // namespace

void android_main(android_app* App) {
    AppState State;

    // #43 section B: hand the NativeActivity ritual to the engine — glue callbacks, the engine log
    // sink, renderer create/init/teardown with the window, the looper drain, internalDataPath. What
    // stays here are the four points where chess actually has an opinion. Start() only WIRES; the
    // callbacks below cannot fire until the first PumpEvents inside the loop, which is why they may
    // safely reference a Host that is configured further down.
    Lur::App::AndroidApp::Callbacks Cb;
    Cb.OnSurfaceReady = [&State] { OnSurfaceReady(State); };
    Cb.OnSurfaceLost  = [&State] { OnSurfaceLost(State); };
    // Backgrounded: persist the in-progress match so it survives a close.
    Cb.OnPause        = [&State] { State.Host.OnBackground(); };
    Cb.OnInput        = [&State](AInputEvent* E) { return HandleInput(State, E); };
    State.Plat.Start(App, std::move(Cb));

    // Persistent device identity (issue #17/#18): the same GUID the BLE role uses,
    // read from the app's internal data dir (== Context.filesDir, where the Kotlin
    // radio reads it too, so both agree). Drives colour + the per-opponent stats key.
    //
    // #43: everything below used to be ~50 lines duplicated comment-for-comment with the iOS main
    // — Store, device id, SyncManager, the match-end persist+log, the hijack-guarded record send,
    // the ready/resync/hash/Sync handlers, Session::Start. All of it is engine choreography, none
    // of it is chess, and the two copies had drifted (ownership, capture style, wiring order, and
    // a MATCH END line whose format differed per phone). What remains here is what is genuinely
    // this app's: where files live, which radio role, how to log — and the game's four decisions.
    Lur::App::GameHost::Config HostCfg;
    HostCfg.SaveDir = State.Plat.SaveDir();
    // The BLE transport. Hello exchanges the device GUIDs; colour comes from the two GUIDs, not
    // from the radio role.
    HostCfg.Transport = Lur::Transport::CreateBleTransport();
    HostCfg.Log = [](const char* M) { LOGI("%s", M); };
    // Init before the view is attached: the view is handed Store/Sync/DeviceId, and it must hold
    // them before a peer can go ready (the ready handler calls back into the view's adopt rule).
    State.Host.Init(HostCfg);

    // #45: chess states its own answers ONCE, in Chess::ChessGame — the record-sync trio, the view
    // attachment, the match-end hook and the state hash. This main used to carry all of it, as did
    // the iOS and desktop mains, with bodies that differed only in capture style. Configure runs in
    // the window GameHost's phase comment describes: after Init (Store + device id exist, so the
    // view can be handed them) and before Start (nothing can call back into an unattached view).
    Lur::App::GameHost::Hooks Hooks;
    State.Game.Configure(State.Host, Hooks);
    State.Host.Start(std::move(Hooks));

#if LUR_INTERNAL
    uint64_t TraceAccumNs = 0;   // #192: latency report — OBSERVATION, so it stays INTERNAL
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
// Plays a random legal move the SAME frame it becomes our turn, so a reply ships the frame
// the opponent's move was received (issue #57/#58).
#if LUR_AGENT
    bool AutoEnabled = false;
    uint32_t Rng = 0xC0FFEEu ^ static_cast<uint32_t>(State.Host.DeviceId().size());
    uint64_t Frame = 0, PeerReplies = 0, SameFrame = 0, NewGameOpens = 0, DelayedReplies = 0;
    uint64_t ReportAccumNs = 0;
    // Net-ms RTT: our move leaves -> the peer's same-frame reply arrives. Measured on
    // this device's clock alone (no cross-device sync): stamp when we send, close when
    // the reply lands. Includes 2x transit + <=1 peer frame + <=1 our frame.
    uint64_t ClockNs = 0, MoveSentNs = 0;
    uint64_t RttCount = 0, RttSumMs = 0, RttMinMs = ~0ull, RttMaxMs = 0;
#endif
    auto PrevTime = std::chrono::steady_clock::now();
    while (State.Plat.IsRunning()) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

#if LUR_AGENT
        const bool     WasMyTurn = State.Game.Match().IsMyTurn();
        const uint32_t MatchesBefore = State.Game.Match().Record().TotalMatches();
#endif
        State.Host.Tick(ElapsedNs);  // real-time-denominated: drives handshake + liveness
#if LUR_AGENT
        {
            const bool     NowMyTurn = State.Game.Match().IsMyTurn();
            const uint32_t MatchesAfter = State.Game.Match().Record().TotalMatches();
            const bool     GotPeerMove = !WasMyTurn && NowMyTurn;       // peer moved -> our turn, this frame
            const bool     PeerEndedGame = MatchesAfter != MatchesBefore;
            if (!AutoEnabled) {
                char V[8] = {0};
                if (__system_property_get("debug.lur.autoplay", V) > 0 && V[0] == '1') {
                    AutoEnabled = true;
                    LOGI("autoplay ENABLED (debug.lur.autoplay=1): auto-drive our turn");
                }
            }
            if (AutoEnabled) {
                ClockNs += ElapsedNs;
                if (GotPeerMove && MoveSentNs != 0) {   // reply to our outstanding move
                    const uint64_t Ms = (ClockNs - MoveSentNs) / 1'000'000ull;
                    ++RttCount; RttSumMs += Ms;
                    if (Ms < RttMinMs) RttMinMs = Ms;
                    if (Ms > RttMaxMs) RttMaxMs = Ms;
                    MoveSentNs = 0;
                }
                const bool Played = (State.Host.Session().IsReady() && NowMyTurn)
                                        ? State.Game.View().AutoPlayRandomLegalMove(Rng) : false;
                if (Played) MoveSentNs = ClockNs;       // our move is on the wire; await reply
                if (GotPeerMove) {
                    ++PeerReplies;
                    if (PeerEndedGame)   ++NewGameOpens;
                    else if (Played)     ++SameFrame;
                    else               { ++DelayedReplies; LOGI("WARN: our turn, no same-frame reply @frame %llu",
                                                                (unsigned long long)Frame); }
                }
            }
            ReportAccumNs += ElapsedNs;
            if (AutoEnabled && ReportAccumNs > 2'000'000'000ull) {
                ReportAccumNs = 0;
                // turn/ply/hash/gate: enough context to diagnose a stall from the log
                // alone — whose move it is, how deep the game is, whether the boards
                // agree (hash), and whether the resync gate is holding moves (#72).
                LOGI("AUTOPLAY game=%u sameFrame=%llu/%llu opens=%llu delayed=%llu "
                     "myTurn=%d ply=%zu hash=%08x gate=%d rtt(n=%llu avg=%llums min=%llums max=%llums) "
                     "presented=%u",  // stuck at 0 = dead swapchain (#73)
                     MatchesAfter, (unsigned long long)SameFrame, (unsigned long long)PeerReplies,
                     (unsigned long long)NewGameOpens, (unsigned long long)DelayedReplies,
                     State.Game.Match().IsMyTurn() ? 1 : 0, State.Game.Match().Record().Moves.size(),
                     (unsigned)(State.Game.Match().PositionHash() & 0xFFFFFFFFu),
                     State.Host.Session().IsAwaitingResync() ? 1 : 0,
                     (unsigned long long)RttCount,
                     (unsigned long long)(RttCount ? RttSumMs / RttCount : 0),
                     (unsigned long long)(RttCount ? RttMinMs : 0),
                     (unsigned long long)RttMaxMs,
                     State.Plat.Renderer() != nullptr ? State.Plat.Renderer()->PresentedFrames() : 0u);
            }
            ++Frame;
        }
#endif
#if LUR_INTERNAL
        // #192: the latency report. Deliberately OUTSIDE the agent block above — the whole
        // point is to read it while a HUMAN taps, which is exactly when no agent is driving.
        // (Same lesson as the #73 heartbeat: a periodic line that needs a live match is blind
        // precisely when you need it.)
        TraceAccumNs += ElapsedNs;
        if (TraceAccumNs > 2'000'000'000ull) {
            TraceAccumNs = 0;
            char TraceLine[512];
            if (Lur::Trace::FormatLineAndReset(TraceLine, sizeof(TraceLine)) > 0)
                LOGI("TRACE %s", TraceLine);
        }
#endif
        // Window/lifecycle commands + input. The callbacks wired at the top fire from in here.
        State.Plat.PumpEvents();

        if (State.Plat.IsReady()) {
            // #189: one more inbox drain, immediately before we draw. A peer move that
            // landed after the top-of-loop Tick would otherwise wait for the NEXT
            // iteration — and this loop is vsync-bound, so that is a whole refresh of
            // sitting still. Half a frame off every inbound move, on average.
            State.Host.PumpInbox();
            State.Game.View().Render(State.Plat.Renderer(), State.Plat.Width(), State.Plat.Height());
            // #192: touch sample -> this frame handed to the presentation engine. Bounds OUR
            // share of move latency: OS dispatch + the wait for this loop iteration + tap
            // handling + record + submit, stopping at vkQueuePresentKHR returning. It does
            // NOT include the compositor holding the image until scan-out, so real photons
            // are up to one more refresh later — a constant, so before/after stays honest.
            if (State.PendingTouchNs != 0) {
                LUR_TRACE_LATENCY("input.toPresent", State.PendingTouchNs);
                State.PendingTouchNs = 0;
            }
        }
    }

    State.Plat.Shutdown();
}
