#include "Chess/View/BoardView.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Chess/Captures.h"
#include "Chess/MatchMeta.h"
#include "Chess/MoveCodec.h"
#include "Chess/OpponentRegistry.h"
#include "Lur/Hud/GuidLabel.h"   // ShortGuid (shared with RPS's selector)
#include "Lur/Net/Session.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Render/Rg8Pack.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Text/BuiltinFonts.h"
// Content reference the cook derives its work from (Cook/Cook.ps1): cook these piece
// PNGs (paths relative to this game's Content/) as an R8G8 shade+coverage set into the
// header included below. Order == Chess::EPieceType (Pawn,Knight,Bishop,Rook,Queen,King).
// LUR_COOK rg8-shade-coverage src=Pieces/wP.png,Pieces/wN.png,Pieces/wB.png,Pieces/wR.png,Pieces/wQ.png,Pieces/wK.png out=View/Private/PieceMasks.h ns=ChessArt size=PieceMaskSize coverage=PieceCoverage shade=PieceShade
#include "PieceMasks.h"  // cooked rhosgfx (CC0) silhouette masks, one per piece type

namespace Chess {
namespace {

// Piece shading, fed to the generic sprite shader per material (see MaterialDesc).
// One mask set renders both colours: the art's dark "ink" band [InkLo,InkHi] is
// recoloured to Outline and the fills get a Gamma tone curve.
//   * White pieces: near-white fill, DARK outline, gentle gamma (soft shadows).
//   * Black pieces: dark fill, WHITE outline, steep gamma (deep shadows).
// Legibility on same-colour squares comes from the baked outline, so no separate
// outline pass is needed.
constexpr Lur::Render::Color PieceLightTint{0.97f, 0.97f, 0.95f, 1.0f};
constexpr Lur::Render::Color PieceDarkTint{0.20f, 0.20f, 0.22f, 1.0f};
constexpr Lur::Render::Color OutlineOnLight{0.0f, 0.0f, 0.0f, 1.0f};  // dark outline for white pieces
constexpr Lur::Render::Color OutlineOnDark{1.0f, 1.0f, 1.0f, 1.0f};   // white outline for black pieces
constexpr float PieceInkLo = 0.32f;      // shade band the art's outline ink occupies
constexpr float PieceInkHi = 0.60f;
constexpr float PieceGammaLight = 1.3f;  // white pieces: gentle shadow contrast
constexpr float PieceGammaDark  = 3.0f;  // black pieces: deep shadow contrast

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

// What a move sounds like, classified from the position it is played IN (issue #78).
//
// It must be called BEFORE the move is applied, for two reasons. A capture is only
// visible pre-move — afterwards the destination square holds the capturing piece and the
// victim is gone. And a mating move auto-concludes the match, which RESETS the board, so
// asking the state about it afterwards would inspect a fresh start position.
//
// Check and mate need the position AFTER the move, so it is made on a throwaway copy: one
// board copy plus one move generation, on the move path only (never per frame).
EMoveSound ClassifyMove(const Board& Before, const Move& M) {
    const bool Captures = (M.Flags & MoveFlagEnPassant) != 0 ||
                          PieceTypeAt(Before, Opposite(Before.SideToMove), M.To) != EPieceType::None;

    Board After = Before;
    After.MakeMove(M);
    if (IsInCheck(After, After.SideToMove)) {
        MoveList Escapes;
        GenerateLegalMoves(After, Escapes);
        return Escapes.Count == 0 ? EMoveSound::Checkmate : EMoveSound::Check;
    }
    return Captures ? EMoveSound::Capture : EMoveSound::Move;
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
    // #193: the peer's anticipated pick-up. Deliberately a DIFFERENT hue from our own green
    // selection — it is a guess about what they are about to do, and must never be mistaken
    // for something that has actually happened.
    PeerHighlight = Renderer->CreateMaterial(MaterialDesc{0, Color{0.98f, 0.75f, 0.25f, 0.45f}, false});

    // Pack each piece into an R8G8 texture — R = shade, G = coverage — and upload once. The
    // interleave itself is Lur::Render::PackRg8 (promoted in #201: RPS packs the same primitive into
    // a side-by-side atlas). Why the layout: the shader multiplies the material tint by the shade,
    // so the tint supplies the piece colour while the art's highlights / mid-tones / dark outline
    // survive instead of flattening to a solid blob (issue #30).
    const int N = ChessArt::PieceMaskSize;
    std::vector<uint8_t> Rg(static_cast<size_t>(N) * N * 2);
    for (int Type = 0; Type < 6; ++Type) {
        Lur::Render::PackRg8(Rg.data(), N, 0, 0, ChessArt::PieceShade[Type],
                             ChessArt::PieceCoverage[Type], N, N);
        const TextureHandle Tex = Renderer->LoadTexture(Rg.data(), N, N, ETextureFormat::Rg8);

        MaterialDesc Light{Tex, PieceLightTint, false};
        Light.Outline = OutlineOnLight; Light.Gamma = PieceGammaLight;
        Light.InkLo = PieceInkLo; Light.InkHi = PieceInkHi;
        PieceLight[Type] = Renderer->CreateMaterial(Light);

        MaterialDesc Dark{Tex, PieceDarkTint, false};
        Dark.Outline = OutlineOnDark; Dark.Gamma = PieceGammaDark;
        Dark.InkLo = PieceInkLo; Dark.InkHi = PieceInkHi;
        PieceDark[Type] = Renderer->CreateMaterial(Dark);
    }

    // Built-in MSDF UI font: upload its atlas, then bind the score/result text field
    // and the opponent selector (both need the font atlas material).
    UiFont.Init(Lur::Text::InterFont());
    UiFont.UploadAtlas(*Renderer);
    Text.CreateResources(Renderer, &UiFont);
    Selector.CreateResources(Renderer, &UiFont);
}

void BoardView::RefreshCaptures() {
    if (State == nullptr) { Caps.Count = 0; return; }
    // Ply count alone would miss a switch to another opponent's game that happens to
    // stand at the same length; the position hash alone would miss a transposition
    // reached by a different capture order. Together they pin the tray to the board.
    const std::size_t   Plies = State->Record().Moves.size();
    const std::uint64_t Hash  = State->PositionHash();
    if (Plies == CapsPlies && Hash == CapsHash) return;
    CollectCaptures(State->Record().Moves, Caps);
    CapsPlies = Plies;
    CapsHash  = Hash;
}

bool BoardView::FlipBoard() const {
    return State != nullptr && State->HasIdentity() && State->MyColor() == EColor::Black;
}

bool BoardView::CanMoveNow() const {
    if (State == nullptr) return false;
    if (!State->HasIdentity()) return true;   // local hot-seat: either side may tap
    // Hold moves through the link-time resync: a move made before the peer's Sync
    // reconciles both boards would be decoded against a stale board and desync the
    // game permanently (issue #71). The gate lifts the instant the Sync lands (or a
    // short fallback timeout). Offline (link down) is NOT gated — see below.
    if (Net != nullptr && Net->IsAwaitingResync()) return false;
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

    // Capture trays (issue #67) sit in the margins the board leaves. Whether they fit
    // is decided here, before anything is drawn, because the bottom tray displaces the
    // score line below it. A near-square window has no margins to spare and simply
    // gets no trays — the phones and the 9:20 desktop window have room to burn.
    const float TrayH   = Sq * 0.50f;
    const float TrayGap = Sq * 0.07f;
    const float TopRoom = L.OriginY;
    const float BotRoom = HeightPx - (L.OriginY + Sq * 8.0f);
    const float TrayBox = TrayH + TrayGap * 2.0f;
    const bool  ShowTopTray = TopRoom >= TrayBox;
    const bool  ShowBotTray = BotRoom >= TrayBox + Sq * 1.2f;   // ...and still fit the score line

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
            const float FillSize = Sq * 0.94f;
            const float FillOff  = (Sq - FillSize) * 0.5f;
            const MaterialHandle Fill = White ? PieceLight[Idx] : PieceDark[Idx];
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

        // #193: the peer's anticipated pick-up, drawn under the pieces like our own
        // selection but in a different hue. Only while it is THEIR turn: on our turn a stale
        // hint would be pointing at a piece that can no longer move, and GenerateLegalMoves
        // below would be generating OUR moves, not theirs.
        if (PeerSelected != NoSquare && State->HasIdentity() && !State->IsMyTurn()) {
            float X, Y; CellTopLeft(L, PeerSelected, Flip, X, Y);
            Renderer->DrawMesh(QuadMesh, PeerHighlight, CellModel(X, Y, Sq));
            MoveList Theirs;
            GenerateLegalMoves(B, Theirs);   // side to move IS the peer here
            const float PDot = Sq * 0.22f;
            const float POff = (Sq - PDot) * 0.5f;
            for (int i = 0; i < Theirs.Count; ++i) {
                if (Theirs.Moves[i].From != PeerSelected) continue;
                float Tx, Ty; CellTopLeft(L, Theirs.Moves[i].To, Flip, Tx, Ty);
                Renderer->DrawMesh(QuadMesh, PeerHighlight, CellModel(Tx + POff, Ty + POff, PDot));
            }
        }

        // Capture trays. The ONE thing that decides both rows is which colour sits at
        // the near (bottom) edge of the screen — so they follow FlipBoard() for free
        // and stay correct when Black's view is rotated 180°.
        //   below the board = what the near player has taken (the far colour);
        //   above the board = the near player's own men, taken by the far player.
        RefreshCaptures();
        const EColor NearColor = Flip ? EColor::Black : EColor::White;
        auto DrawTray = [&](EColor Taken, float RowY) {
            // Overlap the icons slightly (a real capture tray stacks) and tighten the
            // step further if all 15 of a colour's men are gone, so a full tray still
            // spans no more than the board.
            const float Step = std::min(TrayH * 0.62f, (Sq * 8.0f) / 15.0f);
            float X = L.OriginX;
            for (int i = 0; i < Caps.Count; ++i) {
                if (Caps.Items[i].Color != Taken) continue;
                const int Idx = static_cast<int>(Caps.Items[i].Type);
                const MaterialHandle Mat = (Taken == EColor::White) ? PieceLight[Idx]
                                                                    : PieceDark[Idx];
                Renderer->DrawMesh(QuadMesh, Mat, CellModel(X, RowY, TrayH));
                X += Step;
            }
        };
        if (ShowTopTray) DrawTray(NearColor, L.OriginY - TrayGap - TrayH);
        if (ShowBotTray) DrawTray(Opposite(NearColor), L.OriginY + Sq * 8.0f + TrayGap);
    }

