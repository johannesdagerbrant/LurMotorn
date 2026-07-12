// Android entry point. A thin platform shim: it owns the NativeActivity loop,
// creates the Vulkan renderer + BLE transport, and drives the shared
// Chess::BoardView (which owns all render + touch logic). The iOS app drives the
// same BoardView from its UIKit shim — one source of truth for the game view.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;
    Chess::BoardView View;
    Lur::Net::Session Session;
    Chess::ChessMatchState Match;   // authoritative game state (record + board + colour)
};

void HandleCmd(android_app* App, int32_t Cmd) {
    auto* State = static_cast<AppState*>(App->userData);
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (App->window != nullptr) {
                State->Renderer = Lur::Render::VulkanRenderer::Create();
                State->Ready = State->Renderer && State->Renderer->Init(App->window);
                LOGI("Renderer init: %s", State->Ready ? "ok" : "failed");
                if (State->Ready) State->View.CreateResources(State->Renderer);

                // Smoke test: the shared, perft-verified C++ core runs on-device.
                Chess::Board Board = Chess::Board::StartPosition();
                Chess::MoveList Moves;
                Chess::GenerateLegalMoves(Board, Moves);
                LOGI("Chess core alive: %d legal moves from the start position", Moves.Count);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (State->Renderer != nullptr) State->Renderer->Shutdown();
            State->Ready = false;
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
    State.Match.SetOnMatchEnd([&Sync] { Sync.Persist(); });  // durable all-time stats on game end

    // Wire the BLE transport into the net session. The session Hello exchanges the
    // device GUIDs; the shared BoardView renders + mutates State.Match and ships the
    // peer's moves via Chess::MoveCodec. Colour comes from the two GUIDs (not the
    // radio role). The per-opponent record syncs once per link establishment.
    auto* Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
    State.Session.SetLogger([](const char* M) { LOGI("Net: %s", M); });
    State.View.SetState(&State.Match);
    State.View.AttachSession(&State.Session);

    auto SendRecord = [&State, &Sync] {
        const std::vector<uint8_t> Snap = Sync.Snapshot();
        State.Session.Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
    };
    auto LogState = [&State](const char* Tag) {
        LOGI("Net: %s colour=%s toMove=%s moves=%zu myTurn=%d peer=%.6s", Tag,
             State.Match.MyColor() == Chess::EColor::White ? "White" : "Black",
             State.Match.SideToMove() == Chess::EColor::White ? "White" : "Black",
             State.Match.Record().Moves.size(), State.Match.IsMyTurn() ? 1 : 0,
             State.Session.GetPeerGuid().c_str());
    };
    State.Session.SetReadyHandler([&State, &Sync, &SendRecord, &DeviceId, &LogState] {
        State.Match.SetIdentity(DeviceId, State.Session.GetPeerGuid());  // colour + anchor
        Sync.OnLink(State.Session.GetPeerGuid());                        // load our stored record
        LogState("READY");
        SendRecord();                                                    // let the peer reconcile
    });
    State.Session.SetResyncHandler([&SendRecord, &LogState] { LogState("RESYNC"); SendRecord(); });
    State.Session.SetHandler(Lur::Net::EMsgType::Sync,
                             [&Sync, &LogState](const uint8_t* D, std::size_t N) {
                                 const bool Adopted = Sync.OnSync(D, N);
                                 LOGI("Net: SYNC recv %zuB adopted=%d", N, Adopted ? 1 : 0);
                                 LogState("post-SYNC");
                             });
    State.Session.Start(Transport, DeviceId);
    LOGI("Net session started (device id %zuB)", DeviceId.size());

    while (!App->destroyRequested) {
        State.Session.Tick();  // drive the Hello handshake until it completes
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
