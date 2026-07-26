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

// ---- #144 slice 3: SPATIAL placement (position is a decision, not an afterthought) ----
// A recorded human win (2026-07-25) was won on geography: they planted camps ON distant mine
// clusters and put soldier buildings AT the front line, while the AI stacked every building on its
// own baseline in the AiPlaceSpot grid above. Two consequences, both large:
//   * Carts deposit at the NEAREST OWN miner building (Sim §12.4), so a camp beside a mine is a
//     short round trip and a camp at home is a long one. Camp position IS income.
//   * A soldier walks from where it spawns, so home-built soldiers arrive at the fight late and in
//     a trickle, and counters that arrive late don't counter anything.
//
// Both fixed by placing NEAR A TARGET instead of at the first free grid cell: a deterministic ring
// search outward from the target, taking the first spot the sim accepts. Rings are integer offsets
// in world units, ordered nearest-first; everything stays Fixed/integer so placement remains
// hash-safe and identical on both peers.
bool AiPlaceNear(const Sim& S, uint8_t Team, uint8_t Type, Fixed TargetX, Fixed TargetY, Fixed& OX,
                 Fixed& OY) {
    // Offsets ordered by increasing distance. Coarse steps of 3 were fine while the mine clearance
    // equalled the footprint (3), but they put a CLIFF in the clearance knob (#157): the nearest
    // offsets are 6 then 9, so a clearance anywhere in (6, 9] made the AI skip from 6 straight to 9
    // and every cart trip got 50% longer. Measured: clearance 6 -> hard beats easy 8-0; clearance
    // 6.5 -> hard LOSES 0-8, purely from that quantisation, not from the map.
    //
    // So the rings are finer where it matters — 7, 8 and the 5/6 diagonals now exist, letting the AI
    // sit JUST outside whatever the clearance is instead of overshooting to the next multiple of 3.
    // Still a bounded hand-ordered table (~45 entries, integer, nearest-first) rather than a search:
    // this runs on the expand branch and each candidate costs a CanPlaceBuilding scan.
    static const int32_t Dx[] = {
        0,                                   // 0
        3, -3,  0,  0,   3, -3,  3, -3,      // 3, 4.24
        6, -6,  0,  0,                       // 6
        0,  0,  7, -7,   5, -5,  5, -5,      // 7, 7.07
        0,  0,  8, -8,   6, -6,  6, -6,      // 8, 8.49
        0,  0,  9, -9,                       // 9
        0,  0, 12, -12,  9, -9,  9, -9,      // 12, 12.73
       12, -12, 12, -12};                    // 16.97
    static const int32_t Dy[] = {
        0,
        0,  0,  3, -3,   3,  3, -3, -3,
        0,  0,  6, -6,
        7, -7,  0,  0,   5,  5, -5, -5,
        8, -8,  0,  0,   6,  6, -6, -6,
        9, -9,  0,  0,
       12, -12, 0,  0,   9,  9, -9, -9,
       12, -12, -12, 12};
    static_assert(sizeof(Dx) == sizeof(Dy), "AiPlaceNear ring: Dx/Dy must pair up");
    constexpr int32_t Ring = static_cast<int32_t>(sizeof(Dx) / sizeof(Dx[0]));
    const int32_t Tx = TargetX.ToInt(), Ty = TargetY.ToInt();
    for (int32_t I = 0; I < Ring; ++I) {
        const int32_t X = Tx + Dx[I], Y = Ty + Dy[I];
        if (X < 2 || X > WorldWidth.ToInt() - 2 || Y < 2 || Y > WorldHeight.ToInt() - 2) continue;
        if (S.CanPlaceBuilding(Team, Type, F(X), F(Y))) { OX = F(X); OY = F(Y); return true; }
    }
    return false;
}

