#include "Rps/AiController.h"

namespace Rps {

AiKnobs KnobsFor(const CvSnapshot& Cv, EAiTier Tier) {
    switch (Tier) {
        case EAiTier::Easy:
            return {Cv.AiEasyOpenWorkers, Cv.AiEasyWorkerTarget, Cv.AiEasyStaleness,
                    Cv.AiEasyPrecision, Cv.AiEasyCadence, Cv.AiEasyJitter, Cv.AiEasyHysteresis,
                    Cv.AiEasyAllinLead, Cv.AiEasySoldierRatio,
                    Cv.AiEasyQueueDepth, Cv.AiEasyMaxBuildings, Cv.AiEasyDefenceFloor,
                    Cv.AiEasyBuildCluster, Cv.AiEasyMinerQueue, Cv.AiEasyWaveLead, Cv.AiEasyCounterChest};
        case EAiTier::Hard:
            return {Cv.AiHardOpenWorkers, Cv.AiHardWorkerTarget, Cv.AiHardStaleness,
                    Cv.AiHardPrecision, Cv.AiHardCadence, Cv.AiHardJitter, Cv.AiHardHysteresis,
                    Cv.AiHardAllinLead, Cv.AiHardSoldierRatio,
                    Cv.AiHardQueueDepth, Cv.AiHardMaxBuildings, Cv.AiHardDefenceFloor,
                    Cv.AiHardBuildCluster, Cv.AiHardMinerQueue, Cv.AiHardWaveLead, Cv.AiHardCounterChest};
        case EAiTier::Medium:
        default:
            return {Cv.AiMediumOpenWorkers, Cv.AiMediumWorkerTarget, Cv.AiMediumStaleness,
                    Cv.AiMediumPrecision, Cv.AiMediumCadence, Cv.AiMediumJitter,
                    Cv.AiMediumHysteresis, Cv.AiMediumAllinLead, Cv.AiMediumSoldierRatio,
                    Cv.AiMediumQueueDepth, Cv.AiMediumMaxBuildings, Cv.AiMediumDefenceFloor,
                    Cv.AiMediumBuildCluster, Cv.AiMediumMinerQueue, Cv.AiMediumWaveLead, Cv.AiMediumCounterChest};
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
// What ONE of our type I is worth against ONE enemy of type J: our effective strength minus theirs,
// where strength is damage x health (the Lanchester form) and damage carries the RPS multiplier.
// Derived from Cv/UnitTable, so it re-derives itself when the balance is tuned — the scissor change
// on 2026-07-28 (hp 80->60, damage 12->15) moves these numbers with no code edit.
int64_t MatchupValue(const Sim& S, uint8_t I, uint8_t J) {
    const int32_t Mult = S.Cv.CounterMultiplier > 0 ? S.Cv.CounterMultiplier : 1;
    const int32_t Out = S.Units[I].Attack * (UnitTable[I].Beats == J ? Mult : 1);
    const int32_t In = S.Units[J].Attack * (UnitTable[J].Beats == I ? Mult : 1);
    return static_cast<int64_t>(Out) * S.Units[I].MaxHp - static_cast<int64_t>(In) * S.Units[J].MaxHp;
}
// BEST RESPONSE TO THE WHOLE ENEMY COMPOSITION — argmax_i (A.q)_i, not counter(argmax_j q_j).
//
// Countering the single most numerous enemy type is wrong three ways, and the owner exploited all
// three on 2026-07-28. It discards every enemy type except the mode, so against his near-uniform
// army (rock 5.8 / paper 5.3 / scissor 4.6 buildings) one pure counter is 3x against a third of the
// field and 1/3x against another third. It is a period-3 orbit against anyone who adapts. And it
// hands the opponent a STEERING HANDLE: the mode is the one thing he controls for free, so leading
// with Paper forced the AI to want Scissor — the 4000-gold building — on demand.
//
// Weighting every enemy type by its count removes the handle, because no single type can capture the
// decision any more. Ties keep the lowest index, so it stays deterministic. Scores are int64: counts
// reach the hundreds and MatchupValue is a product of two stats, so int32 would overflow.
uint8_t BestResponse(const Sim& S, const int32_t Seen[3], int64_t& OutMargin) {
    int64_t Score[3] = {0, 0, 0};
    int64_t PerUnitMax = 1;
    for (int32_t I = 0; I < 3; ++I) {
        for (int32_t J = 0; J < 3; ++J) {
            const int64_t V = MatchupValue(S, static_cast<uint8_t>(UnitRock + I),
                                           static_cast<uint8_t>(UnitRock + J));
            const int64_t Mag = V < 0 ? -V : V;
            if (Mag > PerUnitMax) PerUnitMax = Mag;
            if (Seen[J] > 0) Score[I] += static_cast<int64_t>(Seen[J]) * V;
        }
    }
    // Deliberately NOT normalised by building price. Dividing the score by the building's cost was
    // tried, on the reasoning that capacity is the binding constraint and a Scissor building is 4x a
    // Rock one — but it measured WORSE (11-5 against hard, vs 12-4 raw), because it talks the AI out
    // of the expensive answer exactly when the enemy army genuinely is the one Scissor hard-counters.
    // The missing term is amortisation over a building's LIFETIME, not division by its price; until
    // that exists, raw strength is the better of the two available approximations.
    int32_t Best = 0;
    for (int32_t I = 1; I < 3; ++I)
        if (Score[I] > Score[Best]) Best = I;
    OutMargin = PerUnitMax;   // one enemy unit's worth of score — the unit hysteresis is denominated in
    return static_cast<uint8_t>(UnitRock + Best);
}
// TARGET MIX — a DISTRIBUTION over the three soldier types, in permille (#158).
//
// This exists because BestResponse above, correct as it is, still returns ONE type, and one type is
// the loss mechanism: the owner fields all three, so a third of his army hard-counters ours at
// counter_mult 3 no matter which one we pick. Two attempts to fix that by scoring types better both
// measured worse, and the lesson was that the argmax itself was the defect — see Tunables.h at
// rps.ai.mix_enable, and negative-result-best-response.md.
//
// STEP 1: the NASH BASE, closed-form. In an antisymmetric 3-cycle the equilibrium is "play each type
// in proportion to the strength of the matchup it is NOT part of". Proof, for A[i][j] the payoff of i
// against j with the cycle P>R (a1), S>P (a2), R>S (a3): equalising every row of A.p gives
// a1*p_P = a3*p_S and a1*p_R = a2*p_S, hence p ~ (a2, a3, a1) for (R, P, S). Three multiplies, no LP.
//
// The cycle is DERIVED from UnitTable rather than written down, so a rebalance that reversed the
// relation could not leave a stale table behind: for each type I, the matchup it sits out is the one
// between the OTHER two, whichever way round that pair beats.
//
// STEP 2: the TILT, off by default. Nash is safe, not strong. Only the enemy's OVER-supply of a type
// is exploitable, so the term is keyed on (their count - what a Nash opponent would field), never on
// the raw count: raw counts make this the plain best response, which against a uniform enemy is the
// pure-Scissor over-pick this whole function exists to remove.
//
// STEP 3: the CAP, which is the tuning axis (CircuitAI's max_percent). Ceiling any one type and
// redistribute the remainder over the rest, repeating until stable — at most two clamps for three
// types. This is the part that structurally prevents hard-countering into a shape that counters us.
//
// Integer/int64 throughout, no allocation, and a floor of 1 permille per type so no type is ever
// permanently excluded (which is what keeps all three alive, an acceptance criterion in its own right).
void TargetMix(const Sim& S, const int32_t Seen[3], int32_t Out[3]) {
    constexpr int64_t Scale = 1000;                  // permille
    int64_t W[3] = {1, 1, 1};
    for (int32_t I = 0; I < 3; ++I) {
        const int32_t J = (I + 1) % 3, K = (I + 2) % 3;
        const uint8_t Jt = static_cast<uint8_t>(UnitRock + J), Kt = static_cast<uint8_t>(UnitRock + K);
        // The matchup I sits out, oriented so the winner is the first argument.
        const int64_t A = UnitTable[Jt].Beats == Kt ? MatchupValue(S, Jt, Kt) : MatchupValue(S, Kt, Jt);
        W[I] = A > 0 ? A : 1;                        // a non-positive margin must not zero a type out
    }
    // STEP 2: blend in the exploit of their deviation from equilibrium, if the tilt is on.
    const int32_t Tilt = S.Cv.AiMixTiltPct < 0 ? 0 : (S.Cv.AiMixTiltPct > 100 ? 100 : S.Cv.AiMixTiltPct);
    if (Tilt > 0) {
        const int64_t Total = static_cast<int64_t>(Seen[0]) + Seen[1] + Seen[2];
        const int64_t SumW = W[0] + W[1] + W[2];
        int64_t Ex[3] = {0, 0, 0};
        for (int32_t J = 0; J < 3; ++J) {
            const int64_t Fair = Total * W[J] / SumW;      // what a Nash opponent would field of J
            const int64_t Over = Seen[J] - Fair;
            if (Over <= 0) continue;                       // under-supplied types are not exploitable
            const uint8_t Jt = static_cast<uint8_t>(UnitRock + J);
            const uint8_t Ct = CounterTo(Jt);
            if (Ct == UnitNone) continue;
            Ex[Ct - UnitRock] += Over * MatchupValue(S, Ct, Jt);
        }
        const int64_t SumEx = Ex[0] + Ex[1] + Ex[2];
        if (SumEx > 0) {
            // Both terms normalised to Scale first, so the blend weights mean what they say rather
            // than being dominated by whichever term happens to carry bigger raw numbers.
            for (int32_t I = 0; I < 3; ++I) {
                const int64_t N = W[I] * Scale / SumW;
                const int64_t E = Ex[I] * Scale / SumEx;
                W[I] = ((100 - Tilt) * N + Tilt * E) / 100;
                if (W[I] < 1) W[I] = 1;
            }
        }
    }
    // STEP 3: normalise to Scale under a per-type ceiling. Below 34% three types cannot fill the
    // distribution between them, so the knob is clamped up rather than allowed to under-fill.
    int32_t CapPct = S.Cv.AiMixCapPct;
    if (CapPct < 34) CapPct = 34;
    if (CapPct > 100) CapPct = 100;
    const int64_t CapRaw = CapPct * Scale / 100;
    bool Capped[3] = {false, false, false};
    int64_t Rem = Scale;
    for (int32_t Pass = 0; Pass < 3; ++Pass) {
        int64_t SumFree = 0;
        for (int32_t I = 0; I < 3; ++I)
            if (!Capped[I]) SumFree += W[I];
        if (SumFree <= 0) break;
        int32_t Worst = -1;
        for (int32_t I = 0; I < 3; ++I) {
            if (Capped[I]) continue;
            Out[I] = static_cast<int32_t>(Rem * W[I] / SumFree);
            if (Out[I] > CapRaw && (Worst < 0 || Out[I] > Out[Worst])) Worst = I;
        }
        if (Worst < 0) break;                        // nothing over the ceiling — the split is stable
        Out[Worst] = static_cast<int32_t>(CapRaw);
        Capped[Worst] = true;
        Rem -= CapRaw;
        if (Rem < 0) Rem = 0;
    }
    for (int32_t I = 0; I < 3; ++I)
        if (Out[I] < 1) Out[I] = 1;                  // never write a type out of the plan entirely
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
// LEAST-SATISFIED QUOTA (0 A.D. Petra, attackPlan.js): the soldier type proportionally furthest
// behind its target share. Sorting by (have + queued) / target and taking the front is what makes
// this MIX BY CONSTRUCTION — every type that falls behind becomes the next thing produced, so no
// type can be over-bought, which is exactly the failure mode of every argmax-of-a-score.
//
// "have + queued", not "have": a batch already ordered at a building is supply in flight, and
// ignoring it made the AI re-order the same type for as long as the queue took to drain.
//
// Ratios are compared CROSS-MULTIPLIED (Have[J] * Target[I] < Have[I] * Target[J]) rather than by
// dividing, so it is exact integer arithmetic with no rounding tie-breaks; a strict < keeps the
// lowest index on a tie, which is what makes the opening deterministic (all counts 0 -> Rock, the
// same neutral opener the argmax path picks).
//
// Rank all three, furthest-behind first, into Order. Both the PLAN and its FALLBACK read this same
// ranking — see LeastSatisfied and BestQueueableSoldier below, and the note there about why those
// have to be two separate decisions.
void RankByQuota(const Sim& S, uint8_t Team, const int32_t Target[3], int32_t Order[3]) {
    int64_t Have[3] = {0, 0, 0};
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || S.Team[I] != Team) continue;
        const uint8_t Ty = S.Type[I];
        if (Ty < UnitRock || Ty > UnitScissor) continue;
        if (S.IsBuilding(I)) {
            if (!S.IsHomeBase(I)) Have[Ty - UnitRock] += S.Queue[I];   // supply in flight
        } else {
            ++Have[Ty - UnitRock];
        }
    }
    Order[0] = 0; Order[1] = 1; Order[2] = 2;
    // Selection sort of three. A zero target can never be promoted (its right-hand side is 0, so the
    // strict < fails), which pushes an excluded type to the bottom.
    for (int32_t A = 0; A < 2; ++A)
        for (int32_t B = A + 1; B < 3; ++B) {
            const int32_t I = Order[A], J = Order[B];
            if (Have[J] * Target[I] < Have[I] * Target[J]) { Order[A] = J; Order[B] = I; }
        }
}
// The PLAN: the type furthest behind quota, whether or not it is affordable this tick.
//
// Affordability deliberately does NOT enter here, and getting that wrong cost two full measurement
// rounds. Filtering the plan down to what is affordable right now sounds like the obvious robustness
// fix, and it measured a pure-Rock army (356/0/0, worse than the argmax it replaced) — because a Rock
// building is 1000 against Paper's 2500 and Scissor's 4000, so Rock is affordable essentially always
// and an affordability-filtered plan simply never saves for anything else. The plan has to be allowed
// to want what it cannot yet buy; that is what the counter chest is FOR.
uint8_t LeastSatisfied(const Sim& S, uint8_t Team, const int32_t Target[3]) {
    int32_t Order[3];
    RankByQuota(S, Team, Target, Order);
    return static_cast<uint8_t>(UnitRock + Order[0]);
}
// The FALLBACK, and the reason the plan and the fallback are two different things.
//
// While the plan saves for an expensive building there is nothing to queue at for that type, and the
// never-stand-idle branch then spent the tick on CARTS — so against medium the tier queued 129 units
// across a whole match (485 with the mix off) and ended on 3 soldiers: 0-16, down from 14-2. Petra
// does not hit this because one 0 A.D. barracks trains every type it knows; here each type needs its
// own building, so "furthest behind quota" and "buildable this instant" routinely disagree.
//
// The answer is not to compromise the plan but to spend the SURPLUS better: the next type down the
// same ranking that we own a building of and can pay for. Paid out of Spendable, so the chest's
// reserve is still untouchable and the expensive building still lands the moment income allows —
// the AI saves for Scissor while making Rock, instead of saving for Scissor while making carts.
// UnitNone if no type is queueable, in which case the cart fallback below still runs.
uint8_t BestQueueableSoldier(const Sim& S, uint8_t Team, const int32_t Target[3], int32_t Spendable,
                             int32_t Depth, int32_t& OutSlot, int32_t& OutQueue) {
    int32_t Order[3];
    RankByQuota(S, Team, Target, Order);
    for (int32_t R = 0; R < 3; ++R) {
        const int32_t Idx = Order[R];
        if (Target[Idx] <= 0) continue;
        const uint8_t T = static_cast<uint8_t>(UnitRock + Idx);
        const TypeCapacity C = SurveyType(S, Team, T);
        if (C.Slot < 0 || C.Queue >= Depth) continue;
        const int32_t UnitCost = S.Units[T].Cost > 0 ? S.Units[T].Cost : 1;
        if (Spendable < UnitCost) continue;
        OutSlot = C.Slot;
        OutQueue = C.Queue;
        return T;
    }
    return UnitNone;
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
    // SERVABLE, not merely nearest. Nearest-first has a trap the furthest-forward rule never hit: a
    // deposit can be unclaimed AND unclaimable — at mine rows 1/5 with mine_clearance 6 there is no
    // legal camp spot around the home row at all, so the AI proposed the same illegal target every
    // tick and its economy froze at 15 workers with 3930 gold banked (caught by
    // TestAiExpandsCapacity the moment the owner's tuning became the default). So walk candidates
    // outward and take the first one a camp can actually be placed near. Bounded to MaxTries so a
    // pathological map costs a handful of ring searches, not one per deposit.
    constexpr int32_t MaxTries = 8;
    uint64_t Tried = 0;
    for (int32_t Try = 0; Try < MaxTries; ++Try) {
    int32_t Best = -1;
    Fixed BestDepth{0};
    for (int32_t M = 0; M < NumMines; ++M) {
        if (S.MineGold[M] <= 0) continue;
        if ((Tried >> M) & 1ull) continue;          // already rejected: no legal spot near it
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
        // NEAREST unclaimed row wins — it used to be the furthest FORWARD, and that walked the
        // economy into the contested half. The frontier follows our frontmost SURVIVOR, so a single
        // soldier deep in enemy ground makes an enemy-half mine "buildable", and the camp planted
        // there dies with the carts working it. Measured on the owner's 2026-07-27 recordings: the
        // AI's camps sat at a median depth of 106-134 (midfield is 120) while HIS sat at 51-67 —
        // and its building count fell 21 -> 3-9 over a match while his grew to 31. He described the
        // same rule in words: leave a camp at every mine row as the frontline advances. Filling
        // outward from home cannot re-mine a worked row, because Served already excluded those, and
        // it shortens every cart trip on top. First index wins a tie, so it stays deterministic.
        const Fixed Depth = Team == 0 ? S.MineY[M] : WorldHeight - S.MineY[M];
        if (Best < 0 || Depth < BestDepth) { BestDepth = Depth; Best = M; }
    }
        if (Best < 0) return false;                 // nothing unclaimed left anywhere
        Fixed Px, Py;
        if (AiPlaceNear(S, Team, UnitMiner, S.MineX[Best], S.MineY[Best], Px, Py)) {
            OX = S.MineX[Best];
            OY = S.MineY[Best];
            return true;
        }
        Tried |= 1ull << Best;                      // unclaimable — try the next row out
    }
    return false;
}

// The deposit OUR OWN CARTS ARE ALREADY WORKING that most needs another drop-off. This is where a
// camp goes once AiBestMineTarget has nothing left to CLAIM — which, in a real match, is most of
// them (owner's note 2026-08-14: "spawn mine camps at the gold rows its carts is mining from").
//
// Read out of his 2026-08-13/14 recordings of wins over hard. In a typical match hard placed 17
// camps: the first five were ring-placed against its two starter rows (y=225..230, gold at
// y=235/239) and the other TWELVE landed exactly on the AiPlaceSpot ladder — 224, 221, 218, 215,
// 212, 206, 203, 200 — marching steadily into empty ground while every cart it owned still worked
// the rows behind them. His own camps sat at y=9..28 against gold at y=1/5, and the moment his
// frontier reached the y=60 row he opened a LINE of five more at y=54..56, right on it.
//
// Why the lattice is the wrong answer but "place fewer camps" is a WORSE one — measured, 12 matches
// vs OwnerBot, and it is the whole reason this function exists rather than a refusal:
//
//     lattice fallback (old)      owner 0 : 12 hard    ai wrk 654, bld 20
//     refuse to place at all      owner 12 : 0 hard    ai wrk  10, bld 10
//
// Production is FLAT PER BUILDING (#132), so a mining camp is not just a drop-off — it is the cart
// FACTORY, and camp count IS economic throughput. Capping camps at one per unclaimed deposit capped
// cart production and the economy collapsed outright. So keep placing as many camps as the tier
// wants; only fix WHERE they land. A camp beside a deposit its carts already work shortens every one
// of those round trips (carts deposit at the nearest own camp, Sim §12.4) instead of lengthening
// them, and it still adds its full share of production.
//
// WHY THIS IS REACTIVE AND CANNOT BE MADE PROACTIVE (asked 2026-08-15: "prevent all their carts from
// having to travel the entire way back to the last row the first time they reach a new row"). The
// honest answer is that placing a camp on a row before the carts get there is not available to
// ANYONE, AI or human: §5.3 makes a team's buildable depth its FRONTMOST LIVE UNIT, so a row you have
// not physically reached is not buildable, and the frontier only advances once a cart has walked
// there. The first wave onto a new row therefore eats one full-length round trip by construction.
//
// It was tried, twice, and both are worth not repeating. Claiming the next unclaimed row whenever
// rich cost hard 60s and a third of its army vs OwnerBot (295s/239 soldiers against 236s/352), because
// a claim spends the tick's one action and it claimed every deposit on the map. Gating that on the
// worked row running down, one row ahead at a time, was near-neutral on strength but did NOT remove
// the transition spike — measured mean haul still hit 47 at the row change — for exactly the frontier
// reason above: at the moment the cue fires, the next row is still unbuildable.
//
// So the trigger below is the earliest one that exists: a cart's Target is set when it RETARGETS, not
// when it arrives, so the camp goes up while the first cart is still crossing. Genuinely pre-empting
// the move would need the AI to send a unit ahead to extend its frontier first — a real strategy, and
// a much bigger change than a placement rule.
//
// Scored carts x haul: the total travel a new drop-off there would save per cycle, which is the
// quantity actually being minimised. A deposit no cart visits scores zero and is skipped — this
// never invents an expansion, that is AiBestMineTarget's job. Strict > keeps the lowest index on a
// tie, and everything is integer world units, so the choice is deterministic and hash-safe.
// MinHaul is the shortest trip worth answering with a camp. 0 = "any improvement will do", which is
// the placement FALLBACK's meaning (we already decided to buy a camp; put it somewhere useful).
// A positive value is the FOLLOW-THE-ORE trigger: only report a deposit whose carts are hauling
// further than this, so the caller can decide to buy a camp it had no other reason to want.
bool AiWorkedMineTarget(const Sim& S, uint8_t Team, Fixed& OX, Fixed& OY, int32_t MinHaul = 0) {
    const Fixed Limit = Team == 0 ? S.FrontierT0 : S.FrontierT1;
    // Carts per deposit. A miner's Target IS its mine index for the whole cycle (WorkerSeek clears it
    // only on deposit), so this counts the carts committed to each one, not merely those stood on it.
    int32_t Carts[NumMines] = {};
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || S.IsBuilding(I)) continue;
        if (S.Team[I] != Team || S.Type[I] != UnitMiner) continue;
        const int32_t M = S.Target[I];
        if (M >= 0 && M < NumMines) ++Carts[M];
    }
    // Bounded like AiBestMineTarget: a deposit whose neighbourhood is full (its near ring is already
    // camps) is struck off and the next-best tried, rather than costing one ring search per deposit.
    constexpr int32_t MaxTries = 8;
    uint64_t Tried = 0;
    for (int32_t Try = 0; Try < MaxTries; ++Try) {
        int32_t Best = -1;
        int64_t BestScore = 0;
        for (int32_t M = 0; M < NumMines; ++M) {
            if (S.MineGold[M] <= 0 || Carts[M] <= 0) continue;
            if ((Tried >> M) & 1ull) continue;
            // Outside our buildable depth the placement can only ever be refused.
            if (Team == 0 ? S.MineY[M] > Limit : S.MineY[M] < Limit) continue;
            // Current haul: Chebyshev to the nearest own camp — the trip this deposit's carts make
            // today, and the trip a camp here would shorten. Chebyshev to match the grid/steering.
            int32_t Haul = -1;
            for (int32_t B = 0; B < S.Count; ++B) {
                if (!S.IsAlive(B) || !S.IsBuilding(B) || S.Team[B] != Team) continue;
                if (S.Type[B] != UnitMiner || S.IsHomeBase(B)) continue;
                const int32_t Ddx = (S.PosX[B] - S.MineX[M]).ToInt();
                const int32_t Ddy = (S.PosY[B] - S.MineY[M]).ToInt();
                const int32_t Adx = Ddx < 0 ? -Ddx : Ddx, Ady = Ddy < 0 ? -Ddy : Ddy;
                const int32_t D = Adx > Ady ? Adx : Ady;
                if (Haul < 0 || D < Haul) Haul = D;
            }
            if (Haul <= 0) continue;        // no camp at all yet (the opening path owns that case)
            if (Haul <= MinHaul) continue;  // close enough already — a camp here would buy nothing
            const int64_t Score = static_cast<int64_t>(Carts[M]) * Haul;
            if (Score > BestScore) { BestScore = Score; Best = M; }
        }
        if (Best < 0) return false;
        Fixed Px, Py;
        if (AiPlaceNear(S, Team, UnitMiner, S.MineX[Best], S.MineY[Best], Px, Py)) {
            OX = S.MineX[Best];
            OY = S.MineY[Best];
            return true;
        }
        Tried |= 1ull << Best;
    }
    return false;
}

