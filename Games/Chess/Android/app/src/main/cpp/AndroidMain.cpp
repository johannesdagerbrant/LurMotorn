// Android entry point. A thin platform shim: it owns the NativeActivity loop,
// creates the Vulkan renderer + BLE transport, and drives the shared
// Chess::BoardView (which owns all render + touch logic). The iOS app drives the
// same BoardView from its UIKit shim — one source of truth for the game view.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>

#include "Chess/Board.h"
#include "Chess/View/BoardView.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;
    Chess::BoardView View;
    Lur::Net::Session Session;
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

    // Wire the BLE transport into the net session: the session runs the Hello
    // handshake (deterministic seat -> colour, independent of the BLE radio role)
    // and delivers the peer's moves to the shared BoardView, which encodes/decodes
    // via Chess::MoveCodec. A random nonce seeds the seat tie-break.
    auto* Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
    std::random_device Rd;
    const uint64_t Nonce = (static_cast<uint64_t>(Rd()) << 32) ^ Rd();
    State.Session.SetLogger([](const char* M) { LOGI("Net: %s", M); });
    State.View.AttachSession(&State.Session);
    State.Session.Start(Transport, Nonce);
    LOGI("Net session started (nonce hi=%08X)", static_cast<uint32_t>(Nonce >> 32));

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
