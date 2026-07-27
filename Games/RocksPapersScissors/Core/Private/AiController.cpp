#include "Rps/AiController.h"

namespace Rps {

AiKnobs KnobsFor(const CvSnapshot& Cv, EAiTier Tier) {
    switch (Tier) {
        case EAiTier::Easy:
            return {Cv.AiEasyOpenWorkers, Cv.AiEasyWorkerTarget, Cv.AiEasyStaleness,
                    Cv.AiEasyPrecision, Cv.AiEasyCadence, Cv.AiEasyJitter, Cv.AiEasyHysteresis,
                    Cv.AiEasyAllinLead, Cv.AiEasySoldierRatio,
                    Cv.AiEasyQueueDepth, Cv.AiEasyMaxBuildings, Cv.AiEasyDefenceFloor,
                    Cv.AiEasyBuildCluster};
        case EAiTier::Hard:
            return {Cv.AiHardOpenWorkers, Cv.AiHardWorkerTarget, Cv.AiHardStaleness,
                    Cv.AiHardPrecision, Cv.AiHardCadence, Cv.AiHardJitter, Cv.AiHardHysteresis,
                    Cv.AiHardAllinLead, Cv.AiHardSoldierRatio,
                    Cv.AiHardQueueDepth, Cv.AiHardMaxBuildings, Cv.AiHardDefenceFloor,
                    Cv.AiHardBuildCluster};
        case EAiTier::Medium:
        default:
            return {Cv.AiMediumOpenWorkers, Cv.AiMediumWorkerTarget, Cv.AiMediumStaleness,
                    Cv.AiMediumPrecision, Cv.AiMediumCadence, Cv.AiMediumJitter,
                    Cv.AiMediumHysteresis, Cv.AiMediumAllinLead, Cv.AiMediumSoldierRatio,
                    Cv.AiMediumQueueDepth, Cv.AiMediumMaxBuildings, Cv.AiMediumDefenceFloor,
                    Cv.AiMediumBuildCluster};
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
    // Two passes, and the split matters.
    //
    // PASS 1 is the historic layout, but DERIVED rather than hardcoded: at the default footprint 3 it
    // reproduces the old table exactly (X = 8/14/20/26, rows every 4), so the balance measured against
    // that layout is preserved, while a tuned footprint now scales it instead of breaking it. Deriving
    // to different numbers is not free — a uniform fine grid changed where every fallback building
    // goes and cost hard 11 of 16 against medium.
    //
    // PASS 2 is a fine sweep that only runs when pass 1 finds nothing. That is what rescues a tight
    // map: at footprint 6 the coarse rows step 13 and jump clean over the only legal band (a brute
    // force finds 165 legal spots around y=15), so the AI placed NOTHING — no camp, no economy. The
    // fine pass costs nothing in the normal case because pass 1 almost always hits first.
    const int32_t Fp = S.Cv.BuildingFootprint.ToInt();
    const int32_t Edge = (3 * Fp + 1) / 2;      // == CanPlaceBuilding's 1.5*Fp margin, rounded up
    const int32_t Dir = Team == 0 ? 1 : -1;
    const int32_t MaxX = WorldWidth.ToInt() - Edge;
    for (int32_t Pass = 0; Pass < 2; ++Pass) {
        const int32_t XStart = Pass == 0 ? 2 * Fp + 2 : Edge;
        const int32_t XStep = Pass == 0 ? 2 * Fp : 2;
        const int32_t RowStep = Pass == 0 ? Fp + 1 : 2;
        const int32_t Base = Team == 0 ? (Pass == 0 ? Fp + 2 : Edge)
                                       : WorldHeight.ToInt() - (Pass == 0 ? Fp + 2 : Edge);
        // Pass 1 stays BOUNDED to just past the opening frontier, as the original was: sweeping the
        // full map instead let it fall back to spots far up the field it never used to consider, and
        // that alone swung hard from 23/24 to 8/16 against medium. Derived from InitialFrontier so it
        // still scales, and equals the old 14 rows at the default 35. Pass 2 is unbounded because by
        // then nothing legal was found anywhere nearer.
        const int32_t Rows = Pass == 0 ? (S.Cv.InitialFrontier.ToInt() + 21) / RowStep
                                       : WorldHeight.ToInt() / RowStep + 2;
        for (int32_t R = 0; R < Rows; ++R) {
            const int32_t Y = Base + Dir * R * RowStep;
            if (Y < 0 || Y > WorldHeight.ToInt()) break;
            for (int32_t X = XStart; X <= MaxX; X += XStep)
                if (S.CanPlaceBuilding(Team, Type, F(X), F(Y))) { OX = F(X); OY = F(Y); return true; }
        }
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
    // ORDER IS BALANCE. This search picks where every building goes, so changing the ORDER in which
    // candidates are tried changes the AI's whole layout and therefore its strength — replacing this
    // table with a uniformly generated ring was arithmetically fine and cost hard 12 of 16 against
    // medium. So the measured table stays exactly as it is, and robustness is added AFTER it.
    //
    // Table first (unchanged, nearest-first, tuned): fine where it matters — 7, 8 and the 5/6
    // diagonals exist so the AI can sit JUST outside the default clearance instead of overshooting to
    // the next multiple of 3 (that quantisation cliff cost hard 8-0 -> 0-8 between clearance 6 and 6.5).
    static const int32_t Dx[] = {
        0,
        3, -3,  0,  0,   3, -3,  3, -3,
        6, -6,  0,  0,
        0,  0,  7, -7,   5, -5,  5, -5,
        0,  0,  8, -8,   6, -6,  6, -6,
        0,  0,  9, -9,
        0,  0, 12, -12,  9, -9,  9, -9,
       12, -12, 12, -12};
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
    const int32_t Lo = 2, HiX = WorldWidth.ToInt() - 2, HiY = WorldHeight.ToInt() - 2;
    auto Try = [&](int32_t X, int32_t Y) {
        if (X < Lo || X > HiX || Y < Lo || Y > HiY) return false;
        if (!S.CanPlaceBuilding(Team, Type, F(X), F(Y))) return false;
        OX = F(X); OY = F(Y); return true;
    };
    for (int32_t I = 0; I < Ring; ++I)
        if (Try(Tx + Dx[I], Ty + Dy[I])) return true;

    // Then, ONLY if the table found nothing: keep sweeping outward to whatever the live clearance and
    // footprint actually demand. The table stops near 17 units, so tuning rps.build.mine_clearance
    // above that used to make every candidate illegal and the AI simply stopped expanding — a knob
    // able to switch the AI off. This never runs at default tuning, so it cannot move the balance.
    const int32_t Clear = S.Cv.MineClearance.ToInt();
    const int32_t TwoFp = 2 * S.Cv.BuildingFootprint.ToInt();
    const int32_t MaxR = (Clear > TwoFp ? Clear : TwoFp) + 12;
    static const int32_t Ux[8] = {7, 0, -7, 0, 5, -5, 5, -5};
    static const int32_t Uy[8] = {0, 7, 0, -7, 5, 5, -5, -5};
    for (int32_t R = 14; R <= MaxR; R += 2)
        for (int32_t D = 0; D < 8; ++D)
            if (Try(Tx + R * Ux[D] / 7, Ty + R * Uy[D] / 7)) return true;
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
    const int32_t ServedRadius = S.Cv.AiMineServedRadius.ToInt();   // see rps.ai.mine_served_radius
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
// Setback is Cv.AiFrontSetback (rps.ai.front_setback) — Tunables.h explains why it moved forward.
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
    const Fixed Sb = S.Cv.AiFrontSetback;
    const Fixed Back = Team == 0 ? Frontier - Sb : Frontier + Sb;
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
    ClusterType_ = UnitNone;
    ClusterLeft_ = 0;
    ClusterUntil_ = 0;
    CounterEnemy_ = UnitNone;
    State_ = EState::Opening;
    PeakMiners_ = 0;
    for (int32_t I = 0; I < RingSize; ++I) Ring_[I][0] = Ring_[I][1] = Ring_[I][2] = 0;
}

void AiController::DecideEvents(const Sim& S, uint32_t Tick, InputEvent* Out, int Cap, int& Count) {
    Count = 0;
    const AiKnobs K = KnobsFor(S.Cv, Tier_);

    // --- Scan the board once: my economy/army + the TRUE enemy soldier composition. ---
    int32_t MyMiners = 0, MySoldiers = 0, MyCombatBldg = 0, FoeCombatBldg = 0;
    int32_t TrueEnemy[3] = {0, 0, 0};  // rock, paper, scissor
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        const uint8_t Ty = S.Type[I];
        // Enemy SOLDIER-PRODUCING buildings: the replacement rate an attack has to out-kill (see
        // the all-in gate). Counted exactly, not through the staleness/precision mirror, because a
        // building is static and huge — a human sees the enemy's production the moment they look at
        // it, and the fuzzed mirror exists to model reading a MOVING army.
        if (S.Team[I] != MyTeam_ && S.IsBuilding(I) && !S.IsHomeBase(I) &&
            Ty >= UnitRock && Ty <= UnitScissor)
            ++FoeCombatBldg;
        if (S.Team[I] == MyTeam_) {
            // Combat CAPACITY, counted separately and correctly. (The MyMiners/MySoldiers tallies
            // just below are deliberately left alone: they lump buildings in with units and the HQ
            // in with soldiers, and every tier's ratio knob was measured against that quirk — fixing
            // it here would silently re-balance all three tiers.)
            if (S.IsBuilding(I) && !S.IsHomeBase(I) && Ty >= UnitRock && Ty <= UnitScissor)
                ++MyCombatBldg;
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
                if (Seen[Dom] >= CurCount + K.Hysteresis) {
                    CounterEnemy_ = DomType;
                    // RE-TARGET a live cluster onto the new counter. Parallel production is only
                    // valuable if it is parallel production of the RIGHT thing — finishing a cluster
                    // of the old counter after the enemy composition moved is the building-scale
                    // version of "a deep queue of the wrong type is dead gold", and far more
                    // expensive, because a building is worth many units.
                    if (ClusterLeft_ > 0) ClusterType_ = CounterTo(CounterEnemy_);
                }
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
    // The soldier type to build: the counter to what we're tracking, or Rock until we've seen an
    // enemy (a neutral opener — it beats scissor, loses to paper, a coin-flip default). Resolved
    // BEFORE the FSM now, because the all-in gate below needs its speed and build time.
    const uint8_t Soldier =
        CounterEnemy_ != UnitNone ? CounterTo(CounterEnemy_) : static_cast<uint8_t>(UnitRock);

    if (MyMiners > PeakMiners_) PeakMiners_ = MyMiners;   // high-water mark (see the econ floor)
    // ALL-IN is gated on the enemy's REPLACEMENT RATE, not on an absolute unit lead. An attack has
    // to walk the gap between the two frontiers, and production is FLAT PER BUILDING (#132), so
    // while it walks, the defender's soldier buildings each add a unit every BuildTicks. A lead
    // smaller than that is spent before it lands. This is measured, not tuned: the 2026-07-27
    // recordings show hard committing at ~150s on a 26-unit lead over a player holding 11 soldiers
    // and 11.6 buildings — the player's own production alone answered it — and since AllIn produces
    // soldiers EXCLUSIVELY, the failed commitment also stopped hard's economy dead. Its workers then
    // fell 109 -> 75 and its army 61 -> 47 while the player tripled. It lost 14 of 19 that way.
    // AllinLead stays as the floor on top of that (a small edge is still not a commitment).
    const Fixed Gap = Abs(S.FrontierT1 - S.FrontierT0);          // the no-man's land to cross
    const Fixed Speed = S.Units[Soldier].Speed;
    const int32_t WalkTicks = Speed.Raw > 0 ? (Gap / Speed).ToInt() : 0;
    const int32_t Bt = S.Units[Soldier].BuildTicks > 0 ? S.Units[Soldier].BuildTicks : 1;
    const int32_t Incoming = FoeCombatBldg * WalkTicks / Bt;     // units that arrive during the walk
    if (MyMiners < K.OpenWorkers) {
        State_ = EState::Opening;
    } else if (MySoldiers - EnemyArmy >= K.AllinLead + Incoming && MySoldiers > 0) {
        State_ = EState::AllIn;              // can out-kill their production -> commit
    } else if (EnemyArmy > 0) {
        State_ = EState::Reacting;           // contested -> army-biased, keep some economy
    } else {
        State_ = EState::Building;           // safe -> grow economy, trickle soldiers
    }

    // --- Pick ONE type to press this tick per state (continuous production, gold-gated). ---
    // ECONOMY FLOOR, checked in the two states that otherwise bias soldiers unconditionally:
    // REPLACE WHAT WAS KILLED before going back to army. The recorded losses show why — the
    // player's raiders rank an enemy CART top priority (TargetPrefer), so hard's carts died faster
    // than a ratio rule would ever rebuy them: in Reacting the soldier bias is `soldiers < ratio %
    // of total`, and a DECAYING army never reaches the ratio, so it never asks for a miner again.
    // Economy decay then feeds army decay. Floored on the high-water mark (capped by the tier's own
    // target) so this only ever restores losses: flooring on WorkerTarget outright would refuse to
    // build a single soldier against an early rush, which is a much worse failure.
    const int32_t EconFloor = (PeakMiners_ < K.WorkerTarget ? PeakMiners_ : K.WorkerTarget) *
                              S.Cv.AiEconFloorPct / 100;
    uint8_t Want = UnitMiner;
    switch (State_) {
        case EState::Opening: Want = UnitMiner; break;
        case EState::AllIn:
            Want = MyMiners < EconFloor ? static_cast<uint8_t>(UnitMiner) : Soldier;
            break;
        case EState::Building:
            // DEFENCE FLOOR: stand up K.DefenceFloor combat buildings before chasing the economy
            // target. Without it, economy-first means a timely attack lands while there is not one
            // soldier building on the map, and the tier has to start production from scratch under
            // fire -- which is precisely how an uncapped worker_target lost to medium. It costs a
            // little tempo and buys the right to be greedy afterwards.
            // OPPORTUNISTIC, not blocking. Making the floor take priority outright deadlocked the
            // tier: from tick one it wanted a 1500 combat building while holding 600 and no miners,
            // so it built no economy, earned nothing, and never afforded the building it was waiting
            // for. Economy stays the default and a combat building is snapped up the moment one is
            // affordable, until the floor is met -- capacity gets bought out of surplus rather than
            // out of the income that pays for it.
            Want = (MyCombatBldg < K.DefenceFloor &&
                    S.Teams[MyTeam_].Gold >= BuildingCostFor(S.Cv, Soldier))
                       ? Soldier
                       : (MyMiners < K.WorkerTarget) ? static_cast<uint8_t>(UnitMiner) : Soldier;
            break;
        case EState::Reacting: {
            // Hold a soldier:worker ratio (percent of army that should be soldiers), but never
            // starve the opening economy.
            const int32_t Total = MySoldiers + MyMiners + 1;
            const bool WantSoldier = MySoldiers * 100 < K.SoldierRatio * Total;
            Want = (MyMiners < K.OpenWorkers)  ? static_cast<uint8_t>(UnitMiner)
                   : (MyMiners < EconFloor)    ? static_cast<uint8_t>(UnitMiner)  // restore losses
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
    int32_t MyBuildings = 0;
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && S.IsBuilding(J) && !S.IsHomeBase(J) && S.Team[J] == MyTeam_)
            ++MyBuildings;

    // 1b. BUILD CLUSTER (player strategy, adopted). A standing intent to finish several buildings of
    // ONE type before queueing anything at them — one placement per tick, never several, so the
    // action rate stays something a human could match.
    //
    // The point is NOT just a faster ramp. Adding a building, queueing at it, adding another, queueing
    // again feeds units out ONE AT A TIME into whatever is already standing there — small cannon fodder
    // walking into a big army, which is exactly the "hard's army declines late" symptom in the
    // recordings (113 -> 93 -> 70). Finishing the cluster first means the counter arrives as a WAVE.
    // While the intent is live the AI deliberately does not queue: the silence is the whole point.
    if (ClusterLeft_ > 0 && ClusterType_ != UnitNone) {
        if (Tick >= ClusterUntil_) {
            ClusterLeft_ = 0;   // could not fund it in time — drop the intent rather than stall forever
        } else if (S.Teams[MyTeam_].Gold >= BuildingCostFor(S.Cv, ClusterType_) +
                                              S.Cv.AiClusterFillUnits * S.Units[ClusterType_].Cost &&
                   (K.MaxBuildings <= 0 || MyBuildings < K.MaxBuildings)) {
            Fixed X, Y, Tx, Ty;
            bool Have = false;
            if (ClusterType_ == UnitMiner) {
                if (AiBestMineTarget(S, MyTeam_, Tx, Ty))
                    Have = AiPlaceNear(S, MyTeam_, ClusterType_, Tx, Ty, X, Y);
            } else {
                AiFrontTarget(S, MyTeam_, Tx, Ty);
                Have = AiPlaceNear(S, MyTeam_, ClusterType_, Tx, Ty, X, Y);
            }
            if (!Have) Have = AiPlaceSpot(S, MyTeam_, ClusterType_, X, Y);
            if (Have) {
                Out[Count++] = InputEvent::Place(MyTeam_, ClusterType_, X, Y);
                --ClusterLeft_;
                return;
            }
            ClusterLeft_ = 0;   // nowhere legal left for this type; stop pretending
        }
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
    // PER-TIER batch size now (K.QueueDepth), not the shared rps.ai.queue_depth. The shared value
    // was the single biggest reason easy buried a beginner: batching 8 at a time against a
    // first-timer issuing ~10 queue commands a match is a 20x throughput gap in decisions alone.
    // Carts use the SHALLOW miner depth, soldiers the tier's own depth. Keeping every camp ticking
    // over on a short queue while the surplus banks toward another camp is the whole opening.
    const int32_t RawDepth = Want == UnitMiner ? S.Cv.AiMinerQueueDepth : K.QueueDepth;
    const int32_t Depth = RawDepth > 0 ? RawDepth : 1;
    const int32_t Factor = S.Cv.AiExpandGoldFactor > 100 ? S.Cv.AiExpandGoldFactor : 100;
    // Below the floor the expansion MARGIN is skipped: buy the building the moment it is affordable
    // rather than waiting to hold ExpandGoldFactor% of its price. A floor that waits for a comfortable
    // bank is not a floor -- the whole point is to have capacity standing before the attack, and the
    // attack does not wait for the AI to feel rich.
    const bool NeedDefence = MyCombatBldg < K.DefenceFloor && Want != UnitMiner;
    // Keep back enough for a few UNITS, in the units' own currency. The percentage margin was
    // described as keeping "a reserve for units", but a percentage of a BUILDING price knows nothing
    // about what a unit costs: quadruple the unit costs and the AI bought camp, camp, camp and
    // stranded itself on 200 gold with a 400 miner it could never afford -- zero income, forever.
    // A reserve denominated in buildings cannot protect a purchase denominated in units.
    // The guard is deliberately MINIMAL: refuse the purchase only if it would leave the AI unable to
    // afford even ONE unit. A flat 3-unit reserve also worked, but it fired at default tuning too and
    // that is a balance change — it made hard fractionally stingier about expanding and cost it 15 of
    // 32 against medium. A robustness guard that is not a no-op in the normal case is a re-tune in
    // disguise. This version only ever triggers in the pathological case it exists for (quadruple the
    // unit costs and the AI used to buy camp, camp, camp and strand itself with no income at all).
    const int32_t WantUnitCost = S.Units[Want].Cost > 0 ? S.Units[Want].Cost : 1;
    const bool LeavesEnoughForAUnit = Gold - Price >= WantUnitCost;
    const bool CanAffordAnother = LeavesEnoughForAUnit &&
                                  (NeedDefence ? (Gold >= Price)
                                               : (Gold >= Price * Factor / 100));
    // EXPAND on SURPLUS, not on saturation. The earlier saturation test (all buildings already
    // carrying Depth work) could never fire, because queueing drains faster than a
    // one-decision-per-tick AI refills — so it never concluded it needed capacity and sat on its
    // gold. Idle gold is the honest signal, and it is what a human acts on: a recorded human win had
    // 21 buildings to the AI's 8 (2026-07-25 flight recordings, #144).
    // Building CAP (K.MaxBuildings, 0 = unlimited): counted over ALL producing buildings, not per
    // type, because it is a proxy for "how much stuff does this tier build" — the recordings put a
    // beginner at 1-4 buildings and easy at 8-12, and since production is FLAT PER BUILDING (#132)
    // that count IS the army-throughput multiplier. Capping it is what makes a 200-unit flood
    // arithmetically impossible rather than merely slower. The HQ is excluded (it produces
    // nothing), so the cap counts exactly the buildings that generate units.
    // ...but the LAST slot is RESERVED FOR COMBAT while no combat building stands, because
    // otherwise the cap DEADLOCKS the tier. Chasing WorkerTarget fills every slot with mining
    // camps; the first soldier building is then forbidden forever, so the AI wants soldiers,
    // cannot place anywhere to make them, and banks gold for the rest of the match. Observed on
    // easy (max_buildings 4, defence_floor 0) in three recorded matches on 2026-07-27: 4 camps,
    // 27 workers, ZERO soldiers, 30k idle gold. It only became reachable when the AI learned to
    // expand its mining — before that it placed one building per type, which hid the trap.
    // MaxBuildings still means exactly what it says; the ECONOMY just stops one short of it until
    // there is somewhere to make soldiers. (Tiers with an unlimited cap are untouched.)
    const int32_t EconLimit = (K.MaxBuildings > 1 && MyCombatBldg == 0) ? K.MaxBuildings - 1
                                                                       : K.MaxBuildings;
    const bool UnderCap = K.MaxBuildings <= 0 ||
                          MyBuildings < (Want == UnitMiner ? EconLimit : K.MaxBuildings);
    // ...and the cap YIELDS BY ONE for a first combat building, whatever the reason there is none.
    // Reserving a slot stops the deadlock from forming; this clears it if it ever forms anyway —
    // a razed combat building, a hand-tuned cap, a future ordering change. Being one building over
    // a tuning cap for one purchase is nothing next to a tier that cannot fight at all, and it is
    // bounded: exactly one, and only while the AI owns zero combat buildings. This is NOT the
    // per-type exemption rejected above (that one floored every cap at four, one per unit type).
    const bool FirstCombat = Want != UnitMiner && MyCombatBldg == 0;
    // The exemption is "I have NO producing buildings at all" — the forced opening camp — NOT "I have
    // none OF THIS TYPE". Per-type would silently floor the cap at one building per unit type (four),
    // so max_buildings below 4 could never bind: asking for 3 still produced 4. The cap has to mean
    // what it says, or it is not a tuning knob.
    const bool OpeningCamp = MyBuildings == 0;
    if ((OpeningCamp || UnderCap || FirstCombat) &&
        (Cap_.Owned == 0 ? Gold >= Price : CanAffordAnother)) {
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
            // Commit to the rest of the cluster. Combat types only: mining camps are placed against
            // specific deposits, so a run of them just fights AiBestMineTarget for the same ground.
            // SIZE THE CLUSTER TO THE ECONOMY. Commit to at most K.BuildCluster buildings, and only
            // to as many as it can also STOCK with ClusterFillUnits units each -- a cluster it cannot
            // fill is just a quiet period followed by empty buildings. Poor -> 1 (no clustering at
            // all, so nothing changes when it is broke); rich -> the full cluster, which is exactly
            // when a human commits to parallel production of a counter.
            // The fill guarantee is checked PER BUILDING as each one is placed (see the cluster
            // branch above), not as a lump sum up front. Demanding the whole cluster's gold at once
            // made clustering unreachable: 2 x (1500 + 10 x 50) is over 3000 banked, and hard never
            // holds that because it spends eagerly — the feature simply never fired, at any fill
            // value. "In quick succession" means the buildings arrive as income allows, each one
            // still stocked when it lands; the patience window bounds how long that may take.
            const int32_t FillCost = S.Cv.AiClusterFillUnits * WantUnitCost;
            if (K.BuildCluster > 1 && Want != UnitMiner && Gold >= Price + FillCost) {
                ClusterType_ = Want;
                ClusterLeft_ = K.BuildCluster - 1;   // this tick places the first of them
                ClusterUntil_ = Tick + 300;          // 30s of patience, then back to producing units
            }
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
