#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Lur/Net/Session.h"  // EMsgType (the generic game slots)
#include "Lur/Sim/Tick.h"
#include "Rps/Sim.h"
#include "Rps/Tunables.h"

namespace Rps {

// RTS message-type aliases over the engine's generic game slots (#44). The engine
// names no game concept; the RTS gives 3..5 meaning here.
constexpr Lur::Net::EMsgType MsgInput       = Lur::Net::EMsgType::Game0;
constexpr Lur::Net::EMsgType MsgAnchor      = Lur::Net::EMsgType::Game1;
constexpr Lur::Net::EMsgType MsgResyncChunk = Lur::Net::EMsgType::Game2;

// Lockstep coordinator for ONE peer (design doc §1-§4). Drives a Sim in lockstep with
// the other peer over a reliable, ordered datagram link — which is what BLE GATT and
// the loopback both are, so this is lockstep without UDP's hard parts (no resend, no
// loss recovery; the only failure is link death).
//
// Single-threaded by design: the two-window loopback pumps BOTH peers on the main loop
// so every net-flow bug is reproducible in a debugger with both peers visible (the
// workbench point). The threaded SimRunner is the separate single-instance path.
//
// The model (§3):
//   * Each peer owns ONE team. Local input sampled at wall tick W is scheduled to
//     EXECUTE at tick W+Delay and sent immediately; the first Delay ticks are empty by
//     convention on both sides (nothing can be scheduled earlier).
//   * Execution tracks wallclock (one exec tick per wall tick) but is GATED: tick T
//     runs only once BOTH masks for T are known. If the peer's input for T hasn't
//     arrived, the sim stalls at the ceiling (T <= peer's watermark = their tick+Delay);
//     the fast peer waits, nobody sprints past a peer.
//   * Every 10 exec ticks an Anchor re-anchors the implicit tick count AND carries a
//     truncated StateHash; a mismatch (impossible under reliable transport + a
//     deterministic sim unless there's a bug) trips Desync -> declare a draw.
class LockstepPeer {
public:
    using SendFn = void (*)(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N);

    void Init(uint64_t Seed, uint8_t MyTeam, SendFn Send, void* Ctx);

    // OR in this frame's presses; consumed (once) by the next produced wall tick.
    void SetLocalMask(uint8_t Mask) { PendingLocalMask |= (Mask & 0xFu); }

    // Advance wallclock: produce + send local input for the new ticks, then execute as
    // far as the ceiling allows.
    void Tick(uint64_t ElapsedNs);

    // A datagram arrived (Input / Anchor / ResyncChunk), dispatched by type.
    void OnMessage(Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N);

    const Sim& GetSim() const { return TheSim; }
    uint32_t ExecTick() const { return TheSim.Tick; }
    bool Desynced() const { return Desync; }
    bool Stalled() const { return TheSim.Tick < WallTicks; }  // behind wallclock = waiting on peer

    // Flight recording (opt-in, off by default so it costs nothing): capture the
    // executed (mask0, mask1) per tick so a fresh Sim can replay the whole match to a
    // hash-identical state — the replay law (design §1), and the post-mortem dump on a
    // desync. Both peers execute the SAME stream, so either peer's recording replays both.
    void SetRecording(bool On) { Recording = On; }
    uint64_t Seed() const { return TheSim.Seed; }
    const std::vector<uint8_t>& RecordedTeam0() const { return RecM0; }
    const std::vector<uint8_t>& RecordedTeam1() const { return RecM1; }

private:
    void ProduceAndSend(uint8_t Mask);
    void Execute();
    void EmitAnchor();
    void CrossCheck(uint32_t Tick);

    Sim TheSim;
    Lur::Sim::TickClock Clock{TickRateHz};
    uint32_t Delay = InputDelayTicks;
    uint8_t MyTeam = 0;
    uint8_t PendingLocalMask = 0;

    std::vector<uint8_t> LocalMasks;  // index = exec tick (pre-seeded empty for 0..Delay-1)
    std::vector<uint8_t> PeerMasks;
    uint32_t WallTicks = 0;           // local wall ticks elapsed = the execution target

    std::unordered_map<uint32_t, uint32_t> MyHash, PeerHash;  // exec tick -> truncated StateHash
    bool Desync = false;

    bool Recording = false;
    std::vector<uint8_t> RecM0, RecM1;  // executed masks per tick (only while Recording)

    SendFn Send = nullptr;
    void* Ctx = nullptr;
};

}  // namespace Rps
