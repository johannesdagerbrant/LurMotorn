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
#include <vector>
#if LUR_INTERNAL
#include <sys/system_properties.h>  // read debug.onlychess.autoplay (dev soak/autoplay)
#endif

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Chess/View/SfxLibrary.h"
#include "Lur/Audio/AudioDevice.h"
#include "Lur/Audio/Mixer.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
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
    Lur::Net::Session Session;
    Chess::ChessMatchState Match;   // authoritative game state (record + board + colour)
    Lur::Save::SyncManager* Sync = nullptr;  // for persist-on-background (set in android_main)
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
                State->Renderer = Lur::Render::VulkanRenderer::Create();
                State->Ready = State->Renderer && State->Renderer->Init(App->window);
                LOGI("Renderer init: %s", State->Ready ? "ok" : "failed");
                if (State->Ready) State->View.CreateResources(State->Renderer);

                // Bring up audio: load the cooked SFX into the mixer, wire a move to a
                // click, then open the low-latency stream. Order matters — Sfx.Load (which
                // calls Mixer::Add) must finish before the device thread starts pulling.
                if (State->Audio == nullptr) {
                    State->Mixer.Init(Lur::Audio::Mixer::DefaultRate);
                    State->Sfx.Load(State->Mixer);
                    AppState* St = State;
                    State->View.SetMovePlayed(
                        [St] { St->Sfx.Play(St->Mixer, Chess::ESfx::Move); });
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
            if (State->Sync != nullptr) State->Sync->Persist();
            break;
        default:
            break;
    }
}

int32_t HandleInput(android_app* App, AInputEvent* Event) {
    auto* State = static_cast<AppState*>(App->userData);
    if (State == nullptr || !State->Ready || App->window == nullptr) return 0;
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((AMotionEvent_getAction(Event) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_UP)
        return 0;
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
    Lur::Save::Store Store(DataDir != nullptr ? DataDir : ".");
    const std::string DeviceId = Lur::Save::LoadOrCreateDeviceId(Store);
    Lur::Save::SyncManager Sync(Store, State.Match);
    State.Sync = &Sync;                                       // for persist-on-background
    State.Match.SetOnMatchEnd([&Sync, &State] {
        Sync.Persist();                                      // durable all-time stats on game end
        LOGI("Net: MATCH END result=%d WLD(lo/hi/dr)=%u/%u/%u total=%u",
             static_cast<int>(State.Match.LastResult()), State.Match.Record().WinsLower,
             State.Match.Record().WinsHigher, State.Match.Record().Draws,
             State.Match.Record().TotalMatches());
    });

    // Wire the BLE transport into the net session. The session Hello exchanges the
    // device GUIDs; the shared BoardView renders + mutates State.Match and ships the
    // peer's moves via Chess::MoveCodec. Colour comes from the two GUIDs (not the
    // radio role). The per-opponent record syncs once per link establishment.
    auto* Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
    State.Session.SetLogger([](const char* M) { LOGI("Net: %s", M); });
    State.View.SetState(&State.Match);
    State.View.AttachSession(&State.Session);
    State.View.AttachPersistence(&Store, &Sync, DeviceId);  // selector + match switching
    State.View.SetLogger([](const char* M) { LOGI("View: %s", M); });

    auto SendRecord = [&State, &Sync] {
        // Only share OUR game with the peer we're actually playing (hijack rule, #38).
        if (State.View.ActiveOpponentGuid() != State.Session.GetPeerGuid()) return;
        const std::vector<uint8_t> Snap = Sync.Snapshot();
        State.Session.Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
    };
    // The view applies the hijack rule and sets identity + loads the record for the
    // adopted peer; we send our record only when it adopted this peer. Both the
    // initial link (ReadyHandler) AND a reconnect (ResyncHandler) go through it, so a
    // peer that (re)joins is adopted per the hijack rule — not just on first contact.
    auto OnLive = [&State, &SendRecord] {
        if (State.View.OnPeerLinked(State.Session.GetPeerGuid())) SendRecord();
    };
    State.Session.SetReadyHandler(OnLive);
    State.Session.SetResyncHandler(OnLive);                              // reconnect: re-adopt + re-sync
    State.Session.SetStateHashFn([&State] { return State.Match.PositionHash(); });  // desync detection (#72)
    State.Session.SetHandler(Lur::Net::EMsgType::Sync,
                             [&State, &Sync](const uint8_t* D, std::size_t N) {
                                 if (State.View.ActiveOpponentGuid() == State.Session.GetPeerGuid())
                                     Sync.OnSync(D, N);
                             });
    State.Session.Start(Transport, DeviceId);
    LOGI("Net session started (device id %zuB)", DeviceId.size());

#if LUR_INTERNAL
    // Dev-only autoplayer (issue #57/#58): when debug.onlychess.autoplay=1, play a
    // random legal move the SAME frame it becomes our turn — so a reply ships the frame
    // the opponent's move was received. Gated by a runtime prop so an ordinary dev
    // build still plays by hand; compiled out entirely of a SHIPPING build.
    bool AutoEnabled = false;
    uint32_t Rng = 0xC0FFEEu ^ static_cast<uint32_t>(DeviceId.size());
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

#if LUR_INTERNAL
        const bool     WasMyTurn = State.Match.IsMyTurn();
        const uint32_t MatchesBefore = State.Match.Record().TotalMatches();
#endif
        State.Session.Tick(ElapsedNs);  // real-time-denominated: drives handshake + liveness
#if LUR_INTERNAL
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
                const bool Played = (State.Session.IsReady() && NowMyTurn)
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
                     State.Session.IsAwaitingResync() ? 1 : 0,
                     (unsigned long long)RttCount,
                     (unsigned long long)(RttCount ? RttSumMs / RttCount : 0),
                     (unsigned long long)(RttCount ? RttMinMs : 0),
                     (unsigned long long)RttMaxMs,
                     State.Renderer != nullptr ? State.Renderer->PresentedFrames() : 0u);
            }
            ++Frame;
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
            State.View.Render(State.Renderer,
                              static_cast<float>(ANativeWindow_getWidth(App->window)),
                              static_cast<float>(ANativeWindow_getHeight(App->window)));
        }
    }

    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