    // Everything below is HUD — enter the GUI layer so it composites on top of the
    // board, drawn by the engine's orthographic camera (see IRenderer::BeginGui).
    Renderer->BeginGui();

    // Opponent selector in the top margin (replaces the old link-state bar). Rebuild
    // the list when the link state changes (a peer linked/dropped) or a move landed.
    if (Persist != nullptr) {
        const int Link = (Net != nullptr) ? static_cast<int>(Net->GetLinkState()) : -1;
        if (Link != LastLink) { LastLink = Link; ItemsDirty = true; }
        if (ItemsDirty) { RebuildItems(); ItemsDirty = false; }

        // Top margin clears the system status bar (the surface is edge-to-edge). A
        // proportional inset is a stopgap until a real safe-area inset is plumbed in.
        const float TopInset = Sq * 0.62f;
        Selector.Draw(Renderer, "Current opponent", L.OriginX, TopInset, Sq * 8.0f, Sq * 0.62f);
    }

    // All-time W/L/D from THIS player's perspective, in the bottom margin (#22). The
    // record is player-agnostic (lower/higher GUID); IsLocalLower() orients it to me.
    if (State != nullptr && State->HasIdentity()) {
        const ChessRecord& Rec = State->Record();
        const int My    = State->IsLocalLower() ? Rec.WinsLower  : Rec.WinsHigher;
        const int Their = State->IsLocalLower() ? Rec.WinsHigher : Rec.WinsLower;
        char Buf[64];
        std::snprintf(Buf, sizeof(Buf), "You %d   Them %d   Draw %d", My, Their, Rec.Draws);

        // A compact score line in a band just below the board — under the capture tray
        // when there is one. Size is tied to the square (not the margin — portrait
        // margins are very tall), centred in the band.
        const float Band = Sq * 1.2f;
        const float BY   = L.OriginY + Sq * 8.0f + (ShowBotTray ? TrayBox : 0.0f);
        Text.Draw(Renderer, Buf, L.OriginX, BY, Sq * 8.0f, Band, Sq * 0.34f,
                  Color{0.92f, 0.92f, 0.95f, 1.0f},
                  Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle, false);
    }

