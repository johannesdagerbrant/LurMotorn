#pragma once
// Rps::AiController — the single-player opponent (design: Docs/Journal/2026-07-22/
// rps-ai-opponent-spec.md, epic #120). Its SOLE interface to the game is the 4-bit input
// mask a human has (miner + three unit types): DecideMask(state, tick) reads the (const) sim
// and returns the press for its team this tick. It never touches sim state — so it is
// structurally fair (can't do anything a player can't) and deterministic (integer/Fixed only,
// seeded SplitMix64 on its own stream, ticks not wall-clock), which keeps single-player inside
// the same replay/rollback model as a networked match.
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
        AllinLead, SoldierRatio;
};
AiKnobs KnobsFor(const CvSnapshot& Cv, EAiTier Tier);

class AiController {
public:
    // Team = the side the AI plays (fills that team's mask). Seed derives the AI's RNG stream
    // (distinct from the sim's), so jitter is reproducible within a replay but varies per match
    // when the caller salts the seed.
    void Init(uint64_t Seed, uint8_t Team, EAiTier Tier);

    // One tick's press for the AI's team. Pure function of (S, Tick) + the controller's seeded
    // RNG + S.Cv; call once per tick on the sim thread (the InputFn seam).
    uint8_t DecideMask(const Sim& S, uint32_t Tick);

    EAiTier Tier() const { return Tier_; }

private:
    enum class EState : uint8_t { Opening, Building, Reacting, AllIn };

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
};

}  // namespace Rps
