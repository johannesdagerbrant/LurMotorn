#include "Chess/View/BoardView.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Chess/MoveCodec.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "PieceMasks.h"  // cooked rhosgfx (CC0) silhouette masks, one per piece type

namespace Chess {
namespace {

// Near-white / near-black piece tints. Each also serves as the OTHER colour's
// outline (a slightly larger silhouette behind the fill), so a white piece reads
// on a light square and a black piece on a dark one.
constexpr Lur::Render::Color PieceLightTint{0.97f, 0.97f, 0.95f, 1.0f};
constexpr Lur::Render::Color PieceDarkTint{0.13f, 0.13f, 0.15f, 1.0f};

// Board placement in the window: a centred square, 0.95 of the shorter side.
struct BoardLayout { float OriginX, OriginY, Square; };

BoardLayout ComputeLayout(float Width, float Height) {
    const float BoardSize = (Width < Height ? Width : Height) * 0.95f;
    const float Square = BoardSize / 8.0f;
    return {(Width - BoardSize) * 0.5f, (Height - BoardSize) * 0.5f, Square};
}

// Screen-space top-left of a chess square's cell (White at the bottom).
void CellTopLeft(const BoardLayout& L, Square S, float& X, float& Y) {
    X = L.OriginX + (S % 8) * L.Square;
    Y = L.OriginY + (7 - S / 8) * L.Square;  // rank 0 (White) at the bottom row
}

// Map a screen point to a chess square, or NoSquare if outside the board.
Square SquareAt(const BoardLayout& L, float X, float Y) {
    const float Fx = (X - L.OriginX) / L.Square;
    const float Fy = (Y - L.OriginY) / L.Square;
    if (Fx < 0.0f || Fx >= 8.0f || Fy < 0.0f || Fy >= 8.0f) return NoSquare;
    const int File = static_cast<int>(Fx);
    const int Rank = 7 - static_cast<int>(Fy);
    return static_cast<Square>(Rank * 8 + File);
}

} // namespace

void BoardView::CreateResources(Lur::Render::IRenderer* Renderer) {
    using namespace Lur::Render;

    const ::Lur::Render::Quad Q = MakeQuad();  // unit (0,0)-(1,1), white vertices
    QuadMesh = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    LightSquare = Renderer->CreateMaterial(MaterialDesc{0, Color{0.93f, 0.85f, 0.70f, 1.0f}, false});
    DarkSquare  = Renderer->CreateMaterial(MaterialDesc{0, Color{0.45f, 0.30f, 0.20f, 1.0f}, false});
    Highlight   = Renderer->CreateMaterial(MaterialDesc{0, Color{0.30f, 0.85f, 0.40f, 0.55f}, false});

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
        const TextureHandle Tex = Renderer->LoadTexture(Rgba.data(), N, N);
        PieceLight[Type] = Renderer->CreateMaterial(MaterialDesc{Tex, PieceLightTint, false});
        PieceDark[Type]  = Renderer->CreateMaterial(MaterialDesc{Tex, PieceDarkTint, false});
    }
}

void BoardView::Render(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx) {
    using namespace Lur::Render;
    using Lur::Math::Mat4;

    const BoardLayout L = ComputeLayout(WidthPx, HeightPx);
    const float Sq = L.Square;
    auto CellModel = [](float X, float Y, float Size) {
        return Mat4::Translation({X, Y, 0.0f}) * Mat4::Scale({Size, Size, 1.0f});
    };

    Renderer->BeginFrame(MakeOrthoCamera(WidthPx, HeightPx));

    // Squares.
    for (int Row = 0; Row < 8; ++Row) {
        for (int File = 0; File < 8; ++File) {
            const MaterialHandle Mat = ((Row + File) % 2 == 0) ? LightSquare : DarkSquare;
            Renderer->DrawMesh(QuadMesh, Mat,
                CellModel(L.OriginX + File * Sq, L.OriginY + Row * Sq, Sq));
        }
    }

    // Selected-square highlight (under the pieces); generate the legal moves once
    // to both highlight the selection and dot its targets below.
    MoveList Legal;
    Legal.Count = 0;
    if (Selected != NoSquare) {
        float X, Y; CellTopLeft(L, Selected, X, Y);
        Renderer->DrawMesh(QuadMesh, Highlight, CellModel(X, Y, Sq));
        GenerateLegalMoves(Position, Legal);
    }

    // Pieces.
    for (Square S = 0; S < 64; ++S) {
        EPieceType Type = PieceTypeAt(Position, EColor::White, S);
        bool White = true;
        if (Type == EPieceType::None) {
            Type = PieceTypeAt(Position, EColor::Black, S);
            White = false;
        }
        if (Type == EPieceType::None) continue;

        float X, Y; CellTopLeft(L, S, X, Y);
        const int Idx = static_cast<int>(Type);
        const float FillSize = Sq * 0.90f;
        const float FillOff  = (Sq - FillSize) * 0.5f;
        const float OutSize  = FillSize * 1.08f;
        const float OutOff   = (Sq - OutSize) * 0.5f;
        const MaterialHandle Fill    = White ? PieceLight[Idx] : PieceDark[Idx];
        const MaterialHandle Outline = White ? PieceDark[Idx]  : PieceLight[Idx];
        Renderer->DrawMesh(QuadMesh, Outline, CellModel(X + OutOff, Y + OutOff, OutSize));
        Renderer->DrawMesh(QuadMesh, Fill, CellModel(X + FillOff, Y + FillOff, FillSize));
    }

    // Legal-target dots (over the pieces, so captures show too).
    const float Dot = Sq * 0.30f;
    const float DotOff = (Sq - Dot) * 0.5f;
    for (int i = 0; i < Legal.Count; ++i) {
        const Move& Mv = Legal.Moves[i];
        if (Mv.From != Selected) continue;
        float X, Y; CellTopLeft(L, Mv.To, X, Y);
        Renderer->DrawMesh(QuadMesh, Highlight, CellModel(X + DotOff, Y + DotOff, Dot));
    }

    Renderer->EndFrame();
}

