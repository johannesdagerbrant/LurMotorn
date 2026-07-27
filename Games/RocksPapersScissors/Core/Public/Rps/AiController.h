#pragma once
// Rps::AiController — the single-player opponent (design: Docs/Journal/2026-07-22/
// rps-ai-opponent-spec.md, epic #120). Its SOLE interface to the game is the SAME input EVENTS
// a human issues (#137 buildings rework — place a building, queue units at it): DecideEvents
// (state, tick) reads the (const) sim and emits this tick's events for its team. It never
// touches sim state — so it is structurally fair (can't do anything a player can't) and
// deterministic (integer/Fixed only, seeded SplitMix64 on its own stream, ticks not wall-clock),
// which keeps single-player inside the same replay/rollback model as a networked match.
//
// The tier structure (staleness/precision/cadence/hysteresis + the Opening/Building/Reacting/
// AllIn FSM) carries over from the press-model AI unchanged; only the ACTION changed — instead
// of pressing a unit type it ensures a building of that type exists (placing a forced mining
// camp first) and queues units there.
//
// Difficulty is NOT a handicap: every tier plays by identical rules and has identical actions.
// They differ only in (a) how STALE their read of the enemy army is, (b) how PRECISE (fuzzy
// buckets -> exact), and (c) how often they RE-DECIDE (reaction cadence). A weak tier reacts to
// old, fuzzy scouting slowly, so it mis-counters — readable and punishable, not random. All the
// numbers live in per-tier CVars (rps.ai.<tier>.*, Tunables.h); the structure is here in code.
#include <cstdint>

#include "Lur/Sim/Random.h"
#include "Rps/Sim.h"
#include "Rps/Tunables.h"

namespace Rps {

enum class EAiTier : uint8_t { Easy = 0, Medium = 1, Hard = 2 };

// The nine per-tier knobs, resolved from the latched CvSnapshot for one tier.
struct AiKnobs {
    int32_t OpenWorkers, WorkerTarget, Staleness, Precision, Cadence, Jitter, Hysteresis,
        AllinLead, SoldierRatio,
        // Per-tier PRODUCTION VOLUME. These were deliberately shared by all tiers ("expansion is
        // action quality, and every tier has identical actions"), but 16 recorded beginner losses
        // showed the shared values were the dominant term in how brutal easy feels: it out-economised
        // first-timers ~3x and converted the bank into 90-220 soldiers. Volume is now per-tier so
        // easy's ramp can be paced to a real beginner's.
        QueueDepth, MaxBuildings, DefenceFloor, BuildCluster;
};
AiKnobs KnobsFor(const CvSnapshot& Cv, EAiTier Tier);

class AiController {
public:
    // Team = the side the AI plays (fills that team's mask). Seed derives the AI's RNG stream
    // (distinct from the sim's), so jitter is reproducible within a replay but varies per match
    // when the caller salts the seed.
    void Init(uint64_t Seed, uint8_t Team, EAiTier Tier);

    // One tick's input events for the AI's team, written into Out (capacity Cap; Count set on
    // return). Pure function of (S, Tick) + the controller's seeded RNG + S.Cv; call once per
    // tick on the sim thread (the InputFn seam).
    void DecideEvents(const Sim& S, uint32_t Tick, InputEvent* Out, int Cap, int& Count);

    EAiTier Tier() const { return Tier_; }

    enum class EState : uint8_t { Opening, Building, Reacting, AllIn };
    // Read-only windows into the decision, for the match recorder / telemetry (#144). Without these
    // a recording shows WHAT the AI built but not WHY — and "hard mis-counters" vs "hard is
    // production-bound" are different bugs with different fixes.
    EState  State() const { return State_; }
    uint8_t CounterEnemy() const { return CounterEnemy_; }   // enemy type being countered, or UnitNone

private:

    // Delayed/fuzzed enemy-composition mirror: a ring of past TRUE soldier counts (per type),
    // recorded every tick; on a reaction tick the AI reads the entry from now-staleness and
    // quantizes it to the tier precision. This delay+fuzz IS the only difference between tiers.
    static constexpr int32_t RingSize = 256;   // >= max staleness (+ margin)
    int16_t Ring_[RingSize][3] = {};           // [tick % RingSize][rock,paper,scissor]

    uint8_t             MyTeam_ = 1;
    uint8_t             FoeTeam_ = 0;
    EAiTier             Tier_ = EAiTier::Medium;
    Lur::Sim::SplitMix64 Rng_{0};
    uint32_t            NextReactTick_ = 0;    // cadence gate for the enemy-read re-decision
    uint8_t             CounterEnemy_ = UnitNone;  // enemy type we're currently countering (hysteresis)
    EState              State_ = EState::Opening;
    // BUILD CLUSTER: a standing intent to add several buildings of ONE type in quick succession,
    // one per tick — never several in a tick, which would be an action rate no human could match.
    // Production is flat per building, so ramping a counter means ramping BUILDINGS, and adding them
    // one at a time interleaved with queueing made that ramp glacial.
    uint8_t             ClusterType_ = UnitNone;
    int32_t             ClusterLeft_ = 0;
    uint32_t            ClusterUntil_ = 0;      // tick deadline, so a cluster it cannot fund is dropped
    // High-water mark of the economy, so losses can be told apart from never having built it.
    // "Restore what you HAD" is a floor the AI can always meet; "reach the target" is not — as an
    // unconditional floor the target would refuse to make a single soldier against an early rush.
    int32_t             PeakMiners_ = 0;
};

}  // namespace Rps
