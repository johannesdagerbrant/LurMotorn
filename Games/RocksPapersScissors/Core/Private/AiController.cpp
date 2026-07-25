#include "Rps/AiController.h"

namespace Rps {

AiKnobs KnobsFor(const CvSnapshot& Cv, EAiTier Tier) {
    switch (Tier) {
        case EAiTier::Easy:
            return {Cv.AiEasyOpenWorkers, Cv.AiEasyWorkerTarget, Cv.AiEasyStaleness,
                    Cv.AiEasyPrecision, Cv.AiEasyCadence, Cv.AiEasyJitter, Cv.AiEasyHysteresis,
                    Cv.AiEasyAllinLead, Cv.AiEasySoldierRatio};
        case EAiTier::Hard:
            return {Cv.AiHardOpenWorkers, Cv.AiHardWorkerTarget, Cv.AiHardStaleness,
                    Cv.AiHardPrecision, Cv.AiHardCadence, Cv.AiHardJitter, Cv.AiHardHysteresis,
                    Cv.AiHardAllinLead, Cv.AiHardSoldierRatio};
        case EAiTier::Medium:
        default:
            return {Cv.AiMediumOpenWorkers, Cv.AiMediumWorkerTarget, Cv.AiMediumStaleness,
                    Cv.AiMediumPrecision, Cv.AiMediumCadence, Cv.AiMediumJitter,
                    Cv.AiMediumHysteresis, Cv.AiMediumAllinLead, Cv.AiMediumSoldierRatio};
    }
}

namespace {
// The type that BEATS enemy type T (its counter): Paper>Rock, Scissor>Paper, Rock>Scissor.
// Derived from the same UnitStats::Beats relation the sim uses, so the AI's counter can never
// disagree with combat. Returns UnitNone for a non-soldier (miner / none).
uint8_t CounterTo(uint8_t Enemy) {
    for (uint8_t X = UnitRock; X <= UnitScissor; ++X)
        if (UnitTable[X].Beats == Enemy) return X;
    return UnitNone;
}
// Round a count to the tier's precision bucket (1 = exact); fuzzes the AI's enemy read.
int32_t Quantize(int32_t C, int32_t Bucket) {
    if (Bucket <= 1) return C;
    return ((C + Bucket / 2) / Bucket) * Bucket;
}
// Survey (Team, Type)'s production capacity: how many such buildings are alive, and which of them
// has the SHALLOWEST queue (ties -> lowest slot, so it stays deterministic).
//
// #144: "shallowest", not "first". Production is flat per building since #132, so N buildings of a
// type produce N units per BuildTicks — but only if the work is SPREAD over them. The old
// first-match lookup piled everything onto one building and left the rest idle, which made extra
// capacity worthless even once the AI could afford it.
struct TypeCapacity {
    int32_t Owned = 0;      // alive buildings of this type
    int32_t Slot = -1;      // the one with the shallowest queue
    int32_t Queue = 0;      //   ...and its depth
};
TypeCapacity SurveyType(const Sim& S, uint8_t Team, uint8_t Type) {
    TypeCapacity C;
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || !S.IsBuilding(I) || S.Team[I] != Team || S.Type[I] != Type) continue;
        if (S.IsHomeBase(I)) continue;          // #146: the HQ produces nothing
        ++C.Owned;
        if (C.Slot < 0 || S.Queue[I] < C.Queue) { C.Slot = I; C.Queue = S.Queue[I]; }
    }
    return C;
}
// First valid placement spot for the AI in its own band — a deterministic sweep of candidate
// cells, first one CanPlaceBuilding accepts (avoids overlaps/mines/frontier). Buildings
// accumulate, so successive placements naturally step to the next free cell.
// #144: the row sweep runs deeper than the opening frontier reaches (rows are 4 units apart, so 14
// rows = 52 units vs the 35-unit initial frontier). Forward rows are simply refused by
// CanPlaceBuilding until the AI's own units have walked far enough to advance its frontier
// high-water — so the AI grows into the ground it earns instead of being boxed in by a sweep that
// stopped short of its own buildable depth.
bool AiPlaceSpot(const Sim& S, uint8_t Team, uint8_t Type, Fixed& OX, Fixed& OY) {
    const int32_t Base = Team == 0 ? 5 : (WorldHeight.ToInt() - 5);
    const int32_t Dir = Team == 0 ? 1 : -1;
    const int32_t Xs[4] = {8, 14, 20, 26};
    for (int32_t R = 0; R < 14; ++R)
        for (int32_t Xi = 0; Xi < 4; ++Xi) {
            const Fixed X = F(Xs[Xi]);
            const Fixed Y = F(Base + Dir * R * 4);
            if (S.CanPlaceBuilding(Team, Type, X, Y)) { OX = X; OY = Y; return true; }
        }
    return false;
}
}  // namespace