    // Between-match result banner: shown centred over the (reset) board after a match
    // concludes, until the first move of the next match is played (#22).
    if (State != nullptr && State->Record().Moves.empty() &&
        State->LastResult() != EGameResult::Ongoing) {
        const char* Msg = "Draw";
        switch (State->LastResult()) {
            case EGameResult::Checkmate:      Msg = "Checkmate"; break;
            case EGameResult::Stalemate:      Msg = "Stalemate"; break;
            case EGameResult::DrawRepetition: Msg = "Draw by repetition"; break;
            case EGameResult::DrawFiftyMove:  Msg = "Draw by 75 moves"; break;
            default: break;   // Ongoing is filtered above; the rest read as a plain "Draw"
        }
        Text.Draw(Renderer, Msg, L.OriginX, L.OriginY, Sq * 8.0f, Sq * 8.0f, Sq * 0.8f,
                  Color{0.98f, 0.85f, 0.30f, 1.0f},
                  Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle, false);
    }

    if (PostGuiHook) PostGuiHook();  // app overlay (e.g. debug HUD), composited last

    Renderer->EndFrame();
}

void BoardView::AttachSession(Lur::Net::Session* Session) {
    Net = Session;
    // The view only needs peer moves + the link state. Identity/colour and the
    // link-time record sync are wired by the app (ChessMatchState + SyncManager).
    // A live move is FRAMED on chess's Game1 slot, like every other message. It used to
    // be a bare 1-byte datagram told apart by length (#19) — a chess assumption that had
    // to live inside the engine's dispatch to work, which is why it is gone (#200).
    Net->SetHandler(Lur::Net::EMsgType::Game1,
                    [this](const uint8_t* D, std::size_t N) { ApplyRemoteMove(D, N); });
    // #193: the peer's selection hint, on chess's Game0 slot. Cosmetic by construction:
    // it lands in a Square used for drawing and nowhere else.
    Net->SetHandler(Lur::Net::EMsgType::Game0, [this](const uint8_t* D, std::size_t N) {
        if (N < 1) return;
        // Same hijack guard as a move (#38): a hint from a peer we are not currently playing
        // describes THEIR board, and drawing it on ours would point at an unrelated piece.
        if (Net != nullptr && !ActiveOpponent.empty() && Net->GetPeerGuid() != ActiveOpponent)
            return;
        const Square S = static_cast<Square>(D[0]);
        SetPeerSelection(S <= NoSquare ? S : NoSquare);   // ignore a nonsense square
    });
}

void BoardView::ApplyRemoteMove(const uint8_t* Data, std::size_t Size) {
    if (State == nullptr) return;
    // Ignore moves from a peer that isn't the opponent we're currently playing — we
    // may be on a different (selected) game, and this peer's move index maps to their
    // board, not ours (hijack rule, #38).
    if (Net != nullptr && !ActiveOpponent.empty() && Net->GetPeerGuid() != ActiveOpponent)
        return;
    // NOTE: we do NOT drop inbound moves while AwaitingResync. At link time both peers
    // hold their OWN moves (CanMoveNow's send-gate), so nothing is in flight then; the
    // only time an inbound move arrives during a resync is MID-GAME, when a resync has
    // cleared asymmetrically — dropping it there caused a self-sustaining desync loop
    // (one resync per move, #72). Instead we apply it if it decodes; a stale-board
    // decode fails the guard below and triggers a resync, which self-heals.
    // Regenerate the identical legal list from our in-sync position; move ORDER is
    // the wire protocol, so the peer's index maps back to the exact same move.
    MoveList Legal; GenerateLegalMoves(State->CurrentBoard(), Legal);
    Lur::Serialization::BitReader R(Data, Size);
    const Move Mv = DecodeMove(R, Legal);
    if (!R.IsOk() || Mv == Move{}) {                               // index won't decode -> boards diverged
        if (Net != nullptr) Net->RequestResync();                 // heal instead of silently dropping (#72)
        return;
    }
    if (State->HasIdentity() && State->SideToMove() == State->MyColor()) return;  // not the peer's turn
    const EMoveSound Sound = ClassifyMove(State->CurrentBoard(), Mv);
    State->ApplyMove(Mv);
    StampMove(Sound);
    Selected = NoSquare;
    PeerSelected = NoSquare;   // #193: the anticipation is spent — the real move just landed
}

void BoardView::OnTap(float XPx, float YPx, float WidthPx, float HeightPx) {
    // The GUI layer gets first crack: if the selector consumed the tap (pill or an
    // open menu row), it must not also reach the board.
    if (Selector.OnTap(XPx, YPx)) {
        if (Selector.TookSelection()) {
            const int Sel = Selector.Selected();
            const std::string Chosen = (Sel >= 0 && Sel < static_cast<int>(ItemGuid.size()))
                                           ? ItemGuid[Sel] : std::string();
            SwitchActive(Chosen);   // switch the active match (or same-device local game)
        }
        return;
    }

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
            // Ship only the move's index (see MoveCodec), framed on chess's Game1 slot,
            // before applying locally, so both boards advance in lockstep off the same
            // pre-move legal list.
            if (Net != nullptr) {
                Lur::Serialization::BitWriter W;
                EncodeMove(*Chosen, Legal, W);
                const std::vector<uint8_t>& Bytes = W.Finish();
                // #190: EXPEDITED — this is the datagram the opponent is waiting to see land,
                // so it must not queue behind a keepalive or a multi-datagram resync.
                Net->Send(Lur::Net::EMsgType::Game1, Bytes.data(), Bytes.size(),
                          Lur::Transport::EBleSendPriority::Expedited);
            }
            const EMoveSound Sound = ClassifyMove(B, *Chosen);
            State->ApplyMove(*Chosen);
            StampMove(Sound);
            Selected = NoSquare;
            return;
        }
    }

    // Not a move: select one's own piece (only on your turn), otherwise clear.
    const Square Was = Selected;
    Selected = (Mine != EPieceType::None && MyTurn) ? Sq : NoSquare;
    // #193: tell the peer the instant we pick a piece up, so their board can start
    // anticipating BEFORE the move exists. Only on a change — holding a selection must not
    // dribble datagrams.
    if (Selected != Was) SendSelectionHint(Selected);
}

