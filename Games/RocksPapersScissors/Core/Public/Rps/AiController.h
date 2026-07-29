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

// APPEND-ONLY: the value is stored in flight recordings (`tier <n>`) and in the per-tier score
// arrays, so renumbering an existing tier silently relabels history.
enum class EAiTier : uint8_t { Easy = 0, Medium = 1, Hard = 2, PerhapsImpossible = 3 };
// One source of truth for "how many tiers are there". Every per-tier array (score tallies, the
// opponent selector's rows, the harness name tables) sizes off this — the alternative is a literal
// 3 in a dozen places, and a new tier then works everywhere except the one place that was missed.
constexpr int AiTierCount = 4;
// Display names, indexed by EAiTier. The UI shows these verbatim.
inline const char* AiTierName(EAiTier T) {
    switch (T) {
        case EAiTier::Easy:              return "Easy";
        case EAiTier::Medium:            return "Medium";
        case EAiTier::Hard:              return "Hard";
        case EAiTier::PerhapsImpossible: return "Perhaps Impossible";
    }
    return "?";
}

// The nine per-tier knobs, resolved from the latched CvSnapshot for one tier.
struct AiKnobs {
    int32_t OpenWorkers, WorkerTarget, Staleness, Precision, Cadence, Jitter, Hysteresis,
        AllinLead, SoldierRatio,
        // Per-tier PRODUCTION VOLUME. These were deliberately shared by all tiers ("expansion is
        // action quality, and every tier has identical actions"), but 16 recorded beginner losses
        // showed the shared values were the dominant term in how brutal easy feels: it out-economised
        // first-timers ~3x and converted the bank into 90-220 soldiers. Volume is now per-tier so
        // easy's ramp can be paced to a real beginner's.
        QueueDepth, MaxBuildings, DefenceFloor, BuildCluster,
        // Carts per batch. 0 = use the shared rps.ai.miner_queue_depth, which is what every
        // tier below the top does — their economies are tuned against that value.
        MinerQueue,
        // WAVE LEAD (the owner's own strategy, 2026-07-27): he keeps expanding mining right up
        // until the opponent's first wave is nearly at his camp, and only THEN commits a cluster of
        // counter buildings with their stacks maxed. Reacting at first SIGHTING instead — what every
        // tier did — spends the whole walk (~34s at the opening frontier) building soldiers it did
        // not need yet, out of income it could have compounded. Ticks of lead before arrival;
        // 0 keeps the old sighting behaviour, which is what the measured lower tiers are tuned on.
        WaveLead,
        // COUNTER CHEST (owner's suggestion, 2026-07-27): percent of a counter BUILDING's price to
        // keep banked while contested, so a composition switch is answered by PLACING at the front
        // rather than by waiting out income. Every coin queued as units is a coin that cannot buy
        // the building the next wave needs. 0 = spend it all, which is what the measured lower tiers
        // do today.
        CounterChest;
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
    // Target SHARE of the army for soldier type (UnitRock + I), in permille; 0 while the mix is off
    // (every tier but the top one, or rps.ai.mix_enable 0). Exposed so a harness can assert the
    // realised composition against the intent — "which type is it short of" is the diagnosis, and an
    // end-state tally cannot give it.
    int32_t MixShare(int32_t I) const { return I >= 0 && I < 3 ? MixShare_[I] : 0; }

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
    // TARGET MIX (#158): the top tier's desired share of each soldier type, in permille, latched on
    // a reaction tick like the enemy read it derives from. It is a DISTRIBUTION and not a choice —
    // the AI produces whichever type is proportionally furthest behind its share, so the army
    // composition IS the mixed strategy and no RNG is involved. All zero = the mix is off, in which
    // case the single-type argmax path below runs unchanged (every tier but the top one).
    int32_t             MixShare_[3] = {0, 0, 0};
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
