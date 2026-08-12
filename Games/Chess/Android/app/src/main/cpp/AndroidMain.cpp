// Android entry point. A thin platform shim: it owns the NativeActivity loop,
// creates the Vulkan renderer + BLE transport, and drives the shared
// Chess::BoardView (which owns all render + touch logic). The iOS app drives the
// same BoardView from its UIKit shim — one source of truth for the game view.
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
#endif

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Chess/View/SfxLibrary.h"
#include "Lur/Audio/AudioDevice.h"
#include "Lur/Audio/Mixer.h"
#include "Lur/App/GameHost.h"   // #43: engine-owned session + persistence choreography
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Trace/Trace.h"   // touch->present latency (#192)
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;
    Chess::BoardView View;
    // #43: the session + persistence choreography lives in the engine now. This main holds no
    // Session, Store or SyncManager of its own — the host owns them, and what used to be ~50 lines
    // of wiring duplicated with the iOS main is now a Config plus four game hooks.
    Lur::App::GameHost Host;
    Chess::ChessMatchState Match;   // authoritative game state (record + board + colour)
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

void HandleCmd(android_app* App, int32_t Cmd) {
    auto* State = static_cast<AppState*>(App->userData);
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (App->window != nullptr) {
                State->Renderer = Lur::Render::VulkanRenderer::Create("OnlyChess");
                State->Ready = State->Renderer && State->Renderer->Init(App->window);
                LOGI("Renderer init: %s", State->Ready ? "ok" : "failed");
                if (State->Ready) {
                    State->View.CreateResources(State->Renderer);
                    // #188: make the frame's idle wait feed the radio. At one frame in
                    // flight the CPU parks ~15 ms per frame inside vkWaitForFences, and
                    // that park sits between the loop's two inbox drains (#189) and the
                    // next iteration — which is exactly where a peer's move was waiting.
                    State->Renderer->SetIdleWaitCallback(
                        [](void* U) { static_cast<AppState*>(U)->Host.PumpInbox(); }, State);
                }

                // Bring up audio: load the cooked SFX into the mixer, wire each move to the
                // sound for its KIND (move / capture / check / checkmate — the view
                // classifies, SfxLibrary picks the clip and its variation), then open the
                // low-latency stream. Order matters — Sfx.Load (which calls Mixer::Add) must
                // finish before the device thread starts pulling.
                if (State->Audio == nullptr) {
                    State->Mixer.Init(Lur::Audio::Mixer::DefaultRate);
                    State->Sfx.Load(State->Mixer);
                    AppState* St = State;
                    State->View.SetMovePlayed(
                        [St](Chess::EMoveSound S) { St->Sfx.Play(St->Mixer, S); });
                    State->Audio = Lur::Audio::CreateAudioDevice();
                    const bool AudioOk = State->Audio && State->Audio->Start(MixThunk, &State->Mixer);
                    LOGI("Audio init: %s", AudioOk ? "ok" : "failed");
                }

                // Smoke test: the shared, perft-verified C++ core runs on-device.
                Chess::Board Board = Chess::Board::StartPosition();
                Chess::MoveList Moves;
                Chess::GenerateLegalMoves(Board, Moves);
                LOGI("Chess core alive: %d legal moves from the start position", Moves.Count);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (State->Audio != nullptr) {
                State->Audio->Stop();
                delete State->Audio;
                State->Audio = nullptr;
            }
            if (State->Renderer != nullptr) State->Renderer->Shutdown();
            State->Ready = false;
            break;
        case APP_CMD_PAUSE:
            // Backgrounded: persist the in-progress match so it survives a close.
            State->Host.OnBackground();   // #43: persist the in-progress record
            break;
        default:
            break;
    }
}