// WHICH CAMP TO BUILD THE NEXT CARTS AT (owner, 2026-08-15: "produce new mine carts from the mine
// camps closest to where there is most gold left, so new carts don't have to travel as long after
// spawned"). A cart is SPAWNED on a ring around the camp that built it, so the camp choice IS the
// cart's starting position, and it then walks to the deposit nearest THAT point.
//
// SurveyType picks the shortest queue — pure load-balancing, blind to geography — so a fresh cart is
// as likely to appear at a mined-out corner of the map as beside the ore.
//
// The queue rule is kept, though, and not merely as a tie-break: production is FLAT PER BUILDING
// (#132), so stacking orders at the single best camp does NOT build them faster, it just idles every
// other camp. So this is a two-level rule — only camps with ROOM (Queue < Depth) are candidates, and
// among those the nearest to live ore wins. Camps fill up and drop out, which spreads the orders as
// before while always biasing the next one toward the gold.
//
// Owned is counted exactly as SurveyType does, because the placement branch keys on it ("do I own any
// of these yet"), and a different count there would change when the AI expands.
TypeCapacity SurveyMinerCampsByGold(const Sim& S, uint8_t Team, int32_t Depth) {
    TypeCapacity C;
    int32_t BestDist = -1;
    int32_t BestGold = -1;
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || !S.IsBuilding(I) || S.Team[I] != Team || S.Type[I] != UnitMiner) continue;
        if (S.IsHomeBase(I)) continue;
        ++C.Owned;
        if (S.Queue[I] >= Depth) continue;          // no room for this batch — not a candidate
        // Distance to the nearest LIVE deposit, and how much is left in it. Nearest-first is the
        // travel term; the gold left is the tie-break, so between two equally close camps the next
        // carts go to the one whose deposit will still be there when they arrive.
        int32_t Dist = -1, GoldAt = 0;
        for (int32_t M = 0; M < NumMines; ++M) {
            if (S.MineGold[M] <= 0) continue;
            const int32_t Ddx = (S.PosX[I] - S.MineX[M]).ToInt();
            const int32_t Ddy = (S.PosY[I] - S.MineY[M]).ToInt();
            const int32_t Adx = Ddx < 0 ? -Ddx : Ddx, Ady = Ddy < 0 ? -Ddy : Ddy;
            const int32_t D = Adx > Ady ? Adx : Ady;
            if (Dist < 0 || D < Dist || (D == Dist && S.MineGold[M] > GoldAt)) {
                Dist = D;
                GoldAt = S.MineGold[M];
            }
        }
        if (Dist < 0) continue;                     // no live ore anywhere: leave it to the caller
        if (BestDist < 0 || Dist < BestDist || (Dist == BestDist && GoldAt > BestGold)) {
            BestDist = Dist;
            BestGold = GoldAt;
            C.Slot = I;
            C.Queue = S.Queue[I];
        }
    }
    // Nothing with room (or no ore left): fall back to the plain least-queue pick, so the caller's
    // own "is there room" test decides, exactly as it did before this existed.
    if (C.Slot < 0) {
        const TypeCapacity Plain = SurveyType(S, Team, UnitMiner);
        C.Slot = Plain.Slot;
        C.Queue = Plain.Queue;
    }
    return C;
}

