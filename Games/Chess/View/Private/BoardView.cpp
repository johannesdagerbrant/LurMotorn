#include "Chess/View/BoardView.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Chess/MoveCodec.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Text/BuiltinFonts.h"
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

// Screen-space top-left of a chess square's cell. Flip == false puts White at the
// bottom; Flip == true rotates the board 180° so the Black player sees their own
// pieces at the near edge.
void CellTopLeft(const BoardLayout& L, Square S, bool Flip, float& X, float& Y) {
    int File = S % 8;
    int Rank = S / 8;
    if (Flip) { File = 7 - File; Rank = 7 - Rank; }
    X = L.OriginX + File * L.Square;
    Y = L.OriginY + (7 - Rank) * L.Square;  // rank 0 at the bottom row (of this view)
}

// Map a screen point to a chess square, or NoSquare if outside the board. Flip is
// the same board orientation used by CellTopLeft, so touch matches what is drawn.
Square SquareAt(const BoardLayout& L, float X, float Y, bool Flip) {
    const float Fx = (X - L.OriginX) / L.Square;
    const float Fy = (Y - L.OriginY) / L.Square;
    if (Fx < 0.0f || Fx >= 8.0f || Fy < 0.0f || Fy >= 8.0f) return NoSquare;
    int File = static_cast<int>(Fx);
    int Rank = 7 - static_cast<int>(Fy);
    if (Flip) { File = 7 - File; Rank = 7 - Rank; }
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

    StatusBar.CreateResources(Renderer);  // engine link-state widget owns its palette

    // Pack each piece into an R8G8 texture — R = shade (the source art's tones),
    // G = coverage (silhouette alpha) — and upload once. The shader multiplies the
    // material tint by the shade, so the tint supplies the piece colour while the
    // art's highlights / mid-tones / dark outline survive instead of flattening to
    // a solid blob (issue #30).
    const int N = ChessArt::PieceMaskSize;
    std::vector<uint8_t> Rg(static_cast<size_t>(N) * N * 2);
    for (int Type = 0; Type < 6; ++Type) {
        const unsigned char* Shade    = ChessArt::PieceShade[Type];
        const unsigned char* Coverage = ChessArt::PieceCoverage[Type];
        for (int i = 0; i < N * N; ++i) {
            Rg[i * 2 + 0] = Shade[i];      // R = shade
            Rg[i * 2 + 1] = Coverage[i];   // G = coverage
        }
        const TextureHandle Tex = Renderer->LoadTexture(Rg.data(), N, N, ETextureFormat::Rg8);
        PieceLight[Type] = Renderer->CreateMaterial(MaterialDesc{Tex, PieceLightTint, false});
        PieceDark[Type]  = Renderer->CreateMaterial(MaterialDesc{Tex, PieceDarkTint, false});
    }

    // Built-in MSDF UI font: upload its atlas, then bind the score/result text field.
    UiFont.Init(Lur::Text::InterFont());
    UiFont.UploadAtlas(*Renderer);
    Text.CreateResources(Renderer, &UiFont);
}

bool BoardView::FlipBoard() const {
    return State != nullptr && State->HasIdentity() && State->MyColor() == EColor::Black;
}

bool BoardView::CanMoveNow() const {
    if (State == nullptr) return false;
    if (!State->HasIdentity()) return true;   // local hot-seat: either side may tap
    // Offline move (issue #19): a player may move on their turn even while the link
    // is down — you can only ever be one move ahead (then it's the opponent's turn),
    // and the next link-establishment record sync heals it.
    return State->IsMyTurn();
}

