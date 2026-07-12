#include "Chess/ChessMatchState.h"

#include <algorithm>
#include <utility>

namespace Chess {

void ChessMatchState::SetIdentity(std::string_view Local, std::string_view Peer) {
    LocalGuid  = std::string(Local);
    PeerGuid   = std::string(Peer);
    LocalLower = LocalGuid < PeerGuid;  // fixed-width hex compare == numeric compare
    Identified = true;
}

EColor ChessMatchState::MyColor() const {
    // Even total matches -> the lower-GUID device is White; odd -> Black. (One fixed
    // polarity, agreed by both phones from the shared record.)
    const bool LowerIsWhite = (Rec.TotalMatches() % 2u) == 0u;
    const bool IAmWhite     = (LocalLower == LowerIsWhite);
    return IAmWhite ? EColor::White : EColor::Black;
}

void ChessMatchState::ApplyMove(const Move& M) {
    Position.MakeMove(M);
    Rec.Moves.push_back(M);
}

void ChessMatchState::RebuildBoard() {
    Position = Board::StartPosition();
    for (const Move& M : Rec.Moves) Position.MakeMove(M);
}

void ChessMatchState::Read(const uint8_t* Data, std::size_t Size) {
    Rec.Read(Data, Size);  // {} -> fresh defaults; corrupt -> unchanged
    RebuildBoard();
}

bool ChessMatchState::StrictlyNewer(const ChessRecord& A, const ChessRecord& B) {
    const std::pair<unsigned, std::size_t> KA{A.TotalMatches(), A.Moves.size()};
    const std::pair<unsigned, std::size_t> KB{B.TotalMatches(), B.Moves.size()};
    return KA > KB;
}

bool ChessMatchState::MergeIfNewer(const uint8_t* Data, std::size_t Size) {
    ChessRecord Incoming;
    if (!Incoming.Read(Data, Size)) return false;  // corrupt peer record: ignore

    if (StrictlyNewer(Incoming, Rec)) {            // peer strictly newer -> adopt
        Rec = std::move(Incoming);
        RebuildBoard();
        return true;
    }
    if (StrictlyNewer(Rec, Incoming)) return false;  // ours newer -> keep

    // Equal (totalMatches, moveCount). In correct turn-based chess this means the
    // identical position, so nothing to do. Defensive tie-break for the impossible
    // "equal counts, different content": the lower-GUID device's record wins, so if
    // WE are the higher GUID and the bytes differ, adopt the peer's.
    if (Identified && !LocalLower) {
        std::vector<uint8_t> Ours;
        Rec.Write(Ours);
        const bool Same = Ours.size() == Size &&
                          (Size == 0 || std::equal(Data, Data + Size, Ours.begin()));
        if (!Same) {
            Rec = std::move(Incoming);
            RebuildBoard();
            return true;
        }
    }
    return false;
}

} // namespace Chess
