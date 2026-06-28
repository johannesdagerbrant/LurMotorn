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
    Lur::Render::MaterialHandle Highlight = 0;  // translucent select/target overlay

    Chess::Square Selected = Chess::NoSquare;    // currently picked-up square, if any
};

// Board placement in the window: a centred square, 0.95 of the shorter side.
struct BoardLayout { float OriginX, OriginY, Square; };

BoardLayout ComputeLayout(float Width, float Height) {
    const float BoardSize = (Width < Height ? Width : Height) * 0.95f;
    const float Square = BoardSize / 8.0f;
    return {(Width - BoardSize) * 0.5f, (Height - BoardSize) * 0.5f, Square};
}

// Screen-space top-left of a chess square's cell (White at the bottom).
void CellTopLeft(const BoardLayout& L, Chess::Square S, float& X, float& Y) {
    X = L.OriginX + (S % 8) * L.Square;
    Y = L.OriginY + (7 - S / 8) * L.Square;  // rank 0 (White) at the bottom row
}

// Map a screen point to a chess square, or NoSquare if outside the board.
Chess::Square SquareAt(const BoardLayout& L, float X, float Y) {
    const float Fx = (X - L.OriginX) / L.Square;
    const float Fy = (Y - L.OriginY) / L.Square;
    if (Fx < 0.0f || Fx >= 8.0f || Fy < 0.0f || Fy >= 8.0f) return Chess::NoSquare;
    const int File = static_cast<int>(Fx);
    const int Rank = 7 - static_cast<int>(Fy);
    return static_cast<Chess::Square>(Rank * 8 + File);
}

void CreateSceneResources(AppState* State) {
    using namespace Lur::Render;
    const Quad Q = MakeQuad();  // unit (0,0)-(1,1), white vertices
    State->Quad = State->Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    State->LightSquare = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.93f, 0.85f, 0.70f, 1.0f}, false});
    State->DarkSquare  = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.45f, 0.30f, 0.20f, 1.0f}, false});
    // Translucent green for the selected square and legal-move dots (alpha blended).
    State->Highlight   = State->Renderer->CreateMaterial(MaterialDesc{0, Color{0.30f, 0.85f, 0.40f, 0.55f}, false});

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

    const BoardLayout L = ComputeLayout(Width, Height);
    const float Square = L.Square;
    auto CellModel = [](float X, float Y, float Size) {
        return Mat4::Translation({X, Y, 0.0f}) * Mat4::Scale({Size, Size, 1.0f});
    };

    // Squares.
    for (int Row = 0; Row < 8; ++Row) {
        for (int File = 0; File < 8; ++File) {
            const MaterialHandle Mat = ((Row + File) % 2 == 0) ? State->LightSquare
                                                               : State->DarkSquare;
            State->Renderer->DrawMesh(State->Quad, Mat,
                CellModel(L.OriginX + File * Square, L.OriginY + Row * Square, Square));
        }
    }

    // Selected-square highlight (under the pieces). Generate the legal moves once,
    // here, to both highlight the selection and dot its targets below.
    Chess::MoveList Legal;
    Legal.Count = 0;
    if (State->Selected != Chess::NoSquare) {
        float X, Y; CellTopLeft(L, State->Selected, X, Y);
        State->Renderer->DrawMesh(State->Quad, State->Highlight, CellModel(X, Y, Square));
        Chess::GenerateLegalMoves(State->Board, Legal);
    }

    // Pieces.
    for (Chess::Square S = 0; S < 64; ++S) {
        Chess::EPieceType Type = Chess::PieceTypeAt(State->Board, Chess::EColor::White, S);
        bool White = true;
        if (Type == Chess::EPieceType::None) {
            Type = Chess::PieceTypeAt(State->Board, Chess::EColor::Black, S);
            White = false;
        }
        if (Type == Chess::EPieceType::None) continue;
        float X, Y; CellTopLeft(L, S, X, Y);
        DrawPiece(State, X, Y, Square, static_cast<int>(Type), White);
    }

    // Legal-target dots (over the pieces, so captures show too).
    const float Dot = Square * 0.30f;
    const float DotOff = (Square - Dot) * 0.5f;
    for (int i = 0; i < Legal.Count; ++i) {
        const Chess::Move& Mv = Legal.Moves[i];
        if (Mv.From != State->Selected) continue;
        float X, Y; CellTopLeft(L, Mv.To, X, Y);
        State->Renderer->DrawMesh(State->Quad, State->Highlight,
            CellModel(X + DotOff, Y + DotOff, Dot));
    }
}

// Apply a tap at screen (X,Y): select one's own piece, or move the selected piece
// to a legal target. Local hot-seat play -- side to move comes from the board.
void HandleTap(AppState* State, float X, float Y, float Width, float Height) {
    const BoardLayout L = ComputeLayout(Width, Height);
    const Chess::Square Sq = SquareAt(L, X, Y);
    if (Sq == Chess::NoSquare) { State->Selected = Chess::NoSquare; return; }

    const Chess::EColor Side = State->Board.SideToMove;
    const Chess::EPieceType Mine = Chess::PieceTypeAt(State->Board, Side, Sq);

    if (State->Selected != Chess::NoSquare) {
        // Try to move Selected -> Sq. Promotions appear as 4 entries (Q/R/B/N) for
        // the same From/To; default to the queen for now (a picker comes later).
        Chess::MoveList Legal; Chess::GenerateLegalMoves(State->Board, Legal);
        const Chess::Move* Chosen = nullptr;
        for (int i = 0; i < Legal.Count; ++i) {
            const Chess::Move& M = Legal.Moves[i];
            if (M.From != State->Selected || M.To != Sq) continue;
            if (M.Flags & Chess::MoveFlagPromotion) {
                if (M.Promo == Chess::EPieceType::Queen) Chosen = &M;
            } else {
                Chosen = &M;
            }
        }
        if (Chosen != nullptr) {
            State->Board.MakeMove(*Chosen);
            State->Selected = Chess::NoSquare;
            return;
        }
    }

    // Not a move: select one's own piece, otherwise clear.
    State->Selected = (Mine != Chess::EPieceType::None) ? Sq : Chess::NoSquare;
}

int32_t HandleInput(android_app* App, AInputEvent* Event) {
    auto* State = static_cast<AppState*>(App->userData);
    if (State == nullptr || !State->Ready || App->window == nullptr) return 0;
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((AMotionEvent_getAction(Event) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_UP)
        return 0;
    HandleTap(State, AMotionEvent_getX(Event, 0), AMotionEvent_getY(Event, 0),
              static_cast<float>(ANativeWindow_getWidth(App->window)),
              static_cast<float>(ANativeWindow_getHeight(App->window)));
    return 1;
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
    App->onInputEvent = HandleInput;  // tap to select / move pieces

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