// The mine worth expanding to: a live deposit that is NOT already served by one of our camps, lies
// inside our buildable depth, and is the FURTHEST FORWARD such deposit — so the economy creeps
// toward the enemy as the frontier earns ground, instead of re-mining the home cluster.
//
// The "inside our frontier" filter is not an optimisation, it is the whole difference between this
// working and not: every mine starts with identical gold, so a richest-first search just took the
// lowest INDEX, which is often a deposit on the enemy's half — permanently unbuildable, so the AI
// proposed an illegal spot every time and never expanded its economy at all.
bool AiBestMineTarget(const Sim& S, uint8_t Team, Fixed& OX, Fixed& OY) {
    constexpr int32_t ServedRadius = 18;   // world units; a cluster's own spread is smaller than this
    const Fixed Limit = Team == 0 ? S.FrontierT0 : S.FrontierT1;
    int32_t Best = -1;
    Fixed BestY{0};
    for (int32_t M = 0; M < NumMines; ++M) {
        if (S.MineGold[M] <= 0) continue;
        // Must be inside our own buildable depth, or the placement can only ever be refused.
        if (Team == 0 ? S.MineY[M] > Limit : S.MineY[M] < Limit) continue;
        bool Served = false;
        for (int32_t I = 0; I < S.Count && !Served; ++I) {
            if (!S.IsAlive(I) || !S.IsBuilding(I) || S.Team[I] != Team) continue;
            if (S.Type[I] != UnitMiner || S.IsHomeBase(I)) continue;
            const int32_t Ddx = (S.PosX[I] - S.MineX[M]).ToInt();
            const int32_t Ddy = (S.PosY[I] - S.MineY[M]).ToInt();
            const int32_t Adx = Ddx < 0 ? -Ddx : Ddx, Ady = Ddy < 0 ? -Ddy : Ddy;
            if (Adx <= ServedRadius && Ady <= ServedRadius) Served = true;   // Chebyshev, like the grid
        }
        if (Served) continue;
        // Furthest forward wins (team 0 = larger Y). First index wins a tie, so it stays deterministic.
        const bool Ahead = Best < 0 || (Team == 0 ? S.MineY[M] > BestY : S.MineY[M] < BestY);
        if (Ahead) { BestY = S.MineY[M]; Best = M; }
    }
    if (Best < 0) return false;
    OX = S.MineX[Best];
    OY = S.MineY[Best];
    return true;
}

