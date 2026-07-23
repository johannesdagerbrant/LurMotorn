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
// First alive building of (Team, Type), or -1.
int32_t FindBuilding(const Sim& S, uint8_t Team, uint8_t Type) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.IsBuilding(I) && S.Team[I] == Team && S.Type[I] == Type) return I;
    return -1;
}
// First valid placement spot for the AI in its own band — a deterministic sweep of candidate
// cells, first one CanPlaceBuilding accepts (avoids overlaps/mines/frontier). Buildings
// accumulate, so successive placements naturally step to the next free cell.
bool AiPlaceSpot(const Sim& S, uint8_t Team, uint8_t Type, Fixed& OX, Fixed& OY) {
    const int32_t Base = Team == 0 ? 5 : (WorldHeight.ToInt() - 5);
    const int32_t Dir = Team == 0 ? 1 : -1;
    const int32_t Xs[4] = {8, 14, 20, 26};
    for (int32_t R = 0; R < 8; ++R)
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
    const int32_t MinerB = FindBuilding(S, MyTeam_, UnitMiner);
    if (MinerB < 0) {
        Fixed X, Y;
        if (AiPlaceSpot(S, MyTeam_, UnitMiner, X, Y))
            Out[Count++] = InputEvent::Place(MyTeam_, UnitMiner, X, Y);
        return;
    }
    // 2. To produce Want, ensure a building of that type. If it exists, queue there (but don't
    //    over-stack — flat production drains slowly); else place one when affordable. Gold and
    //    validity are enforced deterministically by the sim (ApplyPlace/ApplyQueue).
    const int32_t WantB = FindBuilding(S, MyTeam_, Want);
    if (WantB >= 0) {
        if (S.Queue[WantB] < 3) Out[Count++] = InputEvent::Queue(MyTeam_, WantB, 1);
    } else if (S.Teams[MyTeam_].Gold >= BuildingCostFor(S.Cv, Want)) {
        Fixed X, Y;
        if (AiPlaceSpot(S, MyTeam_, Want, X, Y))
            Out[Count++] = InputEvent::Place(MyTeam_, Want, X, Y);
    }
}

}  // namespace Rps
