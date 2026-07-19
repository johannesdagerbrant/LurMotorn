#include "Rps/LockstepPeer.h"

#include "Lur/Core/Assert.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Rps/EventCodec.h"

namespace Rps {

void LockstepPeer::Init(uint64_t Seed, uint8_t InMyTeam, SendFn InSend, void* InCtx) {
    TheSim.Init(Seed);
    MyTeam = InMyTeam & 1u;
    Send = InSend;
    Ctx = InCtx;
    Delay = InputDelayTicks;
    LocalMasks.assign(Delay, 0);  // ticks 0..Delay-1 are empty by convention on BOTH peers
    PeerMasks.assign(Delay, 0);
    WallTicks = 0;
    PendingLocalMask = 0;
    Desync = false;
    MyHash.clear();
    PeerHash.clear();
}

void LockstepPeer::ProduceAndSend(uint8_t Mask) {
    LocalMasks.push_back(Mask);  // lands at exec tick Delay + WallTicks
    Lur::Serialization::BitWriter W;
    EncodeEvent(W, 1, Mask);     // live wire: delta always 1 (tick implicit by count)
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgInput, B.data(), B.size());
}

void LockstepPeer::Tick(uint64_t ElapsedNs) {
    const uint32_t N = Clock.AdvancePreserving(ElapsedNs, 64);
    for (uint32_t I = 0; I < N; ++I) {
        const uint8_t M = PendingLocalMask;
        PendingLocalMask = 0;
        ProduceAndSend(M);
        ++WallTicks;
    }
    Execute();
}

void LockstepPeer::Execute() {
    // Run at wallclock pace (TheSim.Tick < WallTicks), gated by BOTH input timelines.
    while (!Desync && TheSim.Tick < WallTicks &&
           TheSim.Tick < LocalMasks.size() && TheSim.Tick < PeerMasks.size()) {
        const uint32_t T = TheSim.Tick;
        LUR_ASSERT_MSG(T == TheSim.Tick, "lockstep: ticks must stay monotonic");
        const uint8_t Lm = LocalMasks[T], Pm = PeerMasks[T];
        // Both peers map (local, peer) -> (team0, team1) IDENTICALLY, so Step's args are
        // the same on both sides for tick T — the determinism precondition.
        const uint8_t M0 = MyTeam == 0 ? Lm : Pm;
        const uint8_t M1 = MyTeam == 0 ? Pm : Lm;
        TheSim.Step(M0, M1);
        if (TheSim.Tick % 10 == 0) EmitAnchor();
    }
}

void LockstepPeer::EmitAnchor() {
    const uint32_t T = TheSim.Tick;
    const uint32_t H = static_cast<uint32_t>(TheSim.StateHash());
    MyHash[T] = H;
    Lur::Serialization::BitWriter W;
    Lur::Serialization::WriteVarUint(W, T);
    W.WriteBits(H, 32);
    const std::vector<uint8_t>& B = W.Finish();
    if (Send) Send(Ctx, MsgAnchor, B.data(), B.size());
    CrossCheck(T);  // peer's anchor for T may already be in hand
}

void LockstepPeer::CrossCheck(uint32_t Tick) {
    const auto Mine = MyHash.find(Tick);
    const auto Theirs = PeerHash.find(Tick);
    if (Mine != MyHash.end() && Theirs != PeerHash.end() && Mine->second != Theirs->second)
        Desync = true;  // reliable transport + deterministic sim => a mismatch is always a bug
}

void LockstepPeer::OnMessage(Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N) {
    Lur::Serialization::BitReader R(Data, N);
    if (Type == MsgInput) {
        uint32_t D = 0;
        uint8_t M = 0;
        if (DecodeEvent(R, D, M)) {
            PeerMasks.push_back(M);  // live wire: each Input message is the next peer exec tick
            Execute();               // peer input may unblock the ceiling
        }
    } else if (Type == MsgAnchor) {
        const uint32_t T = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
        const uint32_t H = R.ReadBits(32);
        if (R.IsOk()) {
            PeerHash[T] = H;
            CrossCheck(T);
        }
    }
    // MsgResyncChunk: the chunked cold-rejoin path — a later increment of #76.
}

}  // namespace Rps