void AiController::Init(uint64_t Seed, uint8_t Team, EAiTier Tier) {
    MyTeam_ = Team;
    FoeTeam_ = static_cast<uint8_t>(1 - Team);
    Tier_ = Tier;
    // Distinct RNG stream from the sim's; salted by team so two AIs (AI-vs-AI) jitter apart.
    Rng_ = Lur::Sim::SplitMix64(Seed ^ 0xA1C0DEull ^ (static_cast<uint64_t>(Team) + 1) * 0x9E3779B97F4A7C15ull);
    NextReactTick_ = 0;
    CounterEnemy_ = UnitNone;
    State_ = EState::Opening;
    for (int32_t I = 0; I < RingSize; ++I) Ring_[I][0] = Ring_[I][1] = Ring_[I][2] = 0;
}

void AiController::DecideEvents(const Sim& S, uint32_t Tick, InputEvent* Out, int Cap, int& Count) {
    Count = 0;
    const AiKnobs K = KnobsFor(S.Cv, Tier_);

    // --- Scan the board once: my economy/army + the TRUE enemy soldier composition. ---
    int32_t MyMiners = 0, MySoldiers = 0;
    int32_t TrueEnemy[3] = {0, 0, 0};  // rock, paper, scissor
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        const uint8_t Ty = S.Type[I];
        if (S.Team[I] == MyTeam_) {
            if (Ty == UnitMiner) ++MyMiners; else ++MySoldiers;
        } else if (Ty >= UnitRock && Ty <= UnitScissor) {
            ++TrueEnemy[Ty - UnitRock];
        }
    }
    // Record the true composition into the ring EVERY tick, so the delayed read is available.
    const int32_t Slot = static_cast<int32_t>(Tick % RingSize);
    Ring_[Slot][0] = static_cast<int16_t>(TrueEnemy[0]);
    Ring_[Slot][1] = static_cast<int16_t>(TrueEnemy[1]);
    Ring_[Slot][2] = static_cast<int16_t>(TrueEnemy[2]);

    // --- Reaction cadence: re-read the (stale, fuzzed) enemy mirror only every Cadence ticks
    // (+/- seeded jitter), and update which enemy type we're countering (with hysteresis). The
    // opening runs at full speed for all tiers (spec §9 Q2): it's a fixed script, not a react. ---
    if (Tick >= NextReactTick_) {
        const int32_t Stale = K.Staleness < 0 ? 0 : (K.Staleness >= RingSize ? RingSize - 1 : K.Staleness);
        const int32_t RSlot = static_cast<int32_t>((Tick + RingSize - static_cast<uint32_t>(Stale)) % RingSize);
        const int32_t Bucket = K.Precision;
        int32_t Seen[3] = {Quantize(Ring_[RSlot][0], Bucket), Quantize(Ring_[RSlot][1], Bucket),
                           Quantize(Ring_[RSlot][2], Bucket)};
        // Dominant enemy soldier type (ties resolve rock < paper < scissor — deterministic).
        int32_t Dom = 0;
        for (int32_t T = 1; T < 3; ++T)
            if (Seen[T] > Seen[Dom]) Dom = T;
        if (Seen[Dom] > 0) {
            const uint8_t DomType = static_cast<uint8_t>(UnitRock + Dom);
            // Hysteresis: only switch the countered enemy type when the new dominant leads the
            // one we currently counter by the margin (prevents twitchy flip-flop). First lock-on
            // (CounterEnemy_ == none) always takes.
            if (CounterEnemy_ == UnitNone) {
                CounterEnemy_ = DomType;
            } else if (DomType != CounterEnemy_) {
                const int32_t CurCount = Seen[CounterEnemy_ - UnitRock];
                if (Seen[Dom] >= CurCount + K.Hysteresis) CounterEnemy_ = DomType;
            }
        }
        // Schedule the next reaction with +/- jitter (seeded, deterministic).
        int32_t Delay = K.Cadence;
        if (K.Jitter > 0) Delay += static_cast<int32_t>(Rng_.NextBounded(static_cast<uint32_t>(2 * K.Jitter + 1))) - K.Jitter;
        if (Delay < 1) Delay = 1;
        NextReactTick_ = Tick + static_cast<uint32_t>(Delay);
    }

    // --- FSM state from live counts + the (held) enemy read. ---
    int32_t EnemyArmy = 0;
    for (int32_t T = 0; T < 3; ++T) EnemyArmy += TrueEnemy[T];  // pressure signal (own-side, not fuzzed)
    if (MyMiners < K.OpenWorkers) {
        State_ = EState::Opening;
    } else if (MySoldiers - EnemyArmy >= K.AllinLead && MySoldiers > 0) {
        State_ = EState::AllIn;              // clearly ahead -> commit everything to soldiers
    } else if (EnemyArmy > 0) {
        State_ = EState::Reacting;           // contested -> army-biased, keep some economy
    } else {
        State_ = EState::Building;           // safe -> grow economy, trickle soldiers
    }

    // The soldier type to build: the counter to what we're tracking, or Rock until we've seen
    // an enemy (a neutral opener — it beats scissor, loses to paper, a coin-flip default).
    const uint8_t Soldier =
        CounterEnemy_ != UnitNone ? CounterTo(CounterEnemy_) : static_cast<uint8_t>(UnitRock);

    // --- Pick ONE type to press this tick per state (continuous production, gold-gated). ---
    uint8_t Want = UnitMiner;
    switch (State_) {
        case EState::Opening: Want = UnitMiner; break;
        case EState::AllIn:   Want = Soldier; break;
        case EState::Building:
            Want = (MyMiners < K.WorkerTarget) ? static_cast<uint8_t>(UnitMiner) : Soldier;
            break;
        case EState::Reacting: {
            // Hold a soldier:worker ratio (percent of army that should be soldiers), but never
            // starve the opening economy.
            const int32_t Total = MySoldiers + MyMiners + 1;
            const bool WantSoldier = MySoldiers * 100 < K.SoldierRatio * Total;
            Want = (MyMiners < K.OpenWorkers)  ? static_cast<uint8_t>(UnitMiner)
                   : WantSoldier               ? Soldier
                                               : static_cast<uint8_t>(UnitMiner);
            break;
        }
    }

    // --- Translate the desired unit type into building EVENTS (#137). ---
    if (Cap < 1) return;
    // 1. The forced first building is a mining camp: until one exists, that's the only action.
    if (SurveyType(S, MyTeam_, UnitMiner).Owned == 0) {
        Fixed X, Y;
        if (AiPlaceSpot(S, MyTeam_, UnitMiner, X, Y))
            Out[Count++] = InputEvent::Place(MyTeam_, UnitMiner, X, Y);
        return;
    }
    // 2. Produce Want — spreading the work, and BUYING CAPACITY when it runs out (#144).
    //
    // Two rules, one event per tick (so a rich AI still spends over several ticks rather than in a
    // burst, and gold/validity stay the sim's to enforce — ApplyPlace/ApplyQueue are deterministic
    // no-ops when refused):
    //
    //   EXPAND when every building of this type is already carrying QueueDepth work AND gold is at
    //   ExpandGoldFactor% of another building's price. Both halves matter: saturation means capacity
    //   (not money) is the binding constraint, and the gold margin keeps a reserve for units so the
    //   AI can't building-sprawl itself out of an army. This is what was missing — the old code
    //   placed a building of a type only when it owned NONE, capping throughput at four buildings
    //   while income compounded past it forever.
    //
    //   Otherwise QUEUE at the shallowest building of the type, so N buildings do N units per
    //   BuildTicks instead of one building doing all the work with the others idle.
    const TypeCapacity Cap_ = SurveyType(S, MyTeam_, Want);
    const int32_t Price = BuildingCostFor(S.Cv, Want);
    const int32_t Gold = S.Teams[MyTeam_].Gold;
    const int32_t Depth = S.Cv.AiQueueDepth > 0 ? S.Cv.AiQueueDepth : 1;
    const int32_t Factor = S.Cv.AiExpandGoldFactor > 100 ? S.Cv.AiExpandGoldFactor : 100;
    const bool Saturated = Cap_.Owned > 0 && Cap_.Queue >= Depth;
    const bool CanAffordAnother = Gold >= Price * Factor / 100;   // Price <= a few thousand: no overflow
    if ((Cap_.Owned == 0 && Gold >= Price) || (Saturated && CanAffordAnother)) {
        Fixed X, Y;
        if (AiPlaceSpot(S, MyTeam_, Want, X, Y))
            Out[Count++] = InputEvent::Place(MyTeam_, Want, X, Y);
    } else if (Cap_.Slot >= 0 && Cap_.Queue < Depth) {
        Out[Count++] = InputEvent::Queue(MyTeam_, Cap_.Slot, 1);
    }
}

}  // namespace Rps
