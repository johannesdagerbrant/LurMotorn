// Android entry point. NativeActivity + android_native_app_glue call android_main
// on a dedicated thread; we own the loop. This wires the platform window to the
// (Vulkan) renderer and proves the shared C++ core runs on-device. Game loop,
// input, and net wiring are tasks #9/#10.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "Chess/Board.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;
};

void HandleCmd(android_app* App, int32_t Cmd) {
    auto* State = static_cast<AppState*>(App->userData);
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (App->window != nullptr) {
                State->Renderer = Lur::Render::VulkanRenderer::Create();
                State->Ready = State->Renderer && State->Renderer->Init(App->window);
                LOGI("Renderer init: %s", State->Ready ? "ok" : "failed");

                // Smoke test: the shared, perft-verified C++ core runs here.
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

} // namespace

void android_main(android_app* App) {
    AppState State;
    App->userData = &State;
    App->onAppCmd = HandleCmd;

    // Wire the BLE transport: log any datagram from the peer and bounce one reply
    // back, so a live two-phone link shows a round-trip in logcat. The radio is
    // driven from Kotlin (BleShim) once permissions are granted; this is the
    // engine-side seam. Real net/game wiring lands later (#10).
    auto* Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
    Transport->SetReceiver([Transport](const uint8_t* Data, std::size_t Size) {
        char Hex[49];
        const std::size_t Shown = Size < 16 ? Size : 16;
        for (std::size_t i = 0; i < Shown; ++i) std::snprintf(Hex + i * 3, 4, "%02X ", Data[i]);
        Hex[Shown ? Shown * 3 - 1 : 0] = '\0';
        LOGI("BLE received %zu bytes: %s", Size, Hex);
        static bool Replied = false;
        if (!Replied) { Replied = true; const uint8_t Pong[] = {0x5C}; Transport->Send(Pong, sizeof(Pong)); }
    });

    while (!App->destroyRequested) {
        int Events = 0;
        android_poll_source* Source = nullptr;
        // Block when idle (-1); spin when there's a frame to draw (0).
        const int Timeout = State.Ready ? 0 : -1;
        while (ALooper_pollOnce(Timeout, nullptr, &Events, reinterpret_cast<void**>(&Source)) >= 0) {
            if (Source != nullptr) Source->process(App, Source);
            if (App->destroyRequested) break;
        }

        if (State.Ready) {
            // TODO(#9/#10): renderer BeginFrame -> draw board -> EndFrame; pump
            // touch input and the net session here.
        }
    }

    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
