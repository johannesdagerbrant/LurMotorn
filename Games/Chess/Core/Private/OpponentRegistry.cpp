#include "Chess/OpponentRegistry.h"

#include "Chess/ChessMatchState.h"
#include "Chess/ChessRecord.h"

namespace Chess {

namespace {
// A save key is an opponent record iff it looks like a device GUID: 32 lowercase
// hex chars (see Lur::Save::LoadOrCreateDeviceId). This is how the game tells
// opponent records apart from control keys like "device-id"/"peer-id".
bool IsGuidKey(const std::string& K) {
    if (K.size() != 32) return false;
    for (char C : K)
        if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
    return true;
}
}  // namespace

std::vector<OpponentInfo> EnumerateOpponents(const Lur::Save::Store& Store,
                                             const std::string& LocalGuid) {
    std::vector<OpponentInfo> Out;
    for (const std::string& Key : Store.ListKeys()) {
        if (!IsGuidKey(Key) || Key == LocalGuid) continue;

        const std::vector<uint8_t> Blob = Store.Load(Key);

        // Replay the stored record under our identity to recover whose turn it is.
        // SetIdentity must precede Read: colour derives from the GUID order, and
        // Read() rebuilds the board from the move list.
        ChessMatchState St;
        St.SetIdentity(LocalGuid, Key);
        St.Read(Blob.data(), Blob.size());

        const ChessRecord& Rec = St.Record();
        OpponentInfo Info;
        Info.Guid   = Key;
        Info.MyTurn = St.IsMyTurn();
        // Tally is anchored to the lower GUID; orient it to this device.
        Info.Wins   = St.IsLocalLower() ? Rec.WinsLower  : Rec.WinsHigher;
        Info.Losses = St.IsLocalLower() ? Rec.WinsHigher : Rec.WinsLower;
        Info.Draws  = Rec.Draws;
        Info.MoveCount = static_cast<std::uint32_t>(Rec.Moves.size());
        Out.push_back(std::move(Info));
    }
    return Out;
}

} // namespace Chess
