#include "Chess/View/BoardView.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Chess/MatchMeta.h"
#include "Chess/MoveCodec.h"
#include "Chess/OpponentRegistry.h"
#include "Lur/Net/Session.h"
#include "Lur/Render/Sprite2D.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Text/BuiltinFonts.h"
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
    // Ignore moves from a peer that isn't the opponent we're currently playing — we
    // may be on a different (selected) game, and this peer's move index maps to their
    // board, not ours (hijack rule, #38).
    if (Net != nullptr && !ActiveOpponent.empty() && Net->GetPeerGuid() != ActiveOpponent)
        return;
    // Regenerate the identical legal list from our in-sync position; move ORDER is
    // the wire protocol, so the peer's index maps back to the exact same move.
    MoveList Legal; GenerateLegalMoves(State->CurrentBoard(), Legal);
    Lur::Serialization::BitReader R(Data, Size);
    const Move Mv = DecodeMove(R, Legal);
    if (!R.IsOk() || Mv == Move{}) return;                         // corrupt/out-of-range guard
    if (State->HasIdentity() && State->SideToMove() == State->MyColor()) return;  // not the peer's turn
    State->ApplyMove(Mv);
    StampMove();
    Selected = NoSquare;
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
            StampMove();
            Selected = NoSquare;
            return;
        }
    }

    // Not a move: select one's own piece (only on your turn), otherwise clear.
    Selected = (Mine != EPieceType::None && MyTurn) ? Sq : NoSquare;
}

void BoardView::AttachPersistence(Lur::Save::Store* Store, Lur::Save::SyncManager* SyncMgr,
                                  std::string LocalGuid) {
    Persist = Store;
    Sync = SyncMgr;
    DeviceId = std::move(LocalGuid);
    ItemsDirty = true;
}

void BoardView::StampMove() {
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
// First 12 hex of a GUID as three upper-case groups (e.g. "7F3A-C9E1-04B2"). The
// font atlas is ASCII, so use '-' (not a middot) as the separator.
std::string ShortGuid(const std::string& G) {
    auto Up = [](char C) { return (C >= 'a' && C <= 'f') ? static_cast<char>(C - 32) : C; };
    std::string S;
    for (int Grp = 0; Grp < 3; ++Grp) {
        if (Grp) S += '-';
        for (int K = 0; K < 4; ++K) {
            const std::size_t Idx = static_cast<std::size_t>(Grp) * 4 + K;
            S += (Idx < G.size()) ? Up(G[Idx]) : '0';
        }
    }
    return S;
}

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