int32_t HandleInput(android_app* App, AInputEvent* Event) {
    auto* State = static_cast<AppState*>(App->userData);
    if (State == nullptr || !State->Ready || App->window == nullptr) return 0;
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    // #192 (ported from RPS #185): how long the OS took to hand us this sample. Everything
    // before this point is digitiser + input dispatch — a floor we do not control — so
    // measuring it separately is what says how much of the move latency is ours to fix.
    // AMotionEvent_getEventTime and Lur::Trace::NowNs are both CLOCK_MONOTONIC ns.
    const uint64_t EventNs = static_cast<uint64_t>(AMotionEvent_getEventTime(Event));
    LUR_TRACE_LATENCY("input.dispatch", EventNs);
    // Keep the OLDEST unpresented sample: several samples can arrive between two frames,
    // and overwriting would measure the last one to squeak in before the present —
    // flattering, and not what the finger saw.
    if (State->PendingTouchNs == 0) State->PendingTouchNs = EventNs;

    // The dead time this epic exists to reclaim (#187): how long the finger sits on the
    // glass before we do anything with it. Committing on UP spends all of it. A synthetic
    // `adb shell input tap` sends DOWN and UP together and reads ~0 here, so this number is
    // only meaningful under a REAL finger — which is the point: it is the human's gesture,
    // not the machine's, that we are paying for.
    const int32_t Action = AMotionEvent_getAction(Event) & AMOTION_EVENT_ACTION_MASK;
    if (Action == AMOTION_EVENT_ACTION_DOWN) State->TouchDownNs = EventNs;
    if (Action == AMOTION_EVENT_ACTION_UP && State->TouchDownNs != 0) {
        LUR_TRACE_LATENCY("input.downToUp", State->TouchDownNs);
        State->TouchDownNs = 0;
    }

    // #187: commit on DOWN, not UP. This used to wait for ACTION_UP, which spent the whole
    // input.downToUp span — 140 ms measured, and the largest single cost in the chain by an
    // order of magnitude — doing nothing at all.
    //
    // It also removes the ability to change your mind mid-press, and that is deliberate:
    // chess's own TOUCH-MOVE RULE says that once you touch a piece you must move it. This
    // makes the app MORE faithful to over-the-board play, not less. (There is no drag
    // gesture here either, so in practice nothing is lost.)
    if (Action != AMOTION_EVENT_ACTION_DOWN) return 0;
    State->View.OnTap(AMotionEvent_getX(Event, 0), AMotionEvent_getY(Event, 0),
                      static_cast<float>(ANativeWindow_getWidth(App->window)),
                      static_cast<float>(ANativeWindow_getHeight(App->window)));
    return 1;
}

} // namespace

