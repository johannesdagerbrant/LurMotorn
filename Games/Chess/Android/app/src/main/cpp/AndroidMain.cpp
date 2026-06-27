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
#include "Lur/Render/Sprite2D.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;

    // Board render resources (built once the renderer is up). One shared unit
    // quad drawn 64 times with a per-square model matrix + light/dark material.
    Lur::Render::MeshHandle     Quad = 0;
    Lur::Render::MaterialHandle LightSquare = 0;
    Lur::Render::MaterialHandle DarkSquare = 0;
};

// Build the board's GPU resources: one unit quad + two materials. Cheap, done
// once after the renderer initialises.
void CreateBoardResources(AppState* State) {
    using namespace Lur::Render;
    const Quad Q = MakeQuad();  // unit (0,0)-(1,1), white vertices
    State->Quad = State->Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    State->LightSquare = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.93f, 0.85f, 0.70f, 1.0f}, false});
    State->DarkSquare  = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.45f, 0.30f, 0.20f, 1.0f}, false});
}

// Draw the 8x8 board centred in the window, each square a tinted quad.
void DrawBoard(AppState* State, float Width, float Height) {
    using namespace Lur::Render;
    using Lur::Math::Mat4;
    using Lur::Math::Vec3;

    const float BoardSize = (Width < Height ? Width : Height) * 0.95f;
    const float Square = BoardSize / 8.0f;
    const float OriginX = (Width - BoardSize) * 0.5f;
    const float OriginY = (Height - BoardSize) * 0.5f;

    for (int Rank = 0; Rank < 8; ++Rank) {
        for (int File = 0; File < 8; ++File) {
            const Mat4 Model =
                Mat4::Translation({OriginX + File * Square, OriginY + Rank * Square, 0.0f}) *
                Mat4::Scale({Square, Square, 1.0f});
            const MaterialHandle Mat = ((Rank + File) % 2 == 0) ? State->LightSquare
                                                                : State->DarkSquare;
            State->Renderer->DrawMesh(State->Quad, Mat, Model);
        }
    }
}

void HandleCmd(android_app* App, int32_t Cmd) {
    auto* State = static_cast<AppState*>(App->userData);
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (App->window != nullptr) {
                State->Renderer = Lur::Render::VulkanRenderer::Create();
                State->Ready = State->Renderer && State->Renderer->Init(App->window);
                LOGI("Renderer init: %s", State->Ready ? "ok" : "failed");
                if (State->Ready) CreateBoardResources(State);

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
        // Block when idle (-1); spin when there's a frame to draw (0). The timeout
        // MUST be re-evaluated on every poll, not captured once: INIT_WINDOW flips
        // State.Ready to true *inside* this inner loop, and a stale -1 would then
        // block forever on the next poll and we'd never reach the render block.
        while (ALooper_pollOnce(State.Ready ? 0 : -1, nullptr, &Events,
                                reinterpret_cast<void**>(&Source)) >= 0) {
            if (Source != nullptr) Source->process(App, Source);
            if (App->destroyRequested) break;
        }

        if (State.Ready) {
            // Pixel-space ortho camera matching the window; draw the board quads.
            // Touch input and the net session pump in here later.
            const auto Width  = static_cast<float>(ANativeWindow_getWidth(App->window));
            const auto Height = static_cast<float>(ANativeWindow_getHeight(App->window));
            const Lur::Render::Camera Cam = Lur::Render::MakeOrthoCamera(Width, Height);
            State.Renderer->BeginFrame(Cam);
            DrawBoard(&State, Width, Height);
            State.Renderer->EndFrame();
        }
    }

    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