// LAST RESORT FOR A CAMP: the legal spot NEAREST LIVE ORE, rather than the first one a sweep hits.
//
// Same candidate set as AiPlaceSpot — identical passes, steps and bounds, so it can only ever choose
// ground AiPlaceSpot would also have accepted — but it keeps the best instead of returning early.
// That difference is the whole point. AiPlaceSpot sweeps in rows marching AWAY from the baseline and
// takes the first hit, so once the band around the worked rows is legally full (clearance plus
// footprint saturates it at ~7 camps) every later camp lands further out than the one before, in a
// straight line away from the gold. Traced on a mirror Hard match: camps at y=21, 25, 25, 28, 31, 34,
// 37, 37, 40, 43 against ore at y=1/5 — nearest-live-deposit distance climbing 16, 20, 20, 23, 26.
//
// It cannot fix the real ceiling, which is the FRONTIER: the same trace sat at frontier=40 for the
// whole match, so the y=60 row was never buildable and there was genuinely no ore-adjacent ground
// left. What it can do is stop the AI from choosing the WORST of the legal spots — and when the
// frontier does roll forward onto a new row, the nearest-ore rule puts the next camp on that row
// immediately instead of crawling to it three units per placement.
//
// Ties keep the first candidate in sweep order, so the choice stays deterministic and hash-safe.
bool AiPlaceNearestOre(const Sim& S, uint8_t Team, uint8_t Type, Fixed& OX, Fixed& OY) {
    const int32_t Fp = S.Cv.BuildingFootprint.ToInt();
    const int32_t Edge = (3 * Fp + 1) / 2;
    const int32_t Dir = Team == 0 ? 1 : -1;
    const int32_t MaxX = WorldWidth.ToInt() - Edge;
    bool Found = false;
    int32_t BestD = 0;
    for (int32_t Pass = 0; Pass < 2; ++Pass) {
        const int32_t XStart = Pass == 0 ? 2 * Fp + 2 : Edge;
        const int32_t XStep = Pass == 0 ? 2 * Fp : 2;
        const int32_t RowStep = Pass == 0 ? Fp + 1 : 2;
        const int32_t Base = Team == 0 ? (Pass == 0 ? Fp + 2 : Edge)
                                       : WorldHeight.ToInt() - (Pass == 0 ? Fp + 2 : Edge);
        const int32_t Rows = Pass == 0 ? (S.Cv.InitialFrontier.ToInt() + 21) / RowStep
                                       : WorldHeight.ToInt() / RowStep + 2;
        for (int32_t R = 0; R < Rows; ++R) {
            const int32_t Y = Base + Dir * R * RowStep;
            if (Y < 0 || Y > WorldHeight.ToInt()) break;
            for (int32_t X = XStart; X <= MaxX; X += XStep) {
                if (!S.CanPlaceBuilding(Team, Type, F(X), F(Y))) continue;
                int32_t D = -1;
                for (int32_t M = 0; M < NumMines; ++M) {
                    if (S.MineGold[M] <= 0) continue;
                    const int32_t Ddx = X - S.MineX[M].ToInt();
                    const int32_t Ddy = Y - S.MineY[M].ToInt();
                    const int32_t Adx = Ddx < 0 ? -Ddx : Ddx, Ady = Ddy < 0 ? -Ddy : Ddy;
                    const int32_t C = Adx > Ady ? Adx : Ady;
                    if (D < 0 || C < D) D = C;
                }
                if (D < 0) return false;                 // no live ore anywhere: nothing to be near
                if (!Found || D < BestD) { Found = true; BestD = D; OX = F(X); OY = F(Y); }
            }
        }
        // Pass 2 exists only to rescue a map where pass 1 finds nothing at all (AiPlaceSpot's note);
        // running it as well when pass 1 succeeded would widen the candidate set that the measured
        // layout depends on, so stop as soon as a pass has produced a spot.
        if (Found) break;
    }
    return Found;
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
    MixShare_[0] = MixShare_[1] = MixShare_[2] = 0;   // zero = "no mix"; the top tier fills it in
    State_ = EState::Opening;
    PeakMiners_ = 0;
    for (int32_t I = 0; I < RingSize; ++I) Ring_[I][0] = Ring_[I][1] = Ring_[I][2] = 0;
}