void BoardView::Render(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx) {
    using namespace Lur::Render;
    using Lur::Math::Mat4;

    const BoardLayout L = ComputeLayout(WidthPx, HeightPx);
    const float Sq = L.Square;
    const bool  Flip = FlipBoard();
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

    if (State != nullptr) {
        const Board& B = State->CurrentBoard();

        // Selected-square highlight (under the pieces); generate the legal moves once
        // to both highlight the selection and dot its targets below.
        MoveList Legal;
        Legal.Count = 0;
        if (Selected != NoSquare) {
            float X, Y; CellTopLeft(L, Selected, Flip, X, Y);
            Renderer->DrawMesh(QuadMesh, Highlight, CellModel(X, Y, Sq));
            GenerateLegalMoves(B, Legal);
        }

        // Pieces.
        for (Square S = 0; S < 64; ++S) {
            EPieceType Type = PieceTypeAt(B, EColor::White, S);
            bool White = true;
            if (Type == EPieceType::None) {
                Type = PieceTypeAt(B, EColor::Black, S);
                White = false;
            }
            if (Type == EPieceType::None) continue;

            float X, Y; CellTopLeft(L, S, Flip, X, Y);
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
            float X, Y; CellTopLeft(L, Mv.To, Flip, X, Y);
            Renderer->DrawMesh(QuadMesh, Highlight, CellModel(X + DotOff, Y + DotOff, Dot));
        }
    }

    // Link-state indicator: hand the engine widget a slim rect in the top margin;
    // it owns the colour per state. Absent in a local hot-seat (no session).
    if (Net != nullptr) {
        const float Inset = Sq * 0.12f;
        const float MaxH  = L.OriginY - 2.0f * Inset;   // available top-margin height
        float BarH = Sq * 0.5f;
        if (BarH > MaxH) BarH = MaxH;
        StatusBar.Draw(Renderer, Net->GetLinkState(), L.OriginX, Inset, Sq * 8.0f, BarH);
    }

    // All-time W/L/D from THIS player's perspective, in the bottom margin (#22). The
    // record is player-agnostic (lower/higher GUID); IsLocalLower() orients it to me.
    if (State != nullptr && State->HasIdentity()) {
        const ChessRecord& Rec = State->Record();
        const int My    = State->IsLocalLower() ? Rec.WinsLower  : Rec.WinsHigher;
        const int Their = State->IsLocalLower() ? Rec.WinsHigher : Rec.WinsLower;
        char Buf[64];
        std::snprintf(Buf, sizeof(Buf), "You %d   Them %d   Draw %d", My, Their, Rec.Draws);

        // A compact score line in a band just below the board. Size is tied to the
        // square (not the margin — portrait margins are very tall), centred in the band.
        const float Band = Sq * 1.2f;
        const float BY   = L.OriginY + Sq * 8.0f;
        Text.Draw(Renderer, Buf, L.OriginX, BY, Sq * 8.0f, Band, Sq * 0.34f,
                  Color{0.92f, 0.92f, 0.95f, 1.0f},
                  Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle, false);
    }

    // Between-match result banner: shown centred over the (reset) board after a match
    // concludes, until the first move of the next match is played (#22).
    if (State != nullptr && State->Record().Moves.empty() &&
        State->LastResult() != EGameResult::Ongoing) {
        const char* Msg = "Draw";
        if (State->LastResult() == EGameResult::Checkmate)      Msg = "Checkmate";
        else if (State->LastResult() == EGameResult::Stalemate) Msg = "Stalemate";
        Text.Draw(Renderer, Msg, L.OriginX, L.OriginY, Sq * 8.0f, Sq * 8.0f, Sq * 0.8f,
                  Color{0.98f, 0.85f, 0.30f, 1.0f},
                  Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle, false);
    }

    Renderer->EndFrame();
}

void BoardView::AttachSession(Lur::Net::Session* Session) {
    Net = Session;
    // The view only needs peer moves + the link state. Identity/colour and the
    // link-time record sync are wired by the app (ChessMatchState + SyncManager).
    // A live move is a bare 1-byte datagram (issue #19), so it uses the move hook.
    Net->SetMoveHandler([this](const uint8_t* D, std::size_t N) { ApplyRemoteMove(D, N); });
}

void BoardView::ApplyRemoteMove(const uint8_t* Data, std::size_t Size) {
    if (State == nullptr) return;
    // Regenerate the identical legal list from our in-sync position; move ORDER is
    // the wire protocol, so the peer's index maps back to the exact same move.
    MoveList Legal; GenerateLegalMoves(State->CurrentBoard(), Legal);
    Lur::Serialization::BitReader R(Data, Size);
    const Move Mv = DecodeMove(R, Legal);
    if (!R.IsOk() || Mv == Move{}) return;                         // corrupt/out-of-range guard
    if (State->HasIdentity() && State->SideToMove() == State->MyColor()) return;  // not the peer's turn
    State->ApplyMove(Mv);
    Selected = NoSquare;
}

void BoardView::OnTap(float XPx, float YPx, float WidthPx, float HeightPx) {
    if (State == nullptr) return;
    const BoardLayout L = ComputeLayout(WidthPx, HeightPx);
    const Square Sq = SquareAt(L, XPx, YPx, FlipBoard());
    if (Sq == NoSquare) { Selected = NoSquare; return; }

    const bool MyTurn = CanMoveNow();
    const Board& B = State->CurrentBoard();
    const EColor Side = B.SideToMove;
    const EPieceType Mine = PieceTypeAt(B, Side, Sq);

    if (Selected != NoSquare && MyTurn) {
        // Try to move Selected -> Sq. Promotions appear as 4 entries (Q/R/B/N) for
        // the same From/To; default to the queen for now (a picker comes later).
        MoveList Legal; GenerateLegalMoves(B, Legal);
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
            // Ship only the move's index as a bare 1-byte datagram (see MoveCodec)
            // before applying locally, so both boards advance in lockstep off the same
            // pre-move legal list.
            if (Net != nullptr) {
                Lur::Serialization::BitWriter W;
                EncodeMove(*Chosen, Legal, W);
                const std::vector<uint8_t>& Bytes = W.Finish();
                Net->SendMove(Bytes.data(), Bytes.size());
            }
            State->ApplyMove(*Chosen);
            Selected = NoSquare;
            return;
        }
    }

    // Not a move: select one's own piece (only on your turn), otherwise clear.
    Selected = (Mine != EPieceType::None && MyTurn) ? Sq : NoSquare;
}

} // namespace Chess