void BoardView::SendSelectionHint(Square S) {
    if (Net == nullptr) return;
    // One payload byte: the square, or NoSquare (64) to clear. Its own slot, so it can
    // never be confused with a move however short either payload gets.
    const uint8_t Payload = static_cast<uint8_t>(S);
    Net->Send(Lur::Net::EMsgType::Game0, &Payload, 1);
}

#if LUR_INTERNAL
namespace {
// Commit an already-chosen legal move over the SAME wire path OnTap uses: ship the
// index (encoded off the pre-move legal list) BEFORE applying, so both boards advance
// in lockstep. Factored so PlayMove and AutoPlay share one identical path. Returns the
// move's sound category, classified while the pre-move board is still standing.
EMoveSound CommitMove(Lur::Net::Session* Net, ChessMatchState* State, const Move& Chosen, const MoveList& Legal) {
    if (Net != nullptr) {
        Lur::Serialization::BitWriter W;
        EncodeMove(Chosen, Legal, W);
        const std::vector<uint8_t>& Bytes = W.Finish();
        // #190: EXPEDITED, same reasoning as OnTap's send — the peer is waiting on this one.
        Net->Send(Lur::Net::EMsgType::Game1, Bytes.data(), Bytes.size(),
                  Lur::Transport::EBleSendPriority::Expedited);
    }
    const EMoveSound Sound = ClassifyMove(State->CurrentBoard(), Chosen);
    State->ApplyMove(Chosen);
    return Sound;
}
}  // namespace