void AiController::DecideEvents(const Sim& S, uint32_t Tick, InputEvent* Out, int Cap, int& Count) {
    Count = 0;
    const AiKnobs K = KnobsFor(S.Cv, Tier_);

    // --- Scan the board once: my economy/army + the TRUE enemy soldier composition. ---
    int32_t MyMiners = 0, MySoldiers = 0, MyCombatBldg = 0, FoeCombatBldg = 0, FoeWorkers = 0;
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
        } else if (Ty == UnitMiner && !S.IsBuilding(I)) {
            // Enemy CARTS. Counted exactly, for the same reason enemy soldier buildings are: a cart
            // is not a moving army being scouted, it is a visible standing economy, and the commit
            // test below needs it — an economy is latent reinforcement.
            ++FoeWorkers;
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
        // TOP TIER: best response to the whole mixture. Everything below it keeps the measured
        // argmax path — the ladder's ordering was established against that behaviour and this is a
        // strategy change, not a bug fix (contrast the chest clamp, which was shared).
        //
        // It reuses CounterEnemy_ rather than adding a member, by storing the enemy type our chosen
        // soldier BEATS: `Soldier = CounterTo(CounterEnemy_)` then reproduces the choice exactly, and
        // the telemetry, the hysteresis and the cluster re-target all keep working untouched.
        if (Tier_ == EAiTier::Hard) {
            // #158: the TARGET MIX is re-derived here, on the same cadence as the enemy read it is
            // derived from — a distribution recomputed every tick would react faster than the tier's
            // own information model allows. Computed even before an enemy exists, because the Nash
            // base needs no observation; the tilt term is simply inert at zero counts.
            if (S.Cv.AiMixEnable != 0) TargetMix(S, Seen, MixShare_);
            const int32_t Total = Seen[0] + Seen[1] + Seen[2];
            if (Total > 0) {
                int64_t PerUnit = 1;
                const uint8_t Want_ = BestResponse(S, Seen, PerUnit);
                const uint8_t AsIf = UnitTable[Want_].Beats;   // CounterTo(AsIf) == Want_
                if (CounterEnemy_ == UnitNone) {
                    CounterEnemy_ = AsIf;
                } else if (AsIf != CounterEnemy_) {
                    // Hysteresis, in the same currency as before: the challenger must beat the
                    // incumbent by K.Hysteresis ENEMY UNITS' worth of score, not by a bare epsilon.
                    // Without a margin the score version chatters far more than argmax did, because
                    // it moves continuously with every count instead of only when the mode flips.
                    const uint8_t Cur = CounterTo(CounterEnemy_);
                    int64_t SNew = 0, SCur = 0;
                    for (int32_t J = 0; J < 3; ++J) {
                        const uint8_t Jt = static_cast<uint8_t>(UnitRock + J);
                        SNew += static_cast<int64_t>(Seen[J]) * MatchupValue(S, Want_, Jt);
                        if (Cur != UnitNone) SCur += static_cast<int64_t>(Seen[J]) * MatchupValue(S, Cur, Jt);
                    }






                    if (SNew > SCur + static_cast<int64_t>(K.Hysteresis) * PerUnit) {
                        CounterEnemy_ = AsIf;
                        // Re-targeting a live cluster onto the new single counter is right only while
                        // there IS a single counter. Under the mix the cluster's type came from the
                        // quota scheduler, and overwriting it with the argmax would reintroduce the
                        // over-buy at building scale — where it is most expensive, since production is
                        // flat per building. The quota picks the next cluster's type anyway.
                        if (ClusterLeft_ > 0 && S.Cv.AiMixEnable == 0)
                            ClusterType_ = CounterTo(CounterEnemy_);
                    }
                }
            }
        } else {
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
        }   // end of the pre-top-tier argmax path
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
    uint8_t Soldier =
        CounterEnemy_ != UnitNone ? CounterTo(CounterEnemy_) : static_cast<uint8_t>(UnitRock);
    // TOP TIER (#158): PRODUCE TOWARD THE DISTRIBUTION, never toward a chosen type. One line, and it
    // is deliberately here rather than at the queue/place sites further down, because EVERY consumer
    // of `Soldier` has to agree about what is being built or the mix leaks:
    //   * the defence-floor affordability test prices the building it is actually about to place;
    //   * the counter chest saves for that same building instead of one the AI no longer wants;
    //   * the Lanchester commit gate models the army it is actually fielding (it already handles our
    //     type NOT beating theirs — under a mix that is the common case, not an edge case);
    //   * a committed cluster is a cluster of the type the quota asked for.
    // Evaluated EVERY tick against live counts, unlike the target itself: the target is information
    // (cadence-gated) but "what am I short of right now" is bookkeeping over our own board, which a
    // player reads for free.
    const bool Mixing = MixShare_[0] > 0 || MixShare_[1] > 0 || MixShare_[2] > 0;
    if (Mixing) Soldier = LeastSatisfied(S, MyTeam_, MixShare_);

    if (MyMiners > PeakMiners_) PeakMiners_ = MyMiners;   // high-water mark (see the econ floor)

    // ---- WAVE ETA: how many ticks until the nearest enemy soldier REACHES something of ours ----
    // The owner's strategy, in his words: "I build economy, keeping queues as low as possible to
    // expand mining camps as fast as possible, until the first wave from the opponent comes — that
    // is when I build a cluster of counter unit buildings and max out their stacks. I counter right
    // before the first wave reaches the base camp."
    //
    // Reacting at first SIGHTING (what every tier did) throws that away: the wave needs ~34s to
    // cross the opening frontier, and the AI spent all of it making soldiers out of income it could
    // have compounded into camps. So measure ARRIVAL, not existence — Chebyshev over the enemy's own
    // speed, the same metric the sim moves them with, so it is ticks and not a guess.
    int32_t WaveEta = INT32_MAX;
    for (int32_t J = 0; J < S.Count; ++J) {
        if (!S.IsAlive(J) || S.IsBuilding(J) || S.Team[J] == MyTeam_) continue;
        const uint8_t Jt = S.Type[J];
        if (Jt < UnitRock || Jt > UnitScissor) continue;      // carts are not a wave
        const Fixed Sp = S.Units[Jt].Speed;
        if (Sp.Raw <= 0) continue;
        Fixed Near{0}; bool Any = false;
        for (int32_t B = 0; B < S.Count; ++B) {               // nearest thing of MINE it is walking at
            if (!S.IsAlive(B) || !S.IsBuilding(B) || S.Team[B] != MyTeam_) continue;
            const Fixed D = Max(Abs(S.PosX[B] - S.PosX[J]), Abs(S.PosY[B] - S.PosY[J]));
            if (!Any || D < Near) { Near = D; Any = true; }
        }
        if (!Any) continue;
        const int32_t Eta = (Near / Sp).ToInt();
        if (Eta < WaveEta) WaveEta = Eta;
    }
    // Inside the lead window the wave is "arriving": time to stop expanding and answer it. A tier
    // with WaveLead 0 keeps the old sighting rule, so the measured lower rungs are untouched.
    const bool WaveLanding = K.WaveLead <= 0 || WaveEta <= K.WaveLead;
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
    // TOP TIER: Lanchester with reinforcement, and the reinforcement term is the ENEMY'S ECONOMY.
    //
    // The linear test above has the right idea and the wrong arithmetic, and a first attempt at
    // fixing it with standing soldier buildings as rho_enemy measured exactly neutral — because at
    // the moment the AI commits, the owner has zero soldiers AND zero soldier buildings, so rho really
    // is 0 and the model correctly says "attack wins". It is right about the instant and wrong about
    // the future: his fourteen mining camps are production he has not bought yet.
    //
    // So bound reinforcement by what their economy can FUND, not by what they happen to have built.
    // Buildings are a constraint an opponent buys their way out of during the walk; income is the one
    // they cannot. Derived from observable state and Cv only — cart count (visible on screen, like
    // their buildings), CarryCapacity, dig_ticks, unit cost — so it re-derives itself when the economy
    // is tuned and it reads nothing a human could not.
    //
    //   u = alpha*A - rho_B ;  v = beta*B - rho_A ;  A wins iff  beta*u^2 > alpha*v^2 ? u>0 : v<=0
    //
    // Reduces exactly to the classic alpha*A^2 > beta*B^2 when neither side reinforces.
    bool OutproducesThem = false;
    if (Tier_ == EAiTier::Hard) {
        const uint8_t FoeType = CounterEnemy_ != UnitNone ? CounterEnemy_ : static_cast<uint8_t>(UnitRock);
        const int32_t FoeHp = S.Units[FoeType].MaxHp > 0 ? S.Units[FoeType].MaxHp : 1;
        const int32_t MyHp = S.Units[Soldier].MaxHp > 0 ? S.Units[Soldier].MaxHp : 1;
        const int32_t Mult = S.Cv.CounterMultiplier > 0 ? S.Cv.CounterMultiplier : 1;
        // Our soldier counters theirs by construction, so it lands the multiplier and they do not —
        // that asymmetry is most of why committing can ever be right.
        constexpr int64_t Scale = 1024;
        // The multiplier applies only if our type ACTUALLY beats theirs — before an enemy army exists
        // Soldier defaults to Rock against a presumed Rock, and claiming a 3x counter bonus in a
        // mirror inflated our kill rate threefold in exactly the situation the AI over-commits in.
        const int64_t MyMult = UnitTable[Soldier].Beats == FoeType ? Mult : 1;
        const int64_t FoeMult = UnitTable[FoeType].Beats == Soldier ? Mult : 1;
        const int64_t Alpha = static_cast<int64_t>(S.Units[Soldier].Attack) * MyMult * Scale / FoeHp;
        const int64_t Beta = static_cast<int64_t>(S.Units[FoeType].Attack) * FoeMult * Scale / MyHp;
        // A cart's cycle is dig + walk; the walk varies with camp placement, so approximate it as
        // comparable to the dig. Erring SHORT is deliberate — it overestimates their income, which
        // makes the AI commit less readily, and a missed attack is cheaper than a lost army.
        const int64_t Cycle = 2 * (S.Cv.DigTicks > 0 ? S.Cv.DigTicks : 1);
        const int64_t FoeCost = S.Units[FoeType].Cost > 0 ? S.Units[FoeType].Cost : 1;
        const int64_t RhoFunded = static_cast<int64_t>(FoeWorkers) * CarryCapacity * Scale / (Cycle * FoeCost);
        const int64_t RhoBuilt = static_cast<int64_t>(FoeCombatBldg) * Scale / Bt;
        const int64_t RhoB = RhoFunded > RhoBuilt ? RhoFunded : RhoBuilt;   // they will buy the capacity
        const int64_t RhoA = static_cast<int64_t>(MyCombatBldg) * Scale / Bt;
        // FIGHT THE ARMY THAT WILL BE THERE, not the one that is there now. An attack does not land
        // for WalkTicks (~34s at the opening frontier) and the defender reinforces for every one of
        // them, while our own new units have the same walk ahead of them and arrive as a trickle.
        // Without this the test is a snapshot: it saw 64 of ours against 0 of theirs and committed,
        // when what actually met the attack was several hundred units their economy funded during
        // the walk. This term is the whole difference between the previous two attempts and this one.
        const int64_t FoeAtContact =
            EnemyArmy + RhoB * (WalkTicks > 0 ? WalkTicks : 0) / Scale;
        const int64_t U = Alpha * MySoldiers - RhoB;
        const int64_t V = Beta * FoeAtContact - RhoA;
        OutproducesThem = (Beta * U * U > Alpha * V * V) ? (U > 0) : (V <= 0);
    }
    if (MyMiners < K.OpenWorkers) {
        State_ = EState::Opening;
    } else if (Tier_ == EAiTier::Hard) {
        // AllinLead stays as a floor: a model saying "you win" on a two-unit edge is still not a
        // reason to switch the economy off.
        State_ = (OutproducesThem && MySoldiers - EnemyArmy >= K.AllinLead && MySoldiers > 0)
                     ? EState::AllIn
                     : (EnemyArmy > 0 && WaveLanding) ? EState::Reacting : EState::Building;
    } else if (MySoldiers - EnemyArmy >= K.AllinLead + Incoming && MySoldiers > 0) {
        State_ = EState::AllIn;              // can out-kill their production -> commit
    } else if (EnemyArmy > 0 && WaveLanding) {
        State_ = EState::Reacting;           // the wave is landing -> answer it
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
    // A CART IS ONLY WORTH BUYING WHILE THERE IS GOLD TO DIG. Mines are finite (#84), so a match
    // runs out of ore long before it runs out of time — and every knob that asks for miners
    // (WorkerTarget, OpenWorkers, the EconFloor rebuy) is a COUNT, blind to whether there is
    // anything left to count against. Once the map is dug out those knobs keep buying carts that
    // walk to nothing, out of the same purse the army is fought with. The owner's recordings show
    // the tail of it plainly: the AI carried 500+ carts into the endgame against ~250 of his, and
    // the surplus bought him the match rather than it.
    //
    // Checked EVERY tick and not latched: it can only ever flip live->dead (mines never refill), so
    // there is no flapping to guard against, and reading it fresh means a razed-then-rebuilt economy
    // still gets the right answer. Cheap — NumMines is 48, against the O(units) scan just above.
    bool GoldLeft = false;
    for (int32_t M = 0; M < NumMines && !GoldLeft; ++M)
        if (S.MineGold[M] > 0) GoldLeft = true;
    uint8_t Want = UnitMiner;
    switch (State_) {
        case EState::Opening: Want = UnitMiner; break;
        case EState::AllIn:
            Want = MyMiners < EconFloor ? static_cast<uint8_t>(UnitMiner) : Soldier;
            break;
        case EState::Building: {
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
            // "Opportunistic" has to mean out of SURPLUS, so require enough for the combat building
            // AND the next camp. Keyed on the combat building's price alone it stopped being
            // opportunistic the moment one cost less than a camp (rock 1000 vs camp 600 under the
            // owner's tunables): the floor then won every tick and the economy never expanded at all
            // — 47 carts from a single camp, with the army built on top of an economy that never grew.
            const int32_t FloorAndCamp =
                BuildingCostFor(S.Cv, Soldier) + BuildingCostFor(S.Cv, UnitMiner);
            Want = (MyCombatBldg < K.DefenceFloor && S.Teams[MyTeam_].Gold >= FloorAndCamp)
                       ? Soldier
                       : (MyMiners < K.WorkerTarget) ? static_cast<uint8_t>(UnitMiner) : Soldier;
            break;
        }
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
    // ...and with the map dug out, every "I want a miner" above becomes "I want a soldier". Applied
    // once, here, rather than inside each of the four states: the states disagree about WHY they
    // want a cart, but none of them has a reason that survives there being no ore. Soldier is always
    // a legal want — the placement code below falls through to queueing units when it cannot place —
    // so this can never deadlock the tier the way an outright "produce nothing" would.
    if (!GoldLeft && Want == UnitMiner) Want = Soldier;

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
    // Top rung only, like the mix knobs: the lower rungs' ordering was measured with the home-lattice
    // camp fallback live. Read once here, used by both placement sites below.
    const bool CampOnGold = S.Cv.AiCampOnGold != 0 && Tier_ == EAiTier::Hard;

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
            // Same on-gold rule as the main placement below — clusters are combat-only in practice
            // (the commit sets ClusterType_ only for Want != UnitMiner), so this is here to keep the
            // two placement sites saying the same thing rather than because it fires.
            if (!Have && CampOnGold && ClusterType_ == UnitMiner &&
                AiWorkedMineTarget(S, MyTeam_, Tx, Ty))
                Have = AiPlaceNear(S, MyTeam_, ClusterType_, Tx, Ty, X, Y);
            if (!Have) Have = AiPlaceSpot(S, MyTeam_, ClusterType_, X, Y);
            if (Have) {
                Out[Count++] = InputEvent::Place(MyTeam_, ClusterType_, X, Y);
                --ClusterLeft_;
                return;
            }
            ClusterLeft_ = 0;   // nowhere legal left for this type; stop pretending
        }
    }

    // 1c. FOLLOW THE ORE (owner, 2026-08-15: "still no new mine camp gets placed next to every row of
    // gold their carts mine from"). Independent of Want, and that independence is the entire fix.
    //
    // Every other camp placement below hangs off wanting MINERS — WorkerTarget, the econ floor, the
    // opening. Hard meets WorkerTarget early and then wants soldiers for the rest of the match, so at
    // exactly the moment its home rows run dry and its carts walk forward to the next row, the one
    // decision that could give them a local drop-off has switched itself off. The carts keep hauling
    // the full distance back to camps standing over exhausted ground, for the rest of the game.
    //
    // The trigger is the haul itself, which is the thing being complained about: if a deposit our carts
    // are working is further than rps.ai.camp_haul_max from its nearest camp, buy a camp there — even
    // while the AI would rather be buying soldiers. That is a 600-gold purchase that pays for itself in
    // travel, and it is what makes the economy MIGRATE with the ore instead of staying where it opened.
    //
    // Deliberately placed after the build cluster (a committed counter wave still completes first) and
    // before the Want-driven purchase, so it outranks ordinary expansion but never an emergency.
    // Cap-respecting and affordability-checked like any other placement; one event, then return.
    // It fires the moment a cart COMMITS to a deposit — Target is set when it retargets, before it has
    // walked there — so the camp is usually going up while the first cart is still crossing. It cannot
    // do better than that: see AiWorkedMineTarget on why claiming a row BEFORE reaching it is not
    // available to the AI at all.
    if (CampOnGold && S.Cv.AiCampHaulMax > 0 &&
        (K.MaxBuildings <= 0 || MyBuildings < K.MaxBuildings) &&
        S.Teams[MyTeam_].Gold >= BuildingCostFor(S.Cv, UnitMiner)) {
        Fixed X, Y, Tx, Ty;
        if (AiWorkedMineTarget(S, MyTeam_, Tx, Ty, S.Cv.AiCampHaulMax) &&
            AiPlaceNear(S, MyTeam_, UnitMiner, Tx, Ty, X, Y)) {
            Out[Count++] = InputEvent::Place(MyTeam_, UnitMiner, X, Y);
            return;
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
    const int32_t Price = BuildingCostFor(S.Cv, Want);
    const int32_t Gold = S.Teams[MyTeam_].Gold;
    // PER-TIER batch size now (K.QueueDepth), not the shared rps.ai.queue_depth. The shared value
    // was the single biggest reason easy buried a beginner: batching 8 at a time against a
    // first-timer issuing ~10 queue commands a match is a 20x throughput gap in decisions alone.
    // Carts use the SHALLOW miner depth, soldiers the tier's own depth. Keeping every camp ticking
    // over on a short queue while the surplus banks toward another camp is the whole opening.
    // Carts use the tier's own miner batch when it sets one, else the shared shallow depth — and
    // the same two-phase rule as soldiers, for the same stated reason: "keep queues as low as
    // possible to expand mining camps as fast as possible" while banking, then "just spam carts"
    // once committed. Before the wave a shallow cart batch is not thrift, it is TEMPO: the gold not
    // sitting in a camp's queue is the gold that buys the next camp.
    const int32_t MinerBase = K.MinerQueue > 0 ? K.MinerQueue : S.Cv.AiMinerQueueDepth;
    const int32_t MinerDepth = (K.WaveLead > 0 && WaveLanding && State_ != EState::Building)
                                   ? S.Cv.BuildingQueueMax
                                   : MinerBase;
    // Soldiers: the tier's batch normally, but MAX THE STACKS when a wave is landing. Two depths,
    // because the owner's build uses two: queues as shallow as possible while expanding (every gold
    // in a queue is gold not in another camp), then stacks maxed the moment he commits to counters.
    // One global depth cannot be both, which is why no single value measured well — 4 beat hard
    // 15/16 and 5 lost 1/16 on the same seeds, chaos rather than signal.
    const int32_t SoldierDepth = (K.WaveLead > 0 && WaveLanding && State_ != EState::Building)
                                     ? S.Cv.BuildingQueueMax
                                     : K.QueueDepth;
    const int32_t RawDepth = Want == UnitMiner ? MinerDepth : SoldierDepth;
    const int32_t Depth = RawDepth > 0 ? RawDepth : 1;
    // Which building of this type takes the batch. Soldiers: the shallowest queue, as always. CARTS at
    // the top tier: the camp with room that sits NEAREST LIVE ORE, because the camp is where the cart
    // spawns (SurveyMinerCampsByGold). Declared here rather than above the depths because the choice
    // now depends on Depth — "has room for this batch" is what keeps the work spread.
    const TypeCapacity Cap_ = (CampOnGold && Want == UnitMiner)
                                  ? SurveyMinerCampsByGold(S, MyTeam_, Depth)
                                  : SurveyType(S, MyTeam_, Want);
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
        // A CAMP tries two more things before it will take the home lattice (rps.ai.camp_on_gold):
        // reinforce a deposit our carts already work, then — if even that has no ring spot left —
        // take the legal cell NEAREST live ore instead of the first cell of a sweep that marches
        // away from it. Traced over a mirror Hard match, the two together move the last four camps
        // from y=31/34/37/40 (20-26 units from the nearest deposit) to y=46 and three at y=55, which
        // is the y=60 gold row itself — the same line the owner opens when his frontier reaches it.
        // Mean camp-to-ore distance over the match: 16.1 -> 12.0.
        if (!Have && CampOnGold && Want == UnitMiner) {
            if (AiWorkedMineTarget(S, MyTeam_, Tx, Ty))
                Have = AiPlaceNear(S, MyTeam_, Want, Tx, Ty, X, Y);
            if (!Have) Have = AiPlaceNearestOre(S, MyTeam_, Want, X, Y);
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
    // COUNTER CHEST: while contested, keep a counter BUILDING's price out of the unit queue, so
    // a composition switch is answered by placing one at the front instead of waiting out
    // income. Production is flat per building, so the building is worth far more than the four
    // or five units the same gold buys — but only if the money is there at the moment the enemy
    // changes shape. Held only while we own NO building of the type we currently counter (once
    // it stands, there is nothing to save for), and never in the Building state, where the
    // opening is supposed to spend everything on expanding.
    //
    // COMPUTED ONCE, here, and shared by the queue below AND the never-stand-idle fallback under it.
    // It used to be computed twice, identically, which is how the fallback ended up honouring the very
    // reserve it exists to escape.
    const int32_t ChestPct = K.CounterChest < 0 ? 0 : K.CounterChest;
    // CONTESTED states only. Keyed on "not Building" this deadlocked the tier at tick 0: in
    // Opening it reserved a rock building's 1000 out of an 800 purse, so it could not buy a cart,
    // never mined, and sat at 0 workers for the whole match (2399 idle ticks, measured). There is
    // nothing to counter before an enemy army exists.
    const bool SaveForCounter = ChestPct > 0 &&
                                (State_ == EState::Reacting || State_ == EState::AllIn) &&
                                SurveyType(S, MyTeam_, Soldier).Owned == 0;
    int32_t Chest = SaveForCounter ? BuildingCostFor(S.Cv, Soldier) * ChestPct / 100 : 0;
    // ...CLAMPED so it can only ever eat SURPLUS (rps.ai.chest_floor_units). Unclamped, a chest
    // denominated in a BUILDING price silently outlaws every action denominated in a UNIT price: the
    // owner's 2026-07-28 recordings caught the AI holding 3695-3990 gold — against a 4000 scissor
    // building — and emitting nothing for 16-22s, idle 45% of his fastest match. Worse, it is
    // STEERABLE: lead with Paper and the AI locks Scissor, reserves 4000, and stops playing. Leaving a
    // few carts' worth always spendable is what makes this a savings plan rather than an off switch.
    const int32_t CartPrice = S.Units[UnitMiner].Cost > 0 ? S.Units[UnitMiner].Cost : 1;
    const int32_t ChestFloor = S.Cv.AiChestFloorUnits * CartPrice;
    if (Chest > Gold - ChestFloor) Chest = Gold > ChestFloor ? Gold - ChestFloor : 0;
    const int32_t Spendable = Gold > Chest ? Gold - Chest : 0;

    if (Cap_.Slot >= 0 && Cap_.Queue < Depth) {
        const int32_t UnitCost = S.Units[Want].Cost > 0 ? S.Units[Want].Cost : 1;
        int32_t N = Depth - Cap_.Queue;
        const int32_t Room = S.Cv.BuildingQueueMax - Cap_.Queue;
        if (N > Room) N = Room;
        const int32_t Affordable = Spendable / UnitCost;
        if (N > Affordable) N = Affordable;
        if (N > 0) Out[Count++] = InputEvent::Queue(MyTeam_, Cap_.Slot, N);
    }

    // ---- NEVER STAND IDLE: if the wanted type produced no action, grow the economy instead ----
    // Measured under the owner's own tunables (--aidiag hard, 1800 ticks): the AI emitted NOTHING on
    // 1121 ticks in Reacting and ended on 3930 unspent gold with 15 workers and 2 buildings. The
    // cause is a dead end, not poverty: it wants the counter to what it sees, owns no building of
    // that type — so there is nothing to queue at — and cannot yet afford the first one, which under
    // these costs can be 4000 (scissor). So it saved, and did nothing at all, for two minutes.
    //
    // A player in that position keeps spamming carts while the bank fills, which is exactly what the
    // owner described. Carts are the cheapest thing on the board and they PAY FOR the building being
    // saved for, so this is strictly better than silence.
    //
    // It shares the CLAMPED chest computed above, and the clamp is what makes this branch work at all.
    // The reserve was previously recomputed here unclamped, so the escape hatch was gated on the same
    // condition it was escaping: with a 4000 chest and a 50 cart, `Spendable/CartCost` was 0 and the
    // fallback emitted nothing. A fallback that respects the thing it is meant to bypass is not a
    // fallback. A tier with chest 0 still simply spends it all, which is what the lower rungs do.
    // ...but under a MIX, spend the surplus on a soldier we CAN make before falling back to carts
    // (#158). The plan is allowed to want a building it cannot yet afford — that is what the chest is
    // for — and while it saves, this branch keeps the army growing in the next-most-starved type
    // instead of going quiet. Out of Spendable, so the reserve it is saving is still untouchable.
    // Skipped when Want is already queueable (Count != 0) and when the mix is off, so every measured
    // lower rung is untouched.
    if (Count == 0 && Want != UnitMiner && Mixing) {
        int32_t Slot = -1, Queued = 0;
        const uint8_t Alt = BestQueueableSoldier(S, MyTeam_, MixShare_, Spendable, Depth, Slot, Queued);
        if (Alt != UnitNone && Alt != Want) {
            const int32_t UnitCost = S.Units[Alt].Cost > 0 ? S.Units[Alt].Cost : 1;
            int32_t N = Depth - Queued;
            const int32_t Room = S.Cv.BuildingQueueMax - Queued;
            if (N > Room) N = Room;
            const int32_t Affordable = Spendable / UnitCost;
            if (N > Affordable) N = Affordable;
            if (N > 0) Out[Count++] = InputEvent::Queue(MyTeam_, Slot, N);
        }
    }
    // GoldLeft gates this too, and it is the branch that most needed it: this is the never-stand-idle
    // hatch, so with the map dug out it was the one place guaranteed to keep converting the whole bank
    // into carts every tick it could not afford a soldier. "Cheapest thing on the board" stops being
    // an argument once the thing it buys cannot earn.
    if (Count == 0 && Want != UnitMiner && GoldLeft) {
        const TypeCapacity Camps = CampOnGold ? SurveyMinerCampsByGold(S, MyTeam_, MinerDepth)
                                              : SurveyType(S, MyTeam_, UnitMiner);
        if (Camps.Slot >= 0 && Camps.Queue < MinerDepth) {
            int32_t N = MinerDepth - Camps.Queue;
            const int32_t Room = S.Cv.BuildingQueueMax - Camps.Queue;
            if (N > Room) N = Room;
            const int32_t Affordable = Spendable / CartPrice;
            if (N > Affordable) N = Affordable;
            if (N > 0) Out[Count++] = InputEvent::Queue(MyTeam_, Camps.Slot, N);
        }
    }
}

}  // namespace Rps
