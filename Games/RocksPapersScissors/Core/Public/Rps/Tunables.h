#pragma once
#include <cstdint>

#include "Lur/Sim/Fixed.h"

// RocksPapersScissors tunables — ONE table, every number a placeholder to be
// beaten into shape on the desktop build during slice 3 (design doc §8). The
// engine's job is to never be the reason a number stays small; the *values* are a
// playtest question. Nothing here is wire-visible: it's compiled into both peers
// identically, so changing a value is a lockstep-breaking change (both sides must
// run the same build) but NOT a wire-format change.
//
// Design lock 2026-07-19 (#84, Docs/Journal/2026-07-19/rps-hud-prototype.html): the economy
// is GOLD dug from FINITE MINES by MINERS, and production runs four parallel
// per-type queues whose rate scales with how deep each queue is stacked.
namespace Rps {

using Lur::Sim::Fixed;

// Compile-time Fixed builders. F(n) = whole number; F(num,den) = a rational, for
// sub-integer tunables like a 0.45-units/tick speed, evaluated with an int64
// intermediate so it's exact and constexpr.
constexpr Fixed F(int32_t V) { return Fixed::FromInt(V); }
constexpr Fixed F(int32_t Num, int32_t Den) {
    return Fixed{static_cast<int32_t>((static_cast<int64_t>(Num) << Fixed::FracBits) / Den)};
}

// ---- Unit types. Declaration order is load-bearing: it indexes UnitTable AND is
// the bit position in the input mask (bit 0 = Miner ... bit 3 = Scissor). ----
enum EUnit : uint8_t {
    UnitMiner = 0,
    UnitRock = 1,
    UnitPaper = 2,
    UnitScissor = 3,
    UnitCount = 4,
    UnitNone = 0xFF, // "beats nothing" sentinel; also "no type"
};

struct UnitStats {
    int32_t Cost;        // gold
    int32_t BuildTicks;  // spec seconds x 10 (sim is 10 Hz)
    int32_t MaxHp;
    Fixed   Speed;       // world units PER TICK (spec units/s / 10)
    int32_t Attack;      // base damage per hit
    Fixed   Range;       // attack reach (world units), compared as squared
    int32_t Cooldown;    // ticks between attacks
    uint8_t Beats;       // type this deals CounterMultiplier x to (UnitNone = none)
};

// Spec §3 table, converted to tick-denominated time (design doc §3: wallclock
// never enters the sim). Rock=ranged/slow, Scissor=fast/fragile, Paper=tanky/short.
// Playtest 2026-07-19: all WARRIORS share one speed, slightly above the miner's —
// the counter triangle reads through damage, not through chases nobody can win.
constexpr UnitStats UnitTable[UnitCount] = {
    // Cost Build  HP  Speed        Atk Range    CD  Beats
    {  30,   30,  40, F(6, 10),      2, F(12, 10),  8, UnitNone   }, // Miner
    {  50,   50,  60, F(7, 10),      8, F(6),      10, UnitScissor}, // Rock thrower
    {  50,   50,  90, F(7, 10),      9, F(2),      10, UnitRock   }, // Paper wrapper
    {  50,   50,  45, F(7, 10),      7, F(12, 10),  6, UnitPaper  }, // Scissor cutter
};

constexpr int32_t CounterMultiplier = 3;   // attacker vs the type it beats
constexpr int32_t CheapestCost = 30;       // = Miner; the win-rule rebuy floor

// ---- Economy (spec §3, gold/miner + finite mines per #84) ----
// Playtest 2026-07-19: several carts may work one deposit at once — the cap is the
// "room around it" proxy; separation steering spreads the diggers into a ring.
constexpr int32_t WorkersPerMine = 6;
constexpr int32_t DigTicks = 15;           // 1.5 s to fill a carry
constexpr int32_t CarryCapacity = 15;      // gold per round trip
constexpr int32_t StartGold = 60;
constexpr int32_t StartMiners = 3;
// A mine's total reserve. Every completed dig removes the carry from the mine; at
// zero the mine is GONE (skipped by targeting, hidden by the view). 300 = 20 trips.
// Total map gold now bounds the whole economy — starvation makes the lose rule
// genuinely reachable and match length is naturally bounded.
constexpr int32_t MineGoldCapacity = 300;

// ---- Sim rate ----
// 10 Hz (design doc §3). This is what a "tick" means in seconds: BuildTicks and
// every other duration above are wallclock/TickRateHz. The tick thread advances the
// sim at this rate via TickClock, decoupled from render/vsync (#69).
constexpr uint32_t TickRateHz = 10;

// ---- Production (#84: four PARALLEL per-type queues with stack acceleration) ----
// A press appends to that type's queue (gold deducted at enqueue). Each queue's
// build progress advances by its QUEUED COUNT per tick — i.e. rate = count x base,
// so a deep stack snowballs: effective build time = BuildTicks / count. This is the
// pacing thesis (games accelerate as economies grow) — protect it in balance passes.
constexpr int32_t PerTypeQueueCap = 64;    // sanity bound, not a gameplay knob
constexpr int32_t RingSlots = 8;           // deterministic spawn ring (SpawnCounter % RingSlots)

// ---- The field (design doc §9: portrait, width fixed, height the balance knob) ----
// PORTRAIT: short axis = width (fills the screen), long axis = height (scrollable).
// These are FIXED sim constants, identical on both peers — never a device readout.
constexpr Fixed WorldWidth = F(34);
// Taller than a phone screen so the camera actually scrolls (§9): a portrait phone
// shows ~(h/w)*WorldWidth ≈ 75 world-units tall. 240 (~3 screens of march between
// camps) per the 2026-07-19 layout review. The slice-3 balance knob for tempo.
constexpr Fixed WorldHeight = F(240);
constexpr Fixed MaxWorldHeight = F(320);    // headroom the grid arrays size to

// Camps at the two SHORT ends — team 0 bottom, team 1 top (spec §2, rotated to
// portrait). A camp is a location (spawn point + gold drop-off), never an entity.
constexpr int32_t CampInset = 6;            // camp distance in from each short end
constexpr Fixed CampX = F(17);              // centred on the 34-wide field
constexpr Fixed Camp0Y = F(CampInset);
constexpr Fixed Camp1Y = F(WorldHeight.ToInt() - CampInset);

// ---- Movement / steering (spec §5) ----
constexpr Fixed SeparationRadius = F(1);    // same-team push range
constexpr Fixed SeparationStrength = F(1, 4);
// Targeting (playtest 2026-07-19): distances quantize into bands of this width
// (Chebyshev units); within one band, the type WE counter (3x damage) is preferred
// over a marginally nearer neutral target - a paper picks the rock, not the
// scissor, when both are "about equally far".
constexpr Fixed TargetBand = F(3);
// Playtest 2026-07-19: carts RING a deposit instead of standing on it — a cart can
// dig once within MineDigRange, and live deposits push units outward (soft
// obstacles, same strength as unit separation). Repel < dig range, so diggers
// settle in the annulus between the two.
constexpr Fixed MineDigRange = F(22, 10);
constexpr Fixed MineRepelRadius = F(3, 2);

// ---- Spatial grid (design §5) — cell size in whole world units. This is a PURE
// perf knob: any value yields bit-identical results to brute force (rps_sim_tests
// proves it), so it's tuned in slice 3 without touching correctness. ~ the largest
// interaction radius per the design doc; smaller favours the dense separation query.
constexpr int32_t GridCellSize = 3;

// ---- Netcode (slice 1, NOT the core) — recorded here so the constant has one home ----
constexpr int32_t InputDelayTicks = 3;      // press at T executes at T+3 (design doc §3)

// ---- Fixed capacities (no heap in the tick; sized for the raised engine target) ----
// MaxUnitsPerTeam is the compile-time unit ceiling per side (design doc §5's
// "hundreds-to-thousands"). Slot reuse (lowest free slot) bounds live memory here.
constexpr int32_t MaxUnitsPerTeam = 2048;
constexpr int32_t MaxUnits = MaxUnitsPerTeam * 2;
constexpr int32_t MinesPerCluster = 6;      // denser rows per the 2026-07-19 review
constexpr int32_t ClustersPerTeam = 3;      // safe (near camp) / midfield / contested (near mid)
constexpr int32_t MinesPerTeam = MinesPerCluster * ClustersPerTeam;
constexpr int32_t NumMines = MinesPerTeam * 2;

} // namespace Rps