void android_main(android_app* App) {
    AppState State;
    App->userData = &State;
    App->onAppCmd = HandleCmd;
    App->onInputEvent = HandleInput;  // tap to select / move pieces

    // Persistent device identity (issue #17/#18): the same GUID the BLE role uses,
    // read from the app's internal data dir (== Context.filesDir, where the Kotlin
    // radio reads it too, so both agree). Drives colour + the per-opponent stats key.
    const char* DataDir = App->activity != nullptr ? App->activity->internalDataPath : nullptr;

    // #43: everything below used to be ~50 lines duplicated comment-for-comment with the iOS main
    // — Store, device id, SyncManager, the match-end persist+log, the hijack-guarded record send,
    // the ready/resync/hash/Sync handlers, Session::Start. All of it is engine choreography, none
    // of it is chess, and the two copies had drifted (ownership, capture style, wiring order, and
    // a MATCH END line whose format differed per phone). What remains here is what is genuinely
    // this app's: where files live, which radio role, how to log — and the game's four decisions.
    Lur::App::GameHost::Config HostCfg;
    HostCfg.SaveDir = DataDir != nullptr ? DataDir : ".";
    // The BLE transport. Hello exchanges the device GUIDs; colour comes from the two GUIDs, not
    // from the radio role.
    HostCfg.Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
    HostCfg.Log = [](const char* M) { LOGI("%s", M); };
    // Init before the view is attached: the view is handed Store/Sync/DeviceId, and it must hold
    // them before a peer can go ready (the ready handler calls back into the view's adopt rule).
    State.Host.Init(HostCfg);

    Lur::App::GameHost::RecordSync Rec;
    // The view applies the #38 hijack rule and sets identity + loads the record for the adopted
    // peer; the host sends our record only when it adopted. Both the initial link and a reconnect
    // route through this one hook now, so they cannot drift apart.
    Rec.OnPeerAdopted = [&State](const std::string& Peer) { return State.View.OnPeerLinked(Peer); };
    // Only share OUR game with the peer we are actually playing.
    Rec.IsActiveOpponent = [&State](const std::string& Peer) {
        return State.View.ActiveOpponentGuid() == Peer;
    };
    Rec.Summarize = [&State] {
        Lur::App::GameHost::RecordSync::MatchSummary S;
        S.Result     = static_cast<int>(State.Match.LastResult());
        S.WinsLower  = State.Match.Record().WinsLower;
        S.WinsHigher = State.Match.Record().WinsHigher;
        S.Draws      = State.Match.Record().Draws;
        S.Total      = State.Match.Record().TotalMatches();
        return S;
    };
    State.Host.EnableRecordSync(State.Match, std::move(Rec));


    State.View.SetState(&State.Match);
    State.View.AttachSession(&State.Host.Session());
    State.View.AttachPersistence(&State.Host.Store(), &State.Host.Sync(),
                                 State.Host.DeviceId());   // selector + match switching
    State.View.SetLogger([](const char* M) { LOGI("View: %s", M); });

    State.Match.SetOnMatchEnd([&State] { State.Host.OnMatchEnded(); });  // persist + report


    // Chess hashes its board so the session can catch a divergence (#72). RPS leaves StateHash unset
    // — it detects divergence itself with per-tick anchors.
    Lur::App::GameHost::Hooks Hooks;
    Hooks.StateHash = [&State] { return State.Match.PositionHash(); };
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
    while (!App->destroyRequested) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

#if LUR_AGENT
        const bool     WasMyTurn = State.Match.IsMyTurn();
        const uint32_t MatchesBefore = State.Match.Record().TotalMatches();
#endif
        State.Host.Tick(ElapsedNs);  // real-time-denominated: drives handshake + liveness
#if LUR_AGENT
        {
            const bool     NowMyTurn = State.Match.IsMyTurn();
            const uint32_t MatchesAfter = State.Match.Record().TotalMatches();
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
                                        ? State.View.AutoPlayRandomLegalMove(Rng) : false;
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
                     State.Match.IsMyTurn() ? 1 : 0, State.Match.Record().Moves.size(),
                     (unsigned)(State.Match.PositionHash() & 0xFFFFFFFFu),
                     State.Host.Session().IsAwaitingResync() ? 1 : 0,
                     (unsigned long long)RttCount,
                     (unsigned long long)(RttCount ? RttSumMs / RttCount : 0),
                     (unsigned long long)(RttCount ? RttMinMs : 0),
                     (unsigned long long)RttMaxMs,
                     State.Renderer != nullptr ? State.Renderer->PresentedFrames() : 0u);
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
        int Events = 0;
        android_poll_source* Source = nullptr;
        // Re-evaluate the timeout on every poll: INIT_WINDOW flips State.Ready
        // inside this loop, and a stale -1 would block forever before rendering.
        while (ALooper_pollOnce(State.Ready ? 0 : -1, nullptr, &Events,
                                reinterpret_cast<void**>(&Source)) >= 0) {
            if (Source != nullptr) Source->process(App, Source);
            if (App->destroyRequested) break;
        }

        if (State.Ready) {
            // #189: one more inbox drain, immediately before we draw. A peer move that
            // landed after the top-of-loop Tick would otherwise wait for the NEXT
            // iteration — and this loop is vsync-bound, so that is a whole refresh of
            // sitting still. Half a frame off every inbound move, on average.
            State.Host.PumpInbox();
            State.View.Render(State.Renderer,
                              static_cast<float>(ANativeWindow_getWidth(App->window)),
                              static_cast<float>(ANativeWindow_getHeight(App->window)));
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

    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