// Where to build for the fight: JUST BEHIND our own leading edge, on the flank the enemy army is
// coming down. Not at the enemy's centroid — aiming there plants buildings inside their army and
// they are razed as fast as they go up (measured: the AI's building count fell over a match instead
// of rising). Our frontier is where our frontmost survivor stands, so a spot a short way behind it
// is close to the fighting AND covered by our own units — which is where the recorded human win put
// them (their soldier buildings sat 45-80 units short of the enemy base, never at it).
//
// The enemy centroid still chooses the X: same distance from the fight, but on the correct side of
// the map.
constexpr int32_t FrontSetback = 8;   // world units behind the leading edge
void AiFrontTarget(const Sim& S, uint8_t Team, Fixed& OX, Fixed& OY) {
    const uint8_t Foe = static_cast<uint8_t>(1 - Team);
    int64_t Sx = 0, Sy = 0;
    int32_t N = 0;
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || S.IsBuilding(I) || S.Team[I] != Foe) continue;
        if (S.Type[I] == UnitMiner) continue;          // aim at their ARMY, not their economy
        Sx += S.PosX[I].Raw;
        Sy += S.PosY[I].Raw;
        ++N;
    }
    const Fixed Frontier = Team == 0 ? S.FrontierT0 : S.FrontierT1;
    // A setback behind our leading edge, floored at the opening depth so the early game doesn't aim
    // behind the baseline.
    const Fixed Floor = Team == 0 ? S.Cv.InitialFrontier : WorldHeight - S.Cv.InitialFrontier;
    const Fixed Back = Team == 0 ? Frontier - F(FrontSetback) : Frontier + F(FrontSetback);
    OY = Team == 0 ? (Back > Floor ? Back : Floor) : (Back < Floor ? Back : Floor);
    OX = N == 0 ? Fixed{WorldWidth.Raw / 2} : Fixed{static_cast<int32_t>(Sx / N)};
    (void)Sy;   // the enemy's Y picks nothing: we build behind OUR line, not at theirs
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
        // The OPENING camp goes on a mine if one is reachable — carts deposit at the nearest own
        // camp, so a camp beside gold is a short round trip from the first tick. Falls back to the
        // home grid if no mine is placeable yet (frontier).
        Fixed X, Y, Mx, My;
        if (AiBestMineTarget(S, MyTeam_, Mx, My) && AiPlaceNear(S, MyTeam_, UnitMiner, Mx, My, X, Y))
            Out[Count++] = InputEvent::Place(MyTeam_, UnitMiner, X, Y);
        else if (AiPlaceSpot(S, MyTeam_, UnitMiner, X, Y))
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
    const bool CanAffordAnother = Gold >= Price * Factor / 100;   // Price <= a few thousand: no overflow
    // EXPAND on SURPLUS, not on saturation. The earlier saturation test (all buildings already
    // carrying Depth work) could never fire, because queueing drains faster than a
    // one-decision-per-tick AI refills — so it never concluded it needed capacity and sat on its
    // gold. Idle gold is the honest signal, and it is what a human acts on: a recorded human win had
    // 21 buildings to the AI's 8 (2026-07-25 flight recordings, #144).
    if (Cap_.Owned == 0 ? Gold >= Price : CanAffordAnother) {
        // WHERE matters as much as whether (#144 slice 3, from the recorded human win):
        //   * a mining camp goes ON the richest unworked mine — that is what makes cart trips short
        //     and captures the map's economy instead of re-mining the home cluster;
        //   * a soldier building goes AT THE FRONT, as close to the enemy army as our own frontier
        //     allows, so the counters it produces spawn where the fighting already is rather than
        //     walking the length of the map to arrive in a trickle.
        // Each falls back to the home grid if the target has no legal spot (frontier, overlap, mine).
        Fixed X, Y, Tx, Ty;
        bool Have = false;
        if (Want == UnitMiner) {
            if (AiBestMineTarget(S, MyTeam_, Tx, Ty))
                Have = AiPlaceNear(S, MyTeam_, Want, Tx, Ty, X, Y);
        } else {
            AiFrontTarget(S, MyTeam_, Tx, Ty);
            Have = AiPlaceNear(S, MyTeam_, Want, Tx, Ty, X, Y);
        }
        if (!Have) Have = AiPlaceSpot(S, MyTeam_, Want, X, Y);
        if (Have) {
            Out[Count++] = InputEvent::Place(MyTeam_, Want, X, Y);
            return;
        }
        // No legal spot anywhere — fall through and put the gold into units instead.
    }
    // QUEUE IN A BATCH — top the building up to Depth in ONE event, which is what the x1/x5 plate
    // does for a human. This was hardcoded to 1, and it was the single biggest gap in the recorded
    // matches: the human queued 956 units in 192 decisions (x5 on 191 of them) while the AI queued
    // 204 in 204 decisions. Worse than the ratio suggests, it left the AI's OWN buildings idle —
    // 8 buildings could produce ~5.3 units/s but it only ever asked for ~1.2/s.
    // Bounded by what it can pay for and by the building's queue cap; the sim re-checks both
    // deterministically, so an over-ask is a safe no-op rather than a cheat.
    if (Cap_.Slot >= 0 && Cap_.Queue < Depth) {
        const int32_t UnitCost = S.Units[Want].Cost > 0 ? S.Units[Want].Cost : 1;
        int32_t N = Depth - Cap_.Queue;
        const int32_t Room = S.Cv.BuildingQueueMax - Cap_.Queue;
        if (N > Room) N = Room;
        const int32_t Affordable = Gold / UnitCost;
        if (N > Affordable) N = Affordable;
        if (N > 0) Out[Count++] = InputEvent::Queue(MyTeam_, Cap_.Slot, N);
    }
}

}  // namespace Rps
