#include "Rps/SimRunner.h"

namespace Rps {

// The gate is HELD while the gated team has no mining camp. #139: solo must not begin until the
// player has placed their opening camp, and the wait must not be banked as catch-up — that second
// part is Lur::Sim::SimThread's job, this predicate only says whether we are still waiting.
bool SimRunner::Held(void* Ctx, const Sim& S) {
    const SimRunner* Self = static_cast<const SimRunner*>(Ctx);
    return !S.HasMinerCamp(static_cast<uint8_t>(Self->PreMatchTeam));
}

// A batch RELEASES the gate only if it carries a PLACEABLE opening camp for the gated team. The
// CanPlaceBuilding check is the load-bearing half: without it an invalid drop starts the clock and
// the match opens with no camp, which reads as the placement having silently vanished.
bool SimRunner::Opens(void* Ctx, const Sim& S, const InputEvent* Evs, int Count) {
    const SimRunner* Self = static_cast<const SimRunner*>(Ctx);
    const uint8_t Team = static_cast<uint8_t>(Self->PreMatchTeam);
    for (int I = 0; I < Count; ++I) {
        const InputEvent& E = Evs[I];
        if (E.Kind == EventPlaceBuilding && E.Type == UnitMiner && E.Team == Team &&
            S.CanPlaceBuilding(E.Team, E.Type, Fixed{E.X}, Fixed{E.Y}))
            return true;
    }
    return false;
}

void SimRunner::Start(uint64_t Seed, InputFn Input, void* Ctx, uint32_t StressPerTeam,
                      bool DisableCombat, int InPreMatchTeam) {
    PreMatchTeam = InPreMatchTeam;

    // Seed and pre-fill BEFORE the thread exists — no other thread may be reading the sim.
    Sim& S = Thread.GetSim();
    S.Init(Seed);
#if LUR_INTERNAL
    if (StressPerTeam > 0) S.StressFill(static_cast<int32_t>(StressPerTeam));
    S.DisableCombat = DisableCombat;  // --flockdemo (#97): pure flocking, no kills
    S.LiveCvLatch = true;             // solo (no peer to sync with): console edits apply live
#else
    (void)StressPerTeam; (void)DisableCombat;
#endif

    Engine::Hooks H;
    H.Input = Input;
    H.InputCtx = Ctx;
    if (PreMatchTeam >= 0) {
        H.Held = &Held;
        H.Opens = &Opens;
        H.GateCtx = this;
    }
    Thread.Start(H);
}

}  // namespace Rps
