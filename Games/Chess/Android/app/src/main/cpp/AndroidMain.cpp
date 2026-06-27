// Android entry point. NativeActivity + android_native_app_glue call android_main
// on a dedicated thread; we own the loop. This wires the platform window to the
// (Vulkan) renderer and proves the shared C++ core runs on-device. Game loop,
// input, and net wiring are tasks #9/#10.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Chess/Board.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/Transport.h"
#include "PieceMasks.h"  // cooked rhosgfx (CC0) silhouette masks, one per piece type

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)

namespace {

// Near-white / near-black piece tints. Each also serves as the OTHER colour's
// outline (drawn as a slightly larger silhouette behind the fill), so a white
// piece reads on a light square and a black piece on a dark one.
constexpr Lur::Render::Color PieceLightTint{0.97f, 0.97f, 0.95f, 1.0f};
constexpr Lur::Render::Color PieceDarkTint{0.13f, 0.13f, 0.15f, 1.0f};

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;

    Chess::Board Board;  // the position to draw

    // Render resources, built once the renderer is up. One shared unit quad is
    // drawn for every square and every piece with a per-instance model matrix.
    Lur::Render::MeshHandle     Quad = 0;
    Lur::Render::MaterialHandle LightSquare = 0;
    Lur::Render::MaterialHandle DarkSquare = 0;
    // Per piece TYPE (Chess::EPieceType order), a light- and a dark-tinted
    // material over that type's single silhouette texture (the "tint trick").
    Lur::Render::MaterialHandle PieceLight[6] = {};
    Lur::Render::MaterialHandle PieceDark[6] = {};
};

void CreateSceneResources(AppState* State) {
    using namespace Lur::Render;
    const Quad Q = MakeQuad();  // unit (0,0)-(1,1), white vertices
    State->Quad = State->Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    State->LightSquare = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.93f, 0.85f, 0.70f, 1.0f}, false});
    State->DarkSquare  = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.45f, 0.30f, 0.20f, 1.0f}, false});

    // Expand each single-channel coverage mask to RGBA (white, mask as alpha) and
    // upload once; the material tint supplies the piece colour at draw time.
    const int N = ChessArt::PieceMaskSize;
    std::vector<uint8_t> Rgba(static_cast<size_t>(N) * N * 4);
    for (int Type = 0; Type < 6; ++Type) {
        const unsigned char* Mask = ChessArt::PieceMask[Type];
        for (int i = 0; i < N * N; ++i) {
            Rgba[i * 4 + 0] = 255; Rgba[i * 4 + 1] = 255;
            Rgba[i * 4 + 2] = 255; Rgba[i * 4 + 3] = Mask[i];
        }
        const TextureHandle Tex = State->Renderer->LoadTexture(Rgba.data(), N, N);
        State->PieceLight[Type] = State->Renderer->CreateMaterial(MaterialDesc{Tex, PieceLightTint, false});
        State->PieceDark[Type]  = State->Renderer->CreateMaterial(MaterialDesc{Tex, PieceDarkTint, false});
    }

    State->Board = Chess::Board::StartPosition();
}

// One piece: an outline silhouette (contrasting tint, slightly larger) then the
// fill silhouette on top, both the shared quad scaled into the cell.
void DrawPiece(AppState* State, float CellX, float CellY, float Square, int Type, bool White) {
    using namespace Lur::Render;
    using Lur::Math::Mat4;

    const float FillSize = Square * 0.90f;
    const float FillOff  = (Square - FillSize) * 0.5f;
    const float OutSize  = FillSize * 1.08f;
    const float OutOff   = (Square - OutSize) * 0.5f;

    const MaterialHandle Fill    = White ? State->PieceLight[Type] : State->PieceDark[Type];
    const MaterialHandle Outline = White ? State->PieceDark[Type]  : State->PieceLight[Type];

    State->Renderer->DrawMesh(State->Quad, Outline,
        Mat4::Translation({CellX + OutOff, CellY + OutOff, 0.0f}) * Mat4::Scale({OutSize, OutSize, 1.0f}));
    State->Renderer->DrawMesh(State->Quad, Fill,
        Mat4::Translation({CellX + FillOff, CellY + FillOff, 0.0f}) * Mat4::Scale({FillSize, FillSize, 1.0f}));
}

// Draw the 8x8 board centred in the window, then the pieces (white at bottom).
void DrawScene(AppState* State, float Width, float Height) {
    using namespace Lur::Render;
    using Lur::Math::Mat4;

    const float BoardSize = (Width < Height ? Width : Height) * 0.95f;
    const float Square = BoardSize / 8.0f;
    const float OriginX = (Width - BoardSize) * 0.5f;
    const float OriginY = (Height - BoardSize) * 0.5f;

    for (int Row = 0; Row < 8; ++Row) {
        for (int File = 0; File < 8; ++File) {
            const Mat4 Model =
                Mat4::Translation({OriginX + File * Square, OriginY + Row * Square, 0.0f}) *
                Mat4::Scale({Square, Square, 1.0f});
            const MaterialHandle Mat = ((Row + File) % 2 == 0) ? State->LightSquare
                                                               : State->DarkSquare;
            State->Renderer->DrawMesh(State->Quad, Mat, Model);
        }
    }

    for (Chess::Square S = 0; S < 64; ++S) {
        const int File = S % 8;
        const int Rank = S / 8;  // 0 = rank 1 (White's back rank)
        Chess::EPieceType Type = Chess::PieceTypeAt(State->Board, Chess::EColor::White, S);
        bool White = true;
        if (Type == Chess::EPieceType::None) {
            Type = Chess::PieceTypeAt(State->Board, Chess::EColor::Black, S);
            White = false;
        }
        if (Type == Chess::EPieceType::None) continue;

        const int Row = 7 - Rank;  // White (rank 0) at the bottom of the screen
        DrawPiece(State, OriginX + File * Square, OriginY + Row * Square, Square,
                  static_cast<int>(Type), White);
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
                if (State->Ready) CreateSceneResources(State);

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
            DrawScene(&State, Width, Height);
            State.Renderer->EndFrame();
        }
    }

    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
