#pragma once
#include <cstdint>

#include "Lur/Core/CVar.h"
#include "Lur/Sim/Fixed.h"
#include "Lur/Sim/FixedString.h"  // Fixed<->string codec for CVar<Fixed> (ADL)

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
using Lur::Core::CVarFlagAffectsGameplay;  // #112: gameplay-CVar sync boundary

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
// Playtest 2026-07-20: speeds LOWERED (carts 0.4, warriors 0.5) so the flocking reads
// as a slow, viscous lava-lamp flow rather than a sprint.
// Playtest 2026-07-20: attack RANGE is now UNIFORM across all unit types (= Paper's F(2))
// so no type out-reaches another — engagement distance is identical, the counter triangle
// decides fights, not reach. (Miner range is unused; carts don't fight.)
constexpr UnitStats UnitTable[UnitCount] = {
    // Cost Build  HP  Speed        Atk Range    CD  Beats
    {  30,   30,  40, F(4, 10),      2, F(2),       8, UnitNone   }, // Miner (cart)
    {  50,   50,  60, F(5, 10),      8, F(2),      10, UnitScissor}, // Rock thrower
    {  50,   50,  90, F(5, 10),      9, F(2),      10, UnitRock   }, // Paper wrapper
    {  50,   50,  45, F(5, 10),      7, F(2),       6, UnitPaper  }, // Scissor cutter
};

LUR_CVAR(CvCounterMultiplier, "rps.combat.counter_mult", 3, CVarFlagAffectsGameplay, "Combat");   // attacker vs the type it beats
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
// zero the mine is GONE (skipped by targeting, hidden by the view).
// #108 (2026-07-20 playtest): x20 to 6000 (= 400 trips) paired with the SPARSE clustered
// layout below — few, rich, long-lived deposits. A cart settles on a nearby mine and digs
// it for a long time (little travel), so the economy ramps and doesn't deflate, WITHOUT a
// battlefield cluttered by hundreds of mines (which also made the sim/gate crawl).
// Total map gold still bounds the whole economy — starvation keeps the lose rule reachable.
constexpr int32_t MineGoldCapacity = 6000;

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

// ---- Movement / steering (spec §5; boids slice A, #96) ----
// SOLDIERS flock: one neighbour gather blends separation + enemy separation + two-tier
// cohesion into a desired step (Sim.cpp Movement). Miners keep their state machine with
// a separation + mine-repel nudge. ALL values below are playtest PLACEHOLDERS (plan §7)
// to be beaten into shape on the desktop stress scene — the engine is never the reason a
// number stays small. Nothing here is wire-visible (compiled identically into both peers;
// a change is lockstep-breaking — same build both sides — but NOT a wire-format change).
//
// Separation now uses the CORRECTED boids falloff: strongest at contact, zero at the
// radius — dir_cheb × (R − cheb)/R × strength (the old form grew with distance, so
// stacking was nearly free; that was the bundle's root cause, plan §1.3).
// Separation must WIN at short range so units stay visibly spaced (playtest 2026-07-20:
// weak separation let cohesion compress the blob into an unreadable mush). Strong push +
// wider radius = a school-of-fish lattice: grouped, but every unit has its own space.
constexpr Fixed SeparationRadius = F(24, 10);      // same-team keep-apart range (a touch wider, 2026-07-20)
LUR_CVAR(CvSeparationStrength, "rps.boid.sep_strength", F(3, 2), CVarFlagAffectsGameplay, "Flocking");      // > cohesion at contact — sets the spacing
// Enemy separation (new, #96 decision #2): a wider radius / stronger push un-piles engaged
// fights into arcs instead of cross-team pixel-piles. Soldiers only (miners ignore combat).
constexpr Fixed EnemySeparationRadius = F(3, 2);
LUR_CVAR(CvEnemySeparationStrength, "rps.boid.enemy_sep_strength", F(1), CVarFlagAffectsGameplay, "Flocking");
// Two-tier cohesion (soldiers only) — THE readability mechanism. Toward the same-type
// centroid (tight: papers blob with papers) plus a weaker pull toward the whole army's
// warrior centroid (so type-blobs travel loosely together, not scattered).
//
// LAVA-LAMP tuning pass (2026-07-20 playtest): cohesion raised well above the slice-B
// starters so the group moves as a viscous glob — a front-runner's local centroid sits
// BEHIND it, so cohesion pulls it back (it "waits"); a trailing unit has cohesion + seek
// aligned, so it closes the gap. Cohesion self-limits (∝ distance-to-centroid), so a
// tight blob still marches; it only bites hard when the blob starts to stretch.
// GROUP-UP pass (2026-07-20 playtest): same-type cohesion reaches FAR to find teammates
// across the field, but pulls GENTLY (a soft, wide gather rather than a hard clump) — a
// lone spawn drifts toward its type over distance without the group compressing to mush.
constexpr Fixed CohSameR = F(15);                  // same-type affinity radius (wide — find distant teammates)
LUR_CVAR(CvWCohSame, "rps.boid.w_coh_same", F(1, 3), CVarFlagAffectsGameplay, "Flocking");                //   weight (gentle — soft pull, not a hard clump)
constexpr Fixed CohAllR = F(9);                    // cross-type army affinity radius (weak pull, below)
// Cross-type army cohesion is SUPER TINY (2026-07-20 playtest): types shouldn't want to
// pile onto each other — same-type globs are the readable unit; the whole-army pull is a
// barely-there nudge so they don't scatter to opposite corners.
LUR_CVAR(CvWCohAll, "rps.boid.w_coh_all", F(1, 64), CVarFlagAffectsGameplay, "Flocking");                //   weight (≪≪ WCohSame — barely noticeable)
LUR_CVAR(CvWSeek, "rps.boid.w_seek", F(1), CVarFlagAffectsGameplay, "Flocking");                      // goal-pursuit weight (unit direction)
// Predator flee (2026-07-20 playtest): a unit must NEVER steer toward the enemy type it
// is weak against (the type that beats it). A repulsion from that predator, larger radius
// than enemy separation, corrected falloff (strongest at contact). Chases prey, flees the
// counter — so the RPS triangle plays out spatially, not just in the damage numbers.
constexpr Fixed PredatorFleeR = F(7);
LUR_CVAR(CvWPredatorFlee, "rps.boid.w_predator_flee", F(1, 4), CVarFlagAffectsGameplay, "Flocking");           // subtle drift away (playtest 2026-07-20: nudged up a
                                                   //   little); still < WSeek so hunting prey dominates
