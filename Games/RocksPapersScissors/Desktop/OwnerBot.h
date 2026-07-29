#pragma once
// Rps::OwnerBot — a scripted sparring partner that plays the OWNER'S line, extracted from the 64
// flight recordings of 2026-07-28 (see .claude/Documents/interviews/rps-impossible-ai-2026-07-28/).
//
// WHY THIS EXISTS. The tier target is "the owner wins about 1 in 10", and no existing harness can
// see that number. --aivs measures the AI against another AI, and --aibeginner against someone who
// places one camp and then does nothing; neither plays remotely like the person who beat the top
// tier 31-0. Worse, --aivs actively MISLEADS on composition work: the whole point of a best-response
// policy is to remove a handle a HUMAN steers, and two AIs both playing argmax never steer each
// other, so the harness sees only the change's costs. Trying to tune against it cost one reverted
// change already (negative-result-best-response.md). This is the missing opponent.
//
// WHICH LINE IT PLAYS. Not the owner's earlier macro boom (320-350s wins, ~28 camps, first soldier
// building at t1750) but the FAST line he converged on between 21:09 and 21:57, which won in
// 159-208s with a THIRD of the economy and a FIFTH of the army:
//
//     ~14 mining camps, pushed forward (his camps reached depth 121 at p90; the AI's stopped at 37)
//     first soldier building at t970-1180, not t1750
//     genuinely mixed composition — rock 5.8 / paper 5.3 / scissor 4.6 buildings across the batch
//     +1 queue batches while expanding, +5 once the economy is strong
//     ~1.9 input events/second, ~3.5 units per queue command
//
// It is a STRATEGY RE-IMPLEMENTATION, not an event replay, and it has to be: recorded queue events
// carry a building SLOT index, slots are shared across both teams in one array, so replaying his
// events against a differently-placed AI points them at the wrong buildings (MatchRecord.h says so).
//
// It is deliberately NOT adaptive. A fixed opponent is what makes an AI change measurable — if both
// sides move, a win-rate delta cannot be attributed. It plays its line and lets the AI answer it.
#include <cstdint>

#include "Rps/Sim.h"
#include "Rps/Tunables.h"

namespace Rps {

class OwnerBot {
public:
    // Measured from the fast line. Public so a harness can sweep them — "how much worse does he have
    // to play before the AI wins 90%" is the tuning question this whole thing exists to answer.
    struct Params {
        int32_t CampTarget = 14;        // mining camps he ends the fast matches with
        int32_t ArmyStartTick = 1000;   // his first soldier building lands t970-1180
        int32_t ActEveryTicks = 5;      // ~2 events/s, against his measured 1.9
        int32_t EarlyBatch = 1;         // "+1 while building economy" — gold in a queue is gold not
        int32_t LateBatch = 5;          //   buying the next camp; +5 once income outruns capacity
        int32_t SoldierBuildings = 12;  // cap; the fast wins used 4-11, the boom ones up to 39
    };

    void Init(uint8_t Team, const Params& P) {
        Team_ = Team;
        P_ = P;
        Acted_ = 0;
        Rot_ = 0;
    }

