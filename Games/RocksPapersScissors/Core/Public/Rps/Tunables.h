#pragma once
#include <cstdint>

#include "Lur/Sim/Fixed.h"

// RocksPapersScissors tunables — ONE table, every number a placeholder to be
// beaten into shape on the desktop build during slice 3 (design doc §8). The
// engine's job is to never be the reason a number stays small; the *values* are a
// playtest question. Nothing here is wire-visible: it's compiled into both peers
// identically, so changing a value is a lockstep-breaking change (both sides must
// run the same build) but NOT a wire-format change.
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
// the bit position in the input mask (bit 0 = Lumberjack ... bit 3 = Scissor). ----
enum EUnit : uint8_t {
    UnitLumberjack = 0,
    UnitRock = 1,
    UnitPaper = 2,
    UnitScissor = 3,
    UnitCount = 4,
    UnitNone = 0xFF, // "beats nothing" sentinel; also "no type"
};

struct UnitStats {
    int32_t Cost;        // wood
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
constexpr UnitStats UnitTable[UnitCount] = {
    // Cost Build  HP  Speed        Atk Range    CD  Beats
    {  30,   30,  40, F(6, 10),      2, F(12, 10),  8, UnitNone   }, // Lumberjack
    {  50,   50,  60, F(5, 10),      8, F(6),      10, UnitScissor}, // Rock thrower
    {  50,   50,  90, F(45, 100),    9, F(2),      10, UnitRock   }, // Paper wrapper
    {  50,   50,  45, F(8, 10),      7, F(12, 10),  6, UnitPaper  }, // Scissor cutter
};

constexpr int32_t CounterMultiplier = 3;   // attacker vs the type it beats
constexpr int32_t CheapestCost = 30;       // = Lumberjack; the win-rule rebuy floor

// ---- Economy (spec §3) ----
constexpr int32_t WorkersPerTree = 2;      // max simultaneous choppers on one tree
constexpr int32_t ChopTicks = 15;          // 1.5 s to fill a carry
constexpr int32_t CarryCapacity = 15;      // wood per round trip
constexpr int32_t StartWood = 60;
constexpr int32_t StartLumberjacks = 3;

// ---- Sim rate ----
// 10 Hz (design doc §3). This is what a "tick" means in seconds: BuildTicks and
// every other duration above are wallclock/TickRateHz. The tick thread advances the
// sim at this rate via TickClock, decoupled from render/vsync (#69).
constexpr uint32_t TickRateHz = 10;

// ---- Production (spec §4) ----
constexpr int32_t QueueDepth = 4;
constexpr int32_t RingSlots = 8;           // deterministic spawn ring (SpawnCounter % RingSlots)

// ---- The field (design doc §9: portrait, width fixed, height the balance knob) ----
// PORTRAIT: short axis = width (fills the screen), long axis = height (scrollable).
// These are FIXED sim constants, identical on both peers — never a device readout.
constexpr Fixed WorldWidth = F(34);
// Taller than a phone screen so the camera actually scrolls (§9): a portrait phone
// shows ~(h/w)*WorldWidth ≈ 75 world-units tall, so 120 means your camp sits at the
// bottom and you swipe up toward the enemy. The slice-3 balance knob for tempo.
constexpr Fixed WorldHeight = F(120);
constexpr Fixed MaxWorldHeight = F(160);    // headroom the grid arrays size to

// Camps at the two SHORT ends — team 0 bottom, team 1 top (spec §2, rotated to
// portrait). A camp is a location (spawn point + wood drop-off), never an entity.
constexpr int32_t CampInset = 6;            // camp distance in from each short end
constexpr Fixed CampX = F(17);              // centred on the 34-wide field
constexpr Fixed Camp0Y = F(CampInset);
constexpr Fixed Camp1Y = F(WorldHeight.ToInt() - CampInset);

// ---- Movement / steering (spec §5) ----
constexpr Fixed SeparationRadius = F(1);    // same-team push range
constexpr Fixed SeparationStrength = F(1, 4);

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
constexpr int32_t TreesPerGrove = 4;
constexpr int32_t GrovesPerTeam = 2;        // safe (near camp) + contested (near mid)
constexpr int32_t TreesPerTeam = TreesPerGrove * GrovesPerTeam;
constexpr int32_t NumTrees = TreesPerTeam * 2;

} // namespace Rps