bool BoardView::PlayMove(Square From, Square To) {
    if (State == nullptr || !CanMoveNow()) return false;
    const Board& B = State->CurrentBoard();
    MoveList Legal; GenerateLegalMoves(B, Legal);
    const Move* Chosen = nullptr;
    for (int i = 0; i < Legal.Count; ++i) {
        const Move& M = Legal.Moves[i];
        if (M.From != From || M.To != To) continue;
        if (M.Flags & MoveFlagPromotion) { if (M.Promo == EPieceType::Queen) Chosen = &M; }
        else { Chosen = &M; }
    }
    if (Chosen == nullptr) return false;
    StampMove(CommitMove(Net, State, *Chosen, Legal));
    Selected = NoSquare;
    return true;
}

bool BoardView::AutoPlayRandomLegalMove(uint32_t& RngState) {
    if (State == nullptr || !CanMoveNow()) return false;
    MoveList Legal; GenerateLegalMoves(State->CurrentBoard(), Legal);
    if (Legal.Count <= 0) return false;                       // no move (game would have concluded)
    RngState = RngState * 1664525u + 1013904223u;             // Numerical-Recipes LCG
    const int Idx = static_cast<int>((RngState >> 8) % static_cast<uint32_t>(Legal.Count));
    StampMove(CommitMove(Net, State, Legal.Moves[Idx], Legal));
    Selected = NoSquare;
    return true;
}
#endif  // LUR_INTERNAL