    // One tick's events, same interface and same two verbs the AI has. Never emits more than one
    // event, and only on one tick in ActEveryTicks, so it cannot out-click a human.
    void DecideEvents(const Sim& S, uint32_t Tick, InputEvent* Out, int Cap, int& Count) {
        Count = 0;
        if (Cap < 1) return;
        if (P_.ActEveryTicks > 1 && (Tick % static_cast<uint32_t>(P_.ActEveryTicks)) != 0) return;

        int32_t Camps = 0, Soldiers = 0;
        int32_t ShallowCamp = -1, ShallowCampQ = 0;
        int32_t ShallowSoldier = -1, ShallowSoldierQ = 0;
        for (int32_t I = 0; I < S.Count; ++I) {
            if (!S.IsAlive(I) || !S.IsBuilding(I) || S.IsHomeBase(I) || S.Team[I] != Team_) continue;
            if (S.Type[I] == UnitMiner) {
                ++Camps;
                if (ShallowCamp < 0 || S.Queue[I] < ShallowCampQ) { ShallowCamp = I; ShallowCampQ = S.Queue[I]; }
            } else {
                ++Soldiers;
                if (ShallowSoldier < 0 || S.Queue[I] < ShallowSoldierQ) {
                    ShallowSoldier = I; ShallowSoldierQ = S.Queue[I];
                }
            }
        }
        const int32_t Gold = S.Teams[Team_].Gold;
        const bool ArmyTime = static_cast<int32_t>(Tick) >= P_.ArmyStartTick;

        // 1. ECONOMY FIRST, and keep expanding even after the army starts — the recordings show his
        //    camp count still climbing while he fights. A camp goes ON a deposit: carts deposit at the
        //    nearest own camp, so camp position IS income, and this is the single clearest difference
        //    from the AI (his camps reach midfield, the AI's stop at depth 37).
        if (Camps < P_.CampTarget && Gold >= BuildingCostFor(S.Cv, UnitMiner)) {
            Fixed X, Y;
            // ...with a home-grid fallback, and it is load-bearing rather than defensive. The mine
            // rows are DENSE (24 deposits per team across a 34-wide map), so with mine_clearance 6
            // there is frequently no legal cell anywhere beside the nearest deposit — and without a
            // fallback the bot placed nothing, ever, which also froze the AI, because the solo path
            // gates it on the player committing a camp. Both sides sat at zero for 16 matches.
            if (PlaceOnFreshMine(S, X, Y) || PlaceHomeGrid(S, UnitMiner, X, Y)) {
                Out[Count++] = InputEvent::Place(Team_, UnitMiner, X, Y);
                return;
            }
        }

        // 2. ARMY, on the clock rather than on a reaction. He does not wait to be threatened — the
        //    fast line commits at a fixed time, which is why the AI's wave-ETA logic never sees it
        //    coming. Types ROTATE so the composition stays mixed: a mixed army is what makes a single
        //    counter wrong, and it is the thing the AI's argmax cannot answer.
        if (ArmyTime && Soldiers < P_.SoldierBuildings) {
            // Paper-led, then rock, then scissor — his measured mix (5.8 / 5.3 / 4.6) with paper first.
            static const uint8_t Order[3] = {UnitPaper, UnitRock, UnitScissor};
            const uint8_t Want = Order[Rot_ % 3];
            if (Gold >= BuildingCostFor(S.Cv, Want)) {
                Fixed X, Y;
                if (PlaceForward(S, Want, X, Y) || PlaceHomeGrid(S, Want, X, Y)) {
                    ++Rot_;
                    Out[Count++] = InputEvent::Place(Team_, Want, X, Y);
                    return;
                }
            }
        }

        // 3. QUEUE. Batch size is the owner's two-phase rule, and it is the correction he gave when I
        //    first read his +1 as a defect: "+1 when building economy to keep cart production rolling
        //    but still having money for new minecart buildings as early as possible. Stacking unit
        //    queues is bad for expansion during the first minute."
        const int32_t Batch = ArmyTime ? P_.LateBatch : P_.EarlyBatch;
        const int32_t Slot = (ArmyTime && ShallowSoldier >= 0) ? ShallowSoldier : ShallowCamp;
        if (Slot < 0) return;
        const uint8_t Ty = S.Type[Slot];
        const int32_t UnitCost = S.Units[Ty].Cost > 0 ? S.Units[Ty].Cost : 1;
        int32_t N = Batch;
        const int32_t Room = S.Cv.BuildingQueueMax - S.Queue[Slot];
        if (N > Room) N = Room;
        // Hold back a camp's price while still expanding — that is the whole point of the +1 phase.
        //
        // CLAMPED to surplus, for the same reason the AI's counter chest now is, and I wrote the bug
        // here before noticing I had just fixed it there: after the opening camp the purse is 200
        // against a 600 reserve, so Spendable was 0 and the bot never queued a single cart. It ended
        // every match with one building and zero workers. A reserve larger than your whole purse is
        // not a savings plan, it is a halt — whoever writes it.
        int32_t Reserve = (Camps < P_.CampTarget) ? BuildingCostFor(S.Cv, UnitMiner) : 0;
        const int32_t Float = 4 * UnitCost;
        if (Reserve > Gold - Float) Reserve = Gold > Float ? Gold - Float : 0;
        const int32_t Spendable = Gold > Reserve ? Gold - Reserve : 0;
        const int32_t Affordable = Spendable / UnitCost;
        if (N > Affordable) N = Affordable;
        if (N > 0) Out[Count++] = InputEvent::Queue(Team_, Slot, N);
    }

private:
    // The NEAREST live deposit no camp of ours already serves, then the first legal spot beside it.
    // Nearest-first (not furthest-forward) is what the AI learned the hard way: the frontier follows
    // your frontmost survivor, so a "forward" mine can be one a lone scout made briefly legal, and a
    // camp planted there dies with the carts working it.
    bool PlaceOnFreshMine(const Sim& S, Fixed& OX, Fixed& OY) const {
        const int32_t Served = S.Cv.AiMineServedRadius.ToInt();
        const Fixed Limit = Team_ == 0 ? S.FrontierT0 : S.FrontierT1;
        int32_t Best = -1;
        Fixed BestDepth{0};
        for (int32_t M = 0; M < NumMines; ++M) {
            if (S.MineGold[M] <= 0) continue;
            if (Team_ == 0 ? S.MineY[M] > Limit : S.MineY[M] < Limit) continue;
            bool Taken = false;
            for (int32_t I = 0; I < S.Count && !Taken; ++I) {
                if (!S.IsAlive(I) || !S.IsBuilding(I) || S.Team[I] != Team_) continue;
                if (S.Type[I] != UnitMiner || S.IsHomeBase(I)) continue;
                const int32_t Dx = (S.PosX[I] - S.MineX[M]).ToInt();
                const int32_t Dy = (S.PosY[I] - S.MineY[M]).ToInt();
                if ((Dx < 0 ? -Dx : Dx) <= Served && (Dy < 0 ? -Dy : Dy) <= Served) Taken = true;
            }
            if (Taken) continue;
            const Fixed Depth = Team_ == 0 ? S.MineY[M] : WorldHeight - S.MineY[M];
            if (Best < 0 || Depth < BestDepth) { BestDepth = Depth; Best = M; }
        }
        if (Best < 0) return false;
        return RingSearch(S, UnitMiner, S.MineX[Best].ToInt(), S.MineY[Best].ToInt(), OX, OY);
    }

