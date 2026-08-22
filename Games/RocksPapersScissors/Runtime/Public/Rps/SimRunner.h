#pragma once
#include <cstdint>

#include "Lur/Sim/SimThread.h"
#include "Rps/Sim.h"
#include "Rps/Snapshot.h"
#include "Rps/Tunables.h"

namespace Rps {

// RPS's solo sim thread. Since #201 the thread, the clock, the bounded-catch-up rule and the
// hold-drops-elapsed-time rule all live in Lur::Sim::SimThread; what remains here is the RPS-shaped
// part of the original class, and that is exactly three things:
//
//   * the SEEDING and the LUR_INTERNAL pre-fill (the #75 stress scene, --flockdemo's combat toggle,
//     the solo live-CVar latch) — game rules about what a fresh sim looks like;
//   * the two PRE-MATCH GATE predicates (#139): held while the gated team has no mining camp,
//     released by a batch carrying a PLACEABLE opening camp for that team. "Placeable" is
//     Sim::CanPlaceBuilding, so an invalid drop cannot start the clock;
//   * the type bindings (Sim / Snapshot / InputEvent / TickRateHz / MaxEventsPerTick).
//
// Everything else — the ~1 kHz service loop, the per-tick publish, input sampled by tick number,
// idempotent Stop — is engine now, with its own host tests that sabotage each rule.
//
// This wrapper keeps the original signature rather than exposing SimThread directly, because the
// stress/gate arguments belong together with Init: a caller must not be able to spawn the thread and
// *then* seed the sim.
class SimRunner {
public:
    using Engine = Lur::Sim::SimThread<Sim, Snapshot, InputEvent, MaxEventsPerTick>;
    using InputFn = Engine::InputFn;

    // Spawn the sim thread. Init(Seed) runs on the caller before the thread starts.
    // StressPerTeam > 0 (LUR_INTERNAL) bulk-spawns that many soldiers per side first — the #75 stress
    // scene (tick budget + one-draw render at the raised cap).
    // PreMatchTeam >= 0 arms the pre-match hold: nothing is stepped until a tick's input batch
    // contains an acceptable opening mining camp for that team — then the whole batch applies at tick
    // 0, the human's camp and the AI's together, exactly as two lockstep peers begin. -1 (default) =
    // no gate, which is what the stress/flock scenes need: they have no camp and would never tick.
    void Start(uint64_t Seed, InputFn Input, void* Ctx, uint32_t StressPerTeam = 0,
               bool DisableCombat = false, int PreMatchTeam = -1);

    void Stop() { Thread.Stop(); }

    // --- Consumer (render thread), safe while running ---
    bool LatestSnapshot(Snapshot& Out) const { return Thread.LatestSnapshot(Out); }
    uint32_t PublishedTick() const { return Thread.PublishedTick(); }

    // --- Post-Stop() accessors (thread joined; no other thread touches the sim) ---
    uint64_t FinalStateHash() const { return Thread.GetSim().StateHash(); }
    uint32_t FinalTick() const { return Thread.FinalTick(); }

private:
    static bool Held(void* Ctx, const Sim& S);
    static bool Opens(void* Ctx, const Sim& S, const InputEvent* Evs, int Count);

    // One constant for the catch-up burst cap, shared with LockstepPeer::Execute — the two used to
    // be separate 8s kept in step by a comment.
    Engine Thread{TickRateHz, MaxExecTicksPerService};
    int PreMatchTeam = -1;   // >=0: hold the clock until this team's opening camp lands (see Start)
};

} // namespace Rps