void BoardView::AttachPersistence(Lur::Save::Store* Store, Lur::Save::SyncManager* SyncMgr,
                                  std::string LocalGuid) {
    Persist = Store;
    Sync = SyncMgr;
    DeviceId = std::move(LocalGuid);
    ItemsDirty = true;
}

void BoardView::StampMove(EMoveSound Sound) {
    // A move just landed on the board: sound it now (wait-free enqueue on the app side).
    if (MovePlayed) MovePlayed(Sound);
    // Stamp the last-move time against the active opponent and persist its record, so
    // an offline move survives and syncs on the next link. Same-device (empty) has no
    // opponent record to keep.
    if (Persist != nullptr && !ActiveOpponent.empty()) {
        Chess::MatchMeta M; M.LastMoveMs = Chess::NowMillisUtc();
        Chess::SaveMatchMeta(*Persist, ActiveOpponent, M);
        if (Sync != nullptr) Sync->Persist();
    }
    ItemsDirty = true;
}

void BoardView::SwitchActive(const std::string& Guid) {
    if (State == nullptr) return;
    if (Guid == ActiveOpponent) return;   // already active — nothing to do

    if (Sync != nullptr) Sync->Persist();  // save the game we're leaving (under its key)
    Selected = NoSquare;
    PeerSelected = NoSquare;   // #193: the hint belonged to the game we are leaving
    ActiveOpponent = Guid;

    if (Guid.empty()) {
        // "Same device": a fresh local both-sides game (no colour lock, no flip).
        State->ClearIdentity();
        State->Read(nullptr, 0);            // reset to the start position
        if (Sync != nullptr) Sync->Rebind("");
    } else {
        // Hard-load this opponent's stored game (a deliberate switch, not a merge).
        State->SetIdentity(DeviceId, Guid);
        const std::vector<uint8_t> Blob = Persist ? Persist->Load(Guid) : std::vector<uint8_t>{};
        State->Read(Blob.data(), Blob.size());
        if (Sync != nullptr) Sync->Rebind(Guid);
    }
    ItemsDirty = true;
    if (Log) {
        char B[96];
        std::snprintf(B, sizeof(B), "active -> %s", Guid.empty() ? "same-device" : Guid.c_str());
        Log(B);
    }
}

bool BoardView::OnPeerLinked(const std::string& PeerGuid) {
    // Hijack rule: adopt the peer only when we're on "same device" (the sole
    // auto-switch) or when it IS the opponent we've selected; otherwise keep playing
    // the opponent we're on.
    const bool Adopt = ActiveOpponent.empty() || ActiveOpponent == PeerGuid;
    if (Log) {
        char B[112];
        std::snprintf(B, sizeof(B), "peer linked %s -> %s", PeerGuid.c_str(),
                      Adopt ? "adopt (go live)" : "ignored (other game active)");
        Log(B);
    }
    if (!Adopt) { ItemsDirty = true; return false; }

    ActiveOpponent = PeerGuid;
    if (State != nullptr) State->SetIdentity(DeviceId, PeerGuid);
    if (Sync != nullptr)  Sync->OnLink(PeerGuid);   // monotonic reconcile (live)
    ItemsDirty = true;
    return true;
}

namespace {
using Lur::Hud::ShortGuid;   // moved to Modules/Hud so RPS's selector renders GUIDs identically

// Coarse "time ago" for the last-move sublabel.
std::string RelTime(std::uint64_t Ms) {
    const std::uint64_t S = Ms / 1000;
    char B[24];
    if (S < 60)         std::snprintf(B, sizeof(B), "%llus", static_cast<unsigned long long>(S));
    else if (S < 3600)  std::snprintf(B, sizeof(B), "%llum", static_cast<unsigned long long>(S / 60));
    else if (S < 86400) std::snprintf(B, sizeof(B), "%lluh", static_cast<unsigned long long>(S / 3600));
    else                std::snprintf(B, sizeof(B), "%llud", static_cast<unsigned long long>(S / 86400));
    return B;
}
}  // namespace