// Organic wander (2026-07-20 playtest): a slow, smooth per-unit noise offset added to the
// steer — the deterministic fixed-point analog of Simplex/OpenSimplex noise (value noise
// with a smoothstep fade; no floats, no libs). WNoise is its amplitude; NoiseTimeScale is
// ticks→lattice (smaller = slower, smoother drift).
LUR_CVAR(CvNoiseTimeScale, "rps.boid.noise_time_scale", F(1, 12), CVarFlagAffectsGameplay, "Flocking");         // ~1.2 s per noise lattice cell at 10 Hz
LUR_CVAR(CvWNoise, "rps.boid.w_noise", F(2, 5), CVarFlagAffectsGameplay, "Flocking");                  // wander amplitude (world-units-ish of pull)
// Slice B (#97) — FLOW: momentum via implicit velocity Δ = Pos − Prev (fixed tick, so
// last tick's displacement IS the velocity — no VelX/VelY arrays). The finalize does
// NewPos = Pos + Damp·Δ + ChebClamp(desired − Δ, MaxAccel), then clamps the step to
// Speed. Alignment steers a soldier toward its same-type neighbours' average velocity.
// Lava-lamp: slower turns (MaxAccel down) + more glide (Damp up) = the viscous feel.
constexpr Fixed AlignR = F(5);                     // same-type alignment radius (< CohAllR gather)
LUR_CVAR(CvWAlign, "rps.boid.w_align", F(1, 4), CVarFlagAffectsGameplay, "Flocking");                  //   weight (match neighbour heading — laminar flow)
LUR_CVAR(CvMaxAccel, "rps.boid.max_accel", F(10, 100), CVarFlagAffectsGameplay, "Flocking");             // per-tick turn/accel clamp (gloopy, ≈0.7 s to reach Speed)
LUR_CVAR(CvFlockDamping, "rps.boid.flock_damping", F(9, 10), CVarFlagAffectsGameplay, "Flocking");           // carried-Δ retention in free flight (viscous glide)
LUR_CVAR(CvInRangeDamping, "rps.boid.inrange_damping", F(1, 2), CVarFlagAffectsGameplay, "Flocking");          // stronger decay when engaged — no orbiting the target
// Slice C (#98) — guard-lite INTERPOSE: an enemy soldier within GuardAlertR of one of MY
// miners is a RAIDER. A defender that has BOTH a friendly cart and a flagged raider within
// InterposeR steers to the point BETWEEN them — screening the cart (even from a predator it
// wouldn't attack). Positioning, not targeting: it keeps raiders off the economy by body.
constexpr Fixed GuardAlertR = F(6);                // raider = enemy soldier this close to a cart
constexpr Fixed InterposeR = F(12);                // defender reacts to carts/raiders within this (< FlockGatherR)
LUR_CVAR(CvWInterpose, "rps.boid.w_interpose", F(1), CVarFlagAffectsGameplay, "Flocking");                 // pull toward the block point (≈ WSeek)
// The single flock GATHER radius = the LARGEST force radius. One widened neighbour walk
// feeds every force (each re-tests its own smaller radius), so brute≡grid holds no matter
// which force is the widest. MUST be ≥ every radius above — derived here so raising any
// one (e.g. CohSameR) can never silently break the grid path. Also the perf hot knob.
constexpr Fixed FlockGatherR = Lur::Sim::Max(
    Lur::Sim::Max(Lur::Sim::Max(SeparationRadius, EnemySeparationRadius), Lur::Sim::Max(CohSameR, CohAllR)),
    Lur::Sim::Max(AlignR, PredatorFleeR));