    // Soldier buildings go FORWARD — just behind our own leading edge, so what they produce spawns
    // where the fighting is instead of walking the length of the map. His soldier buildings sat at
    // median depth 102-132 against the AI's 66-106.
    bool PlaceForward(const Sim& S, uint8_t Type, Fixed& OX, Fixed& OY) const {
        const Fixed Frontier = Team_ == 0 ? S.FrontierT0 : S.FrontierT1;
        const int32_t Setback = S.Cv.AiFrontSetback.ToInt();
        int32_t Y = Team_ == 0 ? Frontier.ToInt() - Setback : Frontier.ToInt() + Setback;
        const int32_t Floor = S.Cv.InitialFrontier.ToInt();
        if (Team_ == 0) { if (Y < Floor) Y = Floor; }
        else { if (Y > WorldHeight.ToInt() - Floor) Y = WorldHeight.ToInt() - Floor; }
        return RingSearch(S, Type, WorldWidth.ToInt() / 2, Y, OX, OY);
    }

    // Sweep our own half front-to-back for any legal cell. The last resort when no spot beside a
    // deposit is legal — which on a dense mine row is the common case, not the exception.
    bool PlaceHomeGrid(const Sim& S, uint8_t Type, Fixed& OX, Fixed& OY) const {
        const int32_t Dir = Team_ == 0 ? 1 : -1;
        const int32_t Base = Team_ == 0 ? 4 : WorldHeight.ToInt() - 4;
        for (int32_t R = 0; R < WorldHeight.ToInt() / 2; R += 2) {
            const int32_t Y = Base + Dir * R;
            if (Y < 2 || Y > WorldHeight.ToInt() - 2) break;
            for (int32_t X = 4; X <= WorldWidth.ToInt() - 4; X += 2) {
                if (!S.CanPlaceBuilding(Team_, Type, F(X), F(Y))) continue;
                OX = F(X); OY = F(Y);
                return true;
            }
        }
        return false;
    }

    // Nearest-first integer ring outward from a target; first spot the sim accepts wins. Same shape
    // as the AI's, kept separate on purpose — this is a different player, not a copy of the opponent.
    bool RingSearch(const Sim& S, uint8_t Type, int32_t Tx, int32_t Ty, Fixed& OX, Fixed& OY) const {
        static const int32_t Dx[] = {0, 3, -3, 0, 0, 3, -3, 3, -3, 6, -6, 0, 0, 7, -7, 0, 0,
                                     9, -9, 0, 0, 12, -12, 0, 0, 6, -6, 6, -6, 9, -9, 9, -9};
        static const int32_t Dy[] = {0, 0, 0, 3, -3, 3, 3, -3, -3, 0, 0, 6, -6, 0, 0, 7, -7,
                                     0, 0, 9, -9, 0, 0, 12, -12, 6, 6, -6, -6, 9, 9, -9, -9};
        constexpr int32_t N = static_cast<int32_t>(sizeof(Dx) / sizeof(Dx[0]));
        for (int32_t I = 0; I < N; ++I) {
            const int32_t X = Tx + Dx[I], Y = Ty + Dy[I];
            if (X < 2 || X > WorldWidth.ToInt() - 2 || Y < 2 || Y > WorldHeight.ToInt() - 2) continue;
            if (!S.CanPlaceBuilding(Team_, Type, F(X), F(Y))) continue;
            OX = F(X); OY = F(Y);
            return true;
        }
        return false;
    }

    uint8_t  Team_ = 0;
    Params   P_{};
    uint32_t Acted_ = 0;
    uint32_t Rot_ = 0;
};

}  // namespace Rps
