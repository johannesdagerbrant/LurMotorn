// Android entry point. NativeActivity + android_native_app_glue call android_main
// on a dedicated thread; we own the loop. This wires the platform window to the
// (Vulkan) renderer and proves the shared C++ core runs on-device. Game loop,
// input, and net wiring are tasks #9/#10.
#include <android_native_app_glue.h>
#include <android/log.h>

#include "Chess/Board.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"

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