// Targeting: distances quantize into bands of this width (Chebyshev units); within one
// band the TYPE-PREFERENCE ladder decides (prey > mirror > neutral > predator, Sim.cpp).
// Playtest 2026-07-20: WIDENED from 3 to 12 so the whole engagement neighbourhood is one
// band — a unit hunts the enemy type it beats even when a mirror is somewhat nearer,
// instead of just fighting whoever's closest. Beyond the band, closeness takes over again.
constexpr Fixed TargetBand = F(12);
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
// Cap the expanding-ring nearest-enemy search at this many cells (#92): two far-apart
// armies would otherwise scan every empty cell between them (O(units x separation)).
// Beyond the cap a soldier has "no target" and marches on the enemy camp instead (a
// deliberate gameplay change, decision #3 — armies advance, no straggler-chasing). The
// brute path applies the IDENTICAL Chebyshev cell-box cutoff so grid == brute holds.
// Placeholder radius (playtest): 8 cells = 24 world units.
constexpr int32_t TargetSearchMaxK = 8;

// ---- Netcode (slice 1, NOT the core) — recorded here so the constant has one home ----
constexpr int32_t InputDelayTicks = 3;      // press at T executes at T+3 (design doc §3)
// LockstepPeer::Execute drains at most this many ticks per call, so a catch-up burst
// (post-background / thermal / -O0) can't monopolize the loop and starve input -> ANR
// (#90; forensics 2026-07-19). Backlog drains over subsequent calls, never discarded.
// Mirrors SimRunner::MaxTicksPerService. Scheduling never changes results (design §3).
constexpr uint32_t MaxExecTicksPerService = 8;
// Above this start-of-call backlog Execute is "catching up": suppress the per-10-tick
// anchors and emit ONE at the frontier reached, so a burst can't flood the
// 1-outstanding-write GATT queue with stale anchors (#90; seen at 21:22 in the ANR).
constexpr uint32_t AnchorBurstThreshold = 16;

// ---- Fixed capacities (no heap in the tick; sized for the raised engine target) ----
// MaxUnitsPerTeam is the compile-time unit ceiling per side (design doc §5's
// "hundreds-to-thousands"). Slot reuse (lowest free slot) bounds live memory here.
constexpr int32_t MaxUnitsPerTeam = 2048;
constexpr int32_t MaxUnits = MaxUnitsPerTeam * 2;
// CLUSTERED mine field (#108, 2026-07-20 playtest): the dense ~480-mine grid was an
// anti-deflation experiment that also made the sim (and the CI gate) crawl. Reverted to a
// sparse clustered layout — 6 mines per row × 4 cluster rows per team (home/safe/midfield/
// contested, a risk gradient toward mid) × 2 teams = 48 — paired with the x20
// MineGoldCapacity above, so each deposit is rich and long-lived instead of the field
// being carpeted. Deflation is solved by depth-per-mine, not count.
constexpr int32_t MinesPerCluster = 6;      // mines spread across the 34-wide field per row
constexpr int32_t ClustersPerTeam = 4;      // home (at camp) / safe / midfield / contested (near mid)
constexpr int32_t MinesPerTeam = MinesPerCluster * ClustersPerTeam;
constexpr int32_t NumMines = MinesPerTeam * 2;   // 48


// ---- #112: per-tick frozen snapshot of the AffectsGameplay CVars the sim reads ----
// Copied once at the top of Sim::Step so a value is CONSTANT for the whole tick on both
// peers even if an override is applied between ticks (timing-safety half of determinism,
// spec S1). POD => lives in Sim, memcpy-able, and folds into StateHash: a mis-latch shows
// up as an immediate located desync alarm, not silent drift. Read inside the sim as
// S.Cv.Foo, never CvFoo.Get() directly (that mark = "synced gameplay value", by design).
struct CvSnapshot {
    Fixed   SeparationStrength;
    Fixed   EnemySeparationStrength;
    Fixed   WSeek;
    Fixed   WCohSame;
    Fixed   WCohAll;
    Fixed   WAlign;
    Fixed   WPredatorFlee;
    Fixed   WNoise;
    Fixed   WInterpose;
    Fixed   MaxAccel;
    Fixed   FlockDamping;
    Fixed   InRangeDamping;
    Fixed   NoiseTimeScale;
    int32_t CounterMultiplier;
};

inline CvSnapshot LatchCvs() {
    return CvSnapshot{
        CvSeparationStrength.Get(), CvEnemySeparationStrength.Get(), CvWSeek.Get(),
        CvWCohSame.Get(), CvWCohAll.Get(), CvWAlign.Get(), CvWPredatorFlee.Get(),
        CvWNoise.Get(), CvWInterpose.Get(), CvMaxAccel.Get(), CvFlockDamping.Get(),
        CvInRangeDamping.Get(), CvNoiseTimeScale.Get(), CvCounterMultiplier.Get(),
    };
}

} // namespace Rps
