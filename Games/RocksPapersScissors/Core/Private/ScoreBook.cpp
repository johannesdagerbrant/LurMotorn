#include "Rps/ScoreBook.h"

#include <cstring>

#include "Rps/Sim.h"   // Result* constants

namespace Rps {

namespace {

constexpr uint8_t Magic0 = 'R', Magic1 = 'S', Magic2 = 'B';
constexpr uint8_t FormatVersion = 1;

void PutU32(std::vector<uint8_t>& Out, uint32_t V) {
    Out.push_back(static_cast<uint8_t>(V & 0xFFu));
    Out.push_back(static_cast<uint8_t>((V >> 8) & 0xFFu));
    Out.push_back(static_cast<uint8_t>((V >> 16) & 0xFFu));
    Out.push_back(static_cast<uint8_t>((V >> 24) & 0xFFu));
}

// Fixed-width little-endian, NOT the slim varint wire codec — and that is deliberate. The slim-bytes
// rule exists for the BLE link, where every byte is latency; this blob is a few hundred bytes on
// local disk, where a layout you can read in a hex dump years later is worth more than 20 bytes.
uint32_t GetU32(const uint8_t* P) {
    return static_cast<uint32_t>(P[0]) | (static_cast<uint32_t>(P[1]) << 8) |
           (static_cast<uint32_t>(P[2]) << 16) | (static_cast<uint32_t>(P[3]) << 24);
}

// Did THIS device win, from the sim result and the side it played? Draw is neither.
enum class EOutcome : uint8_t { Win, Loss, Draw };
EOutcome OutcomeFor(uint8_t Result, uint8_t MyTeam) {
    if (Result == ResultDraw) return EOutcome::Draw;
    const bool Won = (Result == ResultTeam0Wins && MyTeam == 0) ||
                     (Result == ResultTeam1Wins && MyTeam == 1);
    return Won ? EOutcome::Win : EOutcome::Loss;
}

}  // namespace

Tally ScoreBook::Ai(int Tier) const {
    if (Tier < 0 || Tier >= AiTierCount) return Tally{};
    return Ai_[Tier];
}

int ScoreBook::FindPeer(std::string_view A, std::string_view B) const {
    if (A.size() != Lur::Save::DeviceIdHexLen || B.size() != Lur::Save::DeviceIdHexLen) return -1;
    const std::string_view Lo = A < B ? A : B, Hi = A < B ? B : A;
    for (int I = 0; I < PeerCount_; ++I)
        if (std::memcmp(Peers_[I].Lower, Lo.data(), Lur::Save::DeviceIdHexLen) == 0 &&
            std::memcmp(Peers_[I].Higher, Hi.data(), Lur::Save::DeviceIdHexLen) == 0)
            return I;
    return -1;
}

Tally ScoreBook::Peer(std::string_view PeerGuid, std::string_view MyGuid) const {
    const int I = FindPeer(PeerGuid, MyGuid);
    if (I < 0) return Tally{};
    // ORIENT HERE, not at write time: the stored row is anchored to GUID order so both phones can
    // hold the same bytes. Whichever of us sorts lower owns WinsLower.
    const bool AmLower = MyGuid < PeerGuid;
    Tally T;
    T.Wins   = AmLower ? Peers_[I].WinsLower : Peers_[I].WinsHigher;
    T.Losses = AmLower ? Peers_[I].WinsHigher : Peers_[I].WinsLower;
    T.Draws  = Peers_[I].Draws;
    return T;
}

void ScoreBook::RecordAi(int Tier, uint8_t Result, uint8_t MyTeam) {
    if (Tier < 0 || Tier >= AiTierCount) return;
    switch (OutcomeFor(Result, MyTeam)) {
        case EOutcome::Win:  ++Ai_[Tier].Wins;   break;
        case EOutcome::Loss: ++Ai_[Tier].Losses; break;
        case EOutcome::Draw: ++Ai_[Tier].Draws;  break;
    }
}

bool ScoreBook::RecordPeer(std::string_view PeerGuid, std::string_view MyGuid, uint8_t Result,
                           uint8_t MyTeam) {
    if (!Lur::Save::IsValidDeviceId(PeerGuid) || !Lur::Save::IsValidDeviceId(MyGuid)) return false;
    if (PeerGuid == MyGuid) return false;   // a device is not its own rival (loopback / same-device)
    int I = FindPeer(PeerGuid, MyGuid);
    if (I < 0) {
        if (PeerCount_ >= MaxPeers) return false;
        I = PeerCount_++;
        const std::string_view Lo = MyGuid < PeerGuid ? MyGuid : PeerGuid;
        const std::string_view Hi = MyGuid < PeerGuid ? PeerGuid : MyGuid;
        std::memcpy(Peers_[I].Lower, Lo.data(), Lur::Save::DeviceIdHexLen);
        std::memcpy(Peers_[I].Higher, Hi.data(), Lur::Save::DeviceIdHexLen);
        Peers_[I].WinsLower = Peers_[I].WinsHigher = Peers_[I].Draws = 0;
    }
    const bool AmLower = MyGuid < PeerGuid;
    switch (OutcomeFor(Result, MyTeam)) {
        case EOutcome::Win:  ++(AmLower ? Peers_[I].WinsLower : Peers_[I].WinsHigher); break;
        case EOutcome::Loss: ++(AmLower ? Peers_[I].WinsHigher : Peers_[I].WinsLower); break;
        case EOutcome::Draw: ++Peers_[I].Draws; break;
    }
    return true;
}

void ScoreBook::Write(std::vector<uint8_t>& Out) const {
    Out.push_back(Magic0);
    Out.push_back(Magic1);
    Out.push_back(Magic2);
    Out.push_back(FormatVersion);
    // COUNT-PREFIXED, which is the whole reason a rung can ever be added or removed again: the
    // 2026-07-30 ladder collapse deleted a tier, and a fixed-length array would have read an old
    // 4-tier blob as though every tier had shifted one rung down.
    Out.push_back(static_cast<uint8_t>(AiTierCount));
    for (int T = 0; T < AiTierCount; ++T) {
        PutU32(Out, Ai_[T].Wins);
        PutU32(Out, Ai_[T].Losses);
        PutU32(Out, Ai_[T].Draws);
    }
    Out.push_back(static_cast<uint8_t>(PeerCount_));
    for (int I = 0; I < PeerCount_; ++I) {
        Out.insert(Out.end(), Peers_[I].Lower, Peers_[I].Lower + Lur::Save::DeviceIdHexLen);
        Out.insert(Out.end(), Peers_[I].Higher, Peers_[I].Higher + Lur::Save::DeviceIdHexLen);
        PutU32(Out, Peers_[I].WinsLower);
        PutU32(Out, Peers_[I].WinsHigher);
        PutU32(Out, Peers_[I].Draws);
    }
}

bool ScoreBook::Read(const uint8_t* Data, std::size_t Size) {
    *this = ScoreBook{};                       // absent or corrupt both mean "fresh"
    if (Size == 0) return true;                // no record yet is not an error
    if (Size < 5 || Data[0] != Magic0 || Data[1] != Magic1 || Data[2] != Magic2) return false;
    if (Data[3] != FormatVersion) return false;
    std::size_t P = 4;
    const int StoredTiers = Data[P++];
    for (int T = 0; T < StoredTiers; ++T) {
        if (P + 12 > Size) return false;
        // A tier the CURRENT build no longer has is READ AND DROPPED, not an error: that is what
        // makes removing a rung a non-event for a device that already has a record.
        if (T < AiTierCount) {
            Ai_[T].Wins   = GetU32(Data + P);
            Ai_[T].Losses = GetU32(Data + P + 4);
            Ai_[T].Draws  = GetU32(Data + P + 8);
        }
        P += 12;
    }
    if (P >= Size) return false;               // the peer count is mandatory, even at zero
    const int StoredPeers = Data[P++];
    constexpr std::size_t RowSize = 2 * Lur::Save::DeviceIdHexLen + 12;
    for (int I = 0; I < StoredPeers; ++I) {
        if (P + RowSize > Size) return false;
        if (PeerCount_ < MaxPeers) {
            PeerEntry& E = Peers_[PeerCount_++];
            std::memcpy(E.Lower, Data + P, Lur::Save::DeviceIdHexLen);
            std::memcpy(E.Higher, Data + P + Lur::Save::DeviceIdHexLen, Lur::Save::DeviceIdHexLen);
            E.WinsLower  = GetU32(Data + P + 2 * Lur::Save::DeviceIdHexLen);
            E.WinsHigher = GetU32(Data + P + 2 * Lur::Save::DeviceIdHexLen + 4);
            E.Draws      = GetU32(Data + P + 2 * Lur::Save::DeviceIdHexLen + 8);
        }
        P += RowSize;
    }
    return true;
}

bool ScoreBook::Load(const Lur::Save::Store& S) {
    const std::vector<uint8_t> Blob = S.Load(StoreKey);
    return Read(Blob.data(), Blob.size());
}

bool ScoreBook::Save(Lur::Save::Store& S) const {
    std::vector<uint8_t> Blob;
    Write(Blob);
    return S.Save(StoreKey, Blob.data(), Blob.size());
}

}  // namespace Rps