void BoardView::AttachSession(Lur::Net::Session* Session) {
    Net = Session;
    Net->SetReadyHandler([this] {
        MyColor = (Net->GetSeat() == 0) ? EColor::White : EColor::Black;
    });
    Net->SetHandler(Lur::Net::EMsgType::Move,
                    [this](const uint8_t* D, std::size_t N) { ApplyRemoteMove(D, N); });
}

void BoardView::ApplyRemoteMove(const uint8_t* Data, std::size_t Size) {
    // Regenerate the identical legal list from our in-sync position; move ORDER is
    // the wire protocol, so the peer's index maps back to the exact same move.
    MoveList Legal; GenerateLegalMoves(Position, Legal);
    Lur::Serialization::BitReader R(Data, Size);
    const Move Mv = DecodeMove(R, Legal);
    if (!R.IsOk() || Mv == Move{}) return;                 // corrupt/out-of-range: desync guard
    if (Position.SideToMove == MyColor) return;            // not the peer's turn: ignore
    Position.MakeMove(Mv);
    Selected = NoSquare;
}

void BoardView::OnTap(float XPx, float YPx, float WidthPx, float HeightPx) {
    const BoardLayout L = ComputeLayout(WidthPx, HeightPx);
    const Square Sq = SquareAt(L, XPx, YPx);
    if (Sq == NoSquare) { Selected = NoSquare; return; }

    // In a networked game you may act only on your own turn, and only once the
    // handshake has assigned colours. Local hot-seat (no session) is always "my turn".
    const bool MyTurn = (Net == nullptr) ||
                        (Net->IsReady() && Position.SideToMove == MyColor);

    const EColor Side = Position.SideToMove;
    const EPieceType Mine = PieceTypeAt(Position, Side, Sq);

    if (Selected != NoSquare && MyTurn) {
        // Try to move Selected -> Sq. Promotions appear as 4 entries (Q/R/B/N) for
        // the same From/To; default to the queen for now (a picker comes later).
        MoveList Legal; GenerateLegalMoves(Position, Legal);
        const Move* Chosen = nullptr;
        for (int i = 0; i < Legal.Count; ++i) {
            const Move& M = Legal.Moves[i];
            if (M.From != Selected || M.To != Sq) continue;
            if (M.Flags & MoveFlagPromotion) {
                if (M.Promo == EPieceType::Queen) Chosen = &M;
            } else {
                Chosen = &M;
            }
        }
        if (Chosen != nullptr) {
            // Ship only the move's index (see MoveCodec) before applying locally, so
            // both boards advance in lockstep off the same pre-move legal list.
            if (Net != nullptr) {
                Lur::Serialization::BitWriter W;
                EncodeMove(*Chosen, Legal, W);
                const std::vector<uint8_t>& Bytes = W.Finish();
                Net->Send(Lur::Net::EMsgType::Move, Bytes.data(), Bytes.size());
            }
            Position.MakeMove(*Chosen);
            Selected = NoSquare;
            return;
        }
    }

    // Not a move: select one's own piece (only on your turn), otherwise clear.
    Selected = (Mine != EPieceType::None && MyTurn) ? Sq : NoSquare;
}

} // namespace Chess