void BoardView::RebuildItems() {
    using Lur::Hud::DropdownItem;
    using Lur::Hud::ELeadStyle;
    using Lur::Render::Color;
    constexpr Color Green {0.30f, 0.85f, 0.40f, 1.0f};   // linked
    constexpr Color Black {0.06f, 0.07f, 0.09f, 1.0f};   // not linked
    constexpr Color Yellow{0.98f, 0.85f, 0.30f, 1.0f};   // your turn

    // "Linked" must reflect the CURRENT connection, not the IsReady() latch (which
    // stays set after a later disconnect) — else a dropped peer keeps its green dot.
    const bool Live = Net != nullptr && Net->GetLinkState() == Lur::Net::ELinkState::Linked;
    const std::string LinkedGuid = Live ? Net->GetPeerGuid() : std::string();

    std::vector<OpponentInfo> Ops = EnumerateOpponents(*Persist, DeviceId);
    std::vector<OpponentInfo> Online, Offline;
    for (const OpponentInfo& O : Ops) {
        if (!LinkedGuid.empty() && O.Guid == LinkedGuid) Online.push_back(O);
        else                                             Offline.push_back(O);
    }
    // Your-turn rows float to the top of each group (stable within the group).
    auto TurnFirst = [](const OpponentInfo& A, const OpponentInfo& B) {
        return A.MyTurn && !B.MyTurn;
    };
    std::stable_sort(Online.begin(),  Online.end(),  TurnFirst);
    std::stable_sort(Offline.begin(), Offline.end(), TurnFirst);

    std::vector<DropdownItem> Items;
    ItemGuid.clear();
    auto AddHeader = [&](const char* T) {
        DropdownItem H; H.Header = true; H.Label = T;
        Items.push_back(std::move(H)); ItemGuid.emplace_back();
    };
    auto AddOpp = [&](const OpponentInfo& O, bool Linked) {
        DropdownItem It;
        It.Lead = ELeadStyle::Dot;
        It.LeadFill = Linked ? Green : Black;
        It.Ring = O.MyTurn; It.RingColor = Yellow;
        It.Label = ShortGuid(O.Guid);
        const Chess::MatchMeta M = Chess::LoadMatchMeta(*Persist, O.Guid);
        if (M.LastMoveMs == 0) {
            It.Sublabel = O.MyTurn ? "your move" : "waiting";
        } else {
            const std::uint64_t Now = Chess::NowMillisUtc();
            const std::string Rel = RelTime(Now > M.LastMoveMs ? Now - M.LastMoveMs : 0);
            It.Sublabel = (O.MyTurn ? "moved " + Rel + " ago" : "you moved " + Rel + " ago");
        }
        Items.push_back(std::move(It)); ItemGuid.push_back(O.Guid);
    };

    if (!Online.empty())  { AddHeader("Online");  for (const auto& O : Online)  AddOpp(O, true);  }
    if (!Offline.empty()) { AddHeader("Offline"); for (const auto& O : Offline) AddOpp(O, false); }
    // "Same device" pinned at the very bottom.
    {
        DropdownItem It;
        It.Lead = ELeadStyle::Split;
        It.Label = "Same device";
        It.Sublabel = "Both sides";
        Items.push_back(std::move(It)); ItemGuid.emplace_back();
    }

    Selector.SetItems(Items.data(), static_cast<int>(Items.size()));

    // Select the row matching the active opponent; default to "same device" (last).
    int Sel = static_cast<int>(Items.size()) - 1;
    if (!ActiveOpponent.empty()) {
        for (std::size_t i = 0; i < ItemGuid.size(); ++i)
            if (!Items[i].Header && ItemGuid[i] == ActiveOpponent) { Sel = static_cast<int>(i); break; }
    }
    Selector.SetSelected(Sel);

    if (Log) {
        char B[128];
        std::snprintf(B, sizeof(B), "selector: %zu opp (%zu online) active=%s",
                      Ops.size(), Online.size(),
                      ActiveOpponent.empty() ? "same-device" : ShortGuid(ActiveOpponent).c_str());
        Log(B);
    }
}

} // namespace Chess
