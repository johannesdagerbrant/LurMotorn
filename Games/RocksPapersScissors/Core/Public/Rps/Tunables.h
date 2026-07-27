#pragma once
#include <cstdint>
#include <cstring>  // GameplayIdForName (dev)

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
using Lur::Core::CVarFlagNone;             // #156: dev-only knobs (never latched/hashed/synced)

// Compile-time Fixed builders. F(n) = whole number; F(num,den) = a rational, for
// sub-integer tunables like a 0.45-units/tick speed, evaluated with an int64
// intermediate so it's exact and constexpr.
constexpr Fixed F(int32_t V) { return Fixed::FromInt(V); }
constexpr Fixed F(int32_t Num, int32_t Den) {
    return Fixed{static_cast<int32_t>((static_cast<int64_t>(Num) << Fixed::FracBits) / Den)};
}
// Rounding counterpart to F(Num, Den) — use it for any default you would naturally WRITE as a
// decimal, so the compile-time value and the typed/persisted one are the same raw.
//
// F truncates; the console + cvars.cfg decimal codec (FixedString.h) rounds to nearest. They agree
// whenever the division is exact and disagree when it isn't: F(1,10) is raw 6553, but "0.1" parses
// to 6554. A truncated default therefore can NEVER round-trip through its own console — it persists
// as "0.09999" — and a hand-edited cvars.cfg saying "0.1" silently differs from it. That is not
// theoretical: it cost one raw unit of w_predator_flee and showed up as a StateHash difference
// between the two phones (#155 promotion), visible only because the pre-match LOCKSTEP readout
// prints the hash.
constexpr Fixed FRound(int32_t Num, int32_t Den) {
    return Fixed{static_cast<int32_t>(((static_cast<int64_t>(Num) << Fixed::FracBits) + Den / 2) / Den)};
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

LUR_CVAR(CvCounterMultiplier, "rps.combat.counter_mult", 3, CVarFlagAffectsGameplay,
         "Damage multiplier when attacking the type you beat (the RPS triangle)");
constexpr int32_t CheapestCost = 30;       // = Miner; the win-rule rebuy floor

// ---- Per-type unit stats as gameplay CVars (#122). The dotted name IS the console-tree
// hierarchy: rps.unit.<type>.{cost,hp,speed,damage,build_time}. Defaults EQUAL UnitTable
// above, so StateHash is unchanged until a value is edited. Cost/Hp/Damage/BuildTime are int,
// Speed is Fixed. Range/Cooldown and the RPS Beats relation stay compile-time in UnitTable
// (Beats is wire/order-load-bearing, not a number to twiddle). The Sim latches these into
// Sim::Units[] (DeriveUnits) each tick. ----
LUR_CVAR(CvMinerCost,      "rps.unit.miner.cost",      100,      CVarFlagAffectsGameplay, "Gold to queue a miner cart");
LUR_CVAR(CvMinerHp,        "rps.unit.miner.hp",        40,       CVarFlagAffectsGameplay, "Miner hit points");
LUR_CVAR(CvMinerSpeed,     "rps.unit.miner.speed",     F(4, 10), CVarFlagAffectsGameplay, "Miner move speed (world units/tick)");
LUR_CVAR(CvMinerDamage,    "rps.unit.miner.damage",    2,        CVarFlagAffectsGameplay, "Miner attack damage per hit");
LUR_CVAR(CvMinerBuild,     "rps.unit.miner.build_time",50,       CVarFlagAffectsGameplay, "Miner build time (ticks, 10/s)");
LUR_CVAR(CvRockCost,       "rps.unit.rock.cost",       50,       CVarFlagAffectsGameplay, "Gold to queue a Rock");
LUR_CVAR(CvRockHp,         "rps.unit.rock.hp",         100,      CVarFlagAffectsGameplay, "Rock hit points");
LUR_CVAR(CvRockSpeed,      "rps.unit.rock.speed",      F(5, 10), CVarFlagAffectsGameplay, "Rock move speed (world units/tick)");
LUR_CVAR(CvRockDamage,     "rps.unit.rock.damage",     7,        CVarFlagAffectsGameplay, "Rock attack damage per hit");
LUR_CVAR(CvRockBuild,      "rps.unit.rock.build_time", 15,       CVarFlagAffectsGameplay, "Rock build time (ticks, 10/s)");
LUR_CVAR(CvPaperCost,      "rps.unit.paper.cost",      50,       CVarFlagAffectsGameplay, "Gold to queue a Paper");
LUR_CVAR(CvPaperHp,        "rps.unit.paper.hp",        50,       CVarFlagAffectsGameplay, "Paper hit points");
LUR_CVAR(CvPaperSpeed,     "rps.unit.paper.speed",     F(1),     CVarFlagAffectsGameplay, "Paper move speed (world units/tick)");
LUR_CVAR(CvPaperDamage,    "rps.unit.paper.damage",    9,        CVarFlagAffectsGameplay, "Paper attack damage per hit");
LUR_CVAR(CvPaperBuild,     "rps.unit.paper.build_time",15,       CVarFlagAffectsGameplay, "Paper build time (ticks, 10/s)");
LUR_CVAR(CvScissorCost,    "rps.unit.scissor.cost",    50,       CVarFlagAffectsGameplay, "Gold to queue a Scissor");
LUR_CVAR(CvScissorHp,      "rps.unit.scissor.hp",      80,       CVarFlagAffectsGameplay, "Scissor hit points");
LUR_CVAR(CvScissorSpeed,   "rps.unit.scissor.speed",   F(3, 4),  CVarFlagAffectsGameplay, "Scissor move speed (world units/tick)");
LUR_CVAR(CvScissorDamage,  "rps.unit.scissor.damage",  12,       CVarFlagAffectsGameplay, "Scissor attack damage per hit");
LUR_CVAR(CvScissorBuild,   "rps.unit.scissor.build_time",15,     CVarFlagAffectsGameplay, "Scissor build time (ticks, 10/s)");

// ---- Buildings (#138, spec §8). Buildings are placeable/producing/destroyable entities
// (#131 SoA). Each building type's health + placement cost is a knob nested UNDER that unit's
// console node (rps.unit.<type>.building_*) so a type's unit knobs and building knobs group
// together (spec §8 goal). All AffectsGameplay: they gate the deterministic sim (placement
// cost, building HP, queue cap, footprint/repulsion geometry, the frontier gate, opening
// gold), so they latch into Cv, sync over lockstep, and are console-tunable.
// The placement COSTS are no longer placeholders: they come from a two-human playtest
// (2026-07-25) tuned for a TACTICAL opening rather than a spammy one — a camp is a real
// investment and every soldier building costs more than you start with, so the first soldier is
// always mined for, never opened with. Re-tune by playing, not by reasoning: these are felt
// numbers.
//
// 2026-07-26: promoted from the phone's persisted rps-cvars.cfg — these are what was actually
// being PLAYED on the Galaxy, while the compile-time defaults had drifted well below them (camp
// 500, rock 700, paper 1000, scissor 1250). The spread also widened deliberately: soldier
// buildings now step 1500/2000/3250 rather than 700/1000/1250, so committing to a type is a real
// decision and a mis-read counter is expensive. Anything measured against the old numbers (the AI
// tier ladder especially) has to be re-measured — a tier tuned against a cheaper purse is tuned
// against a different game. ----
LUR_CVAR(CvMinerBuildingHp,     "rps.unit.miner.building_hp",     200, CVarFlagAffectsGameplay, "Mining-camp building hit points");
LUR_CVAR(CvMinerBuildingCost,   "rps.unit.miner.building_cost",   600, CVarFlagAffectsGameplay, "Gold to place a mining camp");
LUR_CVAR(CvRockBuildingHp,      "rps.unit.rock.building_hp",      300, CVarFlagAffectsGameplay, "Rock building hit points");
LUR_CVAR(CvRockBuildingCost,    "rps.unit.rock.building_cost",   1500, CVarFlagAffectsGameplay, "Gold to place a Rock building");
LUR_CVAR(CvPaperBuildingHp,     "rps.unit.paper.building_hp",     350, CVarFlagAffectsGameplay, "Paper building hit points");
LUR_CVAR(CvPaperBuildingCost,   "rps.unit.paper.building_cost",  2000, CVarFlagAffectsGameplay, "Gold to place a Paper building");
LUR_CVAR(CvScissorBuildingHp,   "rps.unit.scissor.building_hp",   250, CVarFlagAffectsGameplay, "Scissor building hit points");
LUR_CVAR(CvScissorBuildingCost, "rps.unit.scissor.building_cost",3250, CVarFlagAffectsGameplay, "Gold to place a Scissor building");
// Home base (#146, the HQ): one per team, auto-placed at the baseline. Inert (no production, no
// gathering, no attacking) — it just sits and soaks damage. Every enemy soldier treats it as prey
// (no RPS counter), friendlies defend it like a cart. Destroying the enemy's home base WINS the
// match — the decisive killing blow that replaced the slow economic-exhaustion win. Tanky by
// design: HP well above any other building (placeholder ~3x the toughest combat building).
LUR_CVAR(CvHomeBaseHp,            "rps.base.home_hp",        900,      CVarFlagAffectsGameplay, "Home base hit points (destroy to win)");
// Shared building knobs (one per concept, not per-type) under rps.build.*
LUR_CVAR(CvBuildingQueueMax,      "rps.build.queue_max",     40,       CVarFlagAffectsGameplay, "Max units queued per building (§12.3)");
LUR_CVAR(CvBuildingFootprint,     "rps.build.footprint",     F(3),     CVarFlagAffectsGameplay, "Building footprint radius, world units (overlap test)");
LUR_CVAR(CvBuildingRepelRadius,   "rps.build.repel_radius",  F(4),     CVarFlagAffectsGameplay, "Building movement-repulsion radius (world units)");
LUR_CVAR(CvBuildingRepelStrength, "rps.build.repel_strength",F(2),     CVarFlagAffectsGameplay, "Building movement-repulsion strength");
// How far a building must sit from a LIVE mine (#157). Was the footprint (3), which only kept the
// mine POINT outside the footprint — but the icons are much bigger than the footprint, so a camp
// drawn at radius 3.45 completely covered a mine drawn at radius 1.1, and the carts working that
// mine vanished under it. The whole gathering loop was invisible at exactly the spot the player
// cares about.
//
// 6 is derived, and it is also a CEILING the map imposes. What has to be visible is the ring of
// carts WORKING the mine: camp icon radius 3.45 + the carts' working spread (~2, WorkersPerMine
// diggers separated around the deposit) = 5.45, so 6.
//
// Do not raise it without re-tuning the AI ladder. Mines within a row are only 5-6 apart in X, so
// demanding more from EVERY mine pushes camps away from the whole row and income becomes bound by
// cart TRAVEL rather than miner count — which breaks economy-heavy tiers specifically. Measured at
// 20 matches/pairing: at 6 the ladder is hard>medium 14-6, medium>easy 17-3, hard>easy 20-0; at 7,
// hard LOSES to easy 0-10 because its worker_target buys miners it cannot employ. 6.5 is
// non-transitive. The clearance and the tier economy knobs are one joint tuning problem.
//
// It also sets how far the end mine rows must sit from the map edge to be unbuildable-behind
// (BuildMap asserts that invariant).
LUR_CVAR(CvMineClearance,         "rps.build.mine_clearance",F(6),     CVarFlagAffectsGameplay, "Min building-centre distance from a live mine (world units)");
// World-space starting buildable depth from a team's baseline (§5.3). NOT pixel-derived —
// tuned to CORRESPOND to the locked bottom camera band, never computed from screen size.
LUR_CVAR(CvInitialFrontier,       "rps.build.initial_frontier", F(35), CVarFlagAffectsGameplay, "Starting buildable depth from baseline (world units)");
// Opening gold (§12.6): sized to buy the forced opening and nothing else — one mining camp
// (MinerBuildingCost 600) + six miner carts (6 x MinerCost 100) = 1200 exactly. A combat building
// is gated on the first miner unit anyway (ApplyPlace) AND now costs more than the whole opening
// purse, so a player cannot open with military under any spend order. Keep this in step with those
// two costs if they change — the "exactly one camp + N carts" property is the point, not the number.
LUR_CVAR(CvStartingGold,          "rps.econ.starting_gold",  1200,     CVarFlagAffectsGameplay, "Opening gold: one mining camp (600) + 6 miner carts (100 ea)");

// ---- Economy (spec §3, gold/miner + finite mines per #84) ----
// Playtest 2026-07-19: several carts may work one deposit at once — the cap is the
// "room around it" proxy; separation steering spreads the diggers into a ring.
constexpr int32_t WorkersPerMine = 6;
constexpr int32_t DigTicks = 15;           // 1.5 s to fill a carry (default for the CVar below)
// How fast a cart gathers (#122): ticks to fill one carry. Lower = faster mining. Default =
// the constant above, so the economy is unchanged until edited.
LUR_CVAR(CvDigTicks, "rps.economy.dig_ticks", DigTicks, CVarFlagAffectsGameplay, "Ticks a cart digs to fill a carry (lower = faster)");
constexpr int32_t CarryCapacity = 15;      // gold per round trip
// (#135: no compile-time StartGold/StartMiners — the match opens with only CvStartingGold and no
// units; each team places its mining camp to begin producing.)
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

// ---- Production spawn ring (#132) ----
// A building spawns each finished unit at a small deterministic offset around its center;
// RingSlots is the ring size (index % RingSlots, no RNG). Also seeds the start-miner ring.
constexpr int32_t RingSlots = 8;           // deterministic spawn ring

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
LUR_CVAR(CvSepRadius, "rps.boid.sep_radius", F(24, 10), CVarFlagAffectsGameplay, "Same-team keep-apart radius (world units)");
LUR_CVAR(CvSeparationStrength, "rps.boid.sep_strength", F(3, 2), CVarFlagAffectsGameplay,
         "Same-team push-apart strength; must beat cohesion at contact or the blob turns to mush");
// Enemy separation (new, #96 decision #2): a wider radius / stronger push un-piles engaged
// fights into arcs instead of cross-team pixel-piles. Soldiers only (miners ignore combat).
LUR_CVAR(CvEnemySepRadius, "rps.boid.enemy_sep_radius", F(3, 2), CVarFlagAffectsGameplay, "Enemy keep-apart radius (world units)");
LUR_CVAR(CvEnemySeparationStrength, "rps.boid.enemy_sep_strength", F(1), CVarFlagAffectsGameplay,
         "Push-apart strength against enemies; un-piles a melee into arcs instead of a pixel-pile");
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
LUR_CVAR(CvCohSameRadius, "rps.boid.coh_same_radius", F(15), CVarFlagAffectsGameplay, "Same-type cohesion radius (world units)");
LUR_CVAR(CvWCohSame, "rps.boid.w_coh_same", F(1, 3), CVarFlagAffectsGameplay,
         "Pull toward your OWN type's centre: keep it gentle, a soft wide gather not a hard clump");
LUR_CVAR(CvCohAllRadius, "rps.boid.coh_all_radius", F(9), CVarFlagAffectsGameplay, "Whole-army cohesion radius (world units)");
// Cross-type army cohesion is SUPER TINY (2026-07-20 playtest): types shouldn't want to
// pile onto each other — same-type globs are the readable unit; the whole-army pull is a
// barely-there nudge so they don't scatter to opposite corners.
LUR_CVAR(CvWCohAll, "rps.boid.w_coh_all", F(1, 64), CVarFlagAffectsGameplay,
         "Pull toward the WHOLE army's centre: far below w_coh_same, just enough not to scatter");
LUR_CVAR(CvWSeek, "rps.boid.w_seek", F(1), CVarFlagAffectsGameplay,
         "Goal-pursuit weight — the reference 1.0 the other flock weights are judged against");
// Predator flee (2026-07-20 playtest): a unit must NEVER steer toward the enemy type it
// is weak against (the type that beats it). A repulsion from that predator, larger radius
// than enemy separation, corrected falloff (strongest at contact). Chases prey, flees the
// counter — so the RPS triangle plays out spatially, not just in the damage numbers.
// 2026-07-26 (promoted from the phone's persisted config): radius roughly doubled to 15 and the
// weight cut from 0.25 to 0.1 — flee EARLIER but far more GENTLY. The strong short-range version
// read as units flinching on contact; a wide soft drift instead bends approach paths, so armies
// slide around their counter rather than bouncing off it.
LUR_CVAR(CvPredatorFleeRadius, "rps.boid.predator_flee_radius", F(15), CVarFlagAffectsGameplay, "Flee-your-counter radius (world units)");
LUR_CVAR(CvWPredatorFlee, "rps.boid.w_predator_flee", FRound(1, 10), CVarFlagAffectsGameplay,
         "Drift away from the type that beats you; keep under w_seek so hunting prey still wins");
// Organic wander (2026-07-20 playtest): a slow, smooth per-unit noise offset added to the
// steer — the deterministic fixed-point analog of Simplex/OpenSimplex noise (value noise
// with a smoothstep fade; no floats, no libs). WNoise is its amplitude; NoiseTimeScale is
// ticks→lattice (smaller = slower, smoother drift).
LUR_CVAR(CvNoiseTimeScale, "rps.boid.noise_time_scale", F(1, 12), CVarFlagAffectsGameplay, "Noise temporal frequency (lattice cells/tick)");
LUR_CVAR(CvWNoise, "rps.boid.w_noise", F(2, 5), CVarFlagAffectsGameplay, "Wander amplitude (world-units of pull)");
// Fractal (fBm) noise levers (#123): stack N octaves of the value noise, each at Lacunarity x
// the frequency and Gain x the amplitude, normalized. Octaves=1 is exactly the single-octave
// wander above (bit-identical default) — turn it up for richer, less repetitive drift.
LUR_CVAR(CvNoiseOctaves,     "rps.boid.noise_octaves",     1,       CVarFlagAffectsGameplay, "Noise octaves (1 = smooth; more = detailed)");
LUR_CVAR(CvNoiseGain,        "rps.boid.noise_gain",        F(1, 2), CVarFlagAffectsGameplay, "Amplitude falloff per octave (persistence)");
LUR_CVAR(CvNoiseLacunarity,  "rps.boid.noise_lacunarity",  F(2),    CVarFlagAffectsGameplay, "Frequency multiply per octave");
// Slice B (#97) — FLOW: momentum via implicit velocity Δ = Pos − Prev (fixed tick, so
// last tick's displacement IS the velocity — no VelX/VelY arrays). The finalize does
// NewPos = Pos + Damp·Δ + ChebClamp(desired − Δ, MaxAccel), then clamps the step to
// Speed. Alignment steers a soldier toward its same-type neighbours' average velocity.
// Lava-lamp: slower turns (MaxAccel down) + more glide (Damp up) = the viscous feel.
LUR_CVAR(CvAlignRadius, "rps.boid.align_radius", F(5), CVarFlagAffectsGameplay, "Same-type velocity-alignment radius (world units)");
LUR_CVAR(CvWAlign, "rps.boid.w_align", F(1, 4), CVarFlagAffectsGameplay,
         "Match same-type neighbours' heading — turns a crowd into laminar flow");
LUR_CVAR(CvMaxAccel, "rps.boid.max_accel", FRound(10, 100), CVarFlagAffectsGameplay,
         "Per-tick turn/accelerate clamp: lower = heavier, gloopier units (~0.7 s to reach speed)");
LUR_CVAR(CvFlockDamping, "rps.boid.flock_damping", F(9, 10), CVarFlagAffectsGameplay,
         "Momentum kept per tick in free flight: higher = more glide (the lava-lamp feel)");
LUR_CVAR(CvInRangeDamping, "rps.boid.inrange_damping", F(1, 2), CVarFlagAffectsGameplay,
         "Momentum kept once in attack range: low so a unit settles instead of orbiting its target");
// Slice C (#98) — guard-lite INTERPOSE: an enemy soldier within GuardAlertR of one of MY
// miners is a RAIDER. A defender that has BOTH a friendly cart and a flagged raider within
// InterposeR steers to the point BETWEEN them — screening the cart (even from a predator it
// wouldn't attack). Positioning, not targeting: it keeps raiders off the economy by body.
constexpr Fixed GuardAlertR = F(6);                // raider = enemy soldier this close to a cart
LUR_CVAR(CvInterposeRadius, "rps.boid.interpose_radius", F(12), CVarFlagAffectsGameplay, "Cart/raider interpose reaction radius (world units)");
LUR_CVAR(CvWInterpose, "rps.boid.w_interpose", F(1), CVarFlagAffectsGameplay,
         "Pull toward the point between a raider and your cart — screening by body, not targeting");
// The single flock GATHER radius = the LARGEST force radius. One widened neighbour walk feeds
// every force (each re-tests its own smaller radius), so brute≡grid holds no matter which
// force is widest. Now that the radii are CVars (#123) it is DERIVED AT RUNTIME from the
// latched snapshot — Sim::GatherR, recomputed by DeriveUnits() each tick as the max of the
// gathered radii — so raising any one radius can never silently under-cover the grid path.
// Targeting: distances quantize into bands of this width (Chebyshev units); within one
// band the TYPE-PREFERENCE ladder decides (prey > mirror > neutral > predator, Sim.cpp).
// Playtest 2026-07-20: WIDENED from 3 to 12 so the whole engagement neighbourhood is one
// band — a unit hunts the enemy type it beats even when a mirror is somewhat nearer,
// instead of just fighting whoever's closest. Beyond the band, closeness takes over again.
constexpr Fixed TargetBand = F(12);
// ---- rps.mine.* — a deposit's PLACEMENT and its SIZE, all tunable (#157) ----
// Playtest 2026-07-19: carts RING a deposit instead of standing on it — a cart can dig once within
// dig_range, and live deposits push units outward (soft obstacles, same strength as unit
// separation). KEEP repel_radius < dig_range, or diggers are pushed out of their own reach and the
// deposit stalls: the diggers settle in the annulus between the two.
//
// A mine has THREE sizes and they are independent, so tuning one alone looks wrong:
//   visual_size  — the drawn diameter. RENDER ONLY (not AffectsGameplay): it never enters the sim,
//                  the hash, or the peer sync, so two devices may legitimately disagree on it.
//   repel_radius — the physical soft-obstacle radius units are pushed out of.
//   dig_range    — how close a cart must get to actually mine.
// They start deliberately mismatched: drawn radius is 1.1 (visual_size 2.2 / 2) while the physical
// push is 1.5, so the obstacle is already wider than the art. Note NONE of these affect how close a
// BUILDING may be placed — that is rps.build.mine_clearance (placement) and rps.build.footprint,
// which are a separate family from repulsion.
LUR_CVAR(CvMineDigRange,    "rps.mine.dig_range",    FRound(22, 10), CVarFlagAffectsGameplay, "How close a cart must be to dig (world units)");
LUR_CVAR(CvMineRepelRadius, "rps.mine.repel_radius", F(3, 2),        CVarFlagAffectsGameplay, "Soft-obstacle radius pushing units off a deposit (keep < dig_range)");
LUR_CVAR(CvMineVisualSize,  "rps.mine.visual_size",  FRound(22, 10), CVarFlagNone,            "Drawn mine DIAMETER in world units (render only — never synced)");
// The two STARTER rows, as a distance in from each team's own end (so they mirror by construction).
// #157 put them hard against the edge to seal the ground behind them; these expose that choice.
// BuildMap WARNS (it does not assert — these are yours to tune) when a value stops sealing: the
// seal holds while row < 1.5 x footprint + mine_clearance, i.e. below 10.5 at the defaults.
// Only the two starter rows are knobs. midfield/contested stay derived from WorldHeight so they
// keep scaling with the map instead of freezing at an absolute Y.
LUR_CVAR(CvMineRowHome,     "rps.mine.row_home",     F(3),           CVarFlagAffectsGameplay, "Starter row 1: distance in from each team's end (world units)");
LUR_CVAR(CvMineRowSafe,     "rps.mine.row_safe",     F(9),           CVarFlagAffectsGameplay, "Starter row 2: distance in from each team's end (world units)");

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

// #149: how long the win/lose screen stands before a FRESH match begins (awaiting both camps
// again). PRESENTATION, never hashed and never a sim input — it only paces when the next match is
// built. It lives here rather than in either main so solo and linked cannot drift apart: a peer
// restarting on a different hold than its opponent is harmless (the #139 both-camps gate covers
// the skew) but two DIFFERENT holds would read as a bug on whichever phone waited longer.
//
// Counted in WALL time on purpose. Sim::StepEvents early-returns once Result != ResultOngoing, so
// there is no post-result tick to count — a tick-based hold would never expire.
constexpr uint64_t PostMatchHoldNs = 4'000'000'000ull;

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

// ---- AI opponent per-tier knobs (#124-#126). The dotted name is the console tree:
// rps.ai.<tier>.<knob>, so each difficulty is its own sub-category (rps > ai > easy/medium/
// hard). All AffectsGameplay, so they latch into Cv (deterministic + latched-at-Init + console-
// visible) exactly like every other gameplay knob; single-player never has a peer, so the sync
// is moot but harmless. Difficulty = information quality (staleness/precision) + reaction
// cadence; the strategy knobs (open/worker/ratio/allin) shape the FSM. One macro emits the nine
// knobs for a tier so the three stay in lockstep. ----
#define LUR_AI_TIER(Tier, Pfx, OW, WT, ST, PR, CA, JI, HY, AL, SR, QD, MB, DF, BC)                                    \
    LUR_CVAR(CvAi##Tier##OpenWorkers,  "rps.ai." Pfx ".open_workers",  OW, CVarFlagAffectsGameplay, "Miners to open with before soldiers");        \
    LUR_CVAR(CvAi##Tier##WorkerTarget, "rps.ai." Pfx ".worker_target", WT, CVarFlagAffectsGameplay, "Target miner count (economy)");               \
    LUR_CVAR(CvAi##Tier##Staleness,    "rps.ai." Pfx ".staleness",     ST, CVarFlagAffectsGameplay, "Enemy-read delay in ticks (higher = slower to react)"); \
    LUR_CVAR(CvAi##Tier##Precision,    "rps.ai." Pfx ".precision",     PR, CVarFlagAffectsGameplay, "Enemy-count rounding bucket (1 = exact)");    \
    LUR_CVAR(CvAi##Tier##Cadence,      "rps.ai." Pfx ".cadence",       CA, CVarFlagAffectsGameplay, "Ticks between re-decisions");                 \
    LUR_CVAR(CvAi##Tier##Jitter,       "rps.ai." Pfx ".jitter",        JI, CVarFlagAffectsGameplay, "Random +/- cadence jitter (ticks)");          \
    LUR_CVAR(CvAi##Tier##Hysteresis,   "rps.ai." Pfx ".hysteresis",    HY, CVarFlagAffectsGameplay, "Lead margin before switching countered type");\
    LUR_CVAR(CvAi##Tier##AllinLead,    "rps.ai." Pfx ".allin_lead",    AL, CVarFlagAffectsGameplay, "Army lead (units) that triggers all-in");     \
    LUR_CVAR(CvAi##Tier##SoldierRatio, "rps.ai." Pfx ".soldier_ratio", SR, CVarFlagAffectsGameplay, "Soldier bias vs workers (percent)"); \
    LUR_CVAR(CvAi##Tier##QueueDepth,   "rps.ai." Pfx ".queue_depth",   QD, CVarFlagAffectsGameplay, "Units kept queued per building (batch size)"); \
    LUR_CVAR(CvAi##Tier##MaxBuildings, "rps.ai." Pfx ".max_buildings", MB, CVarFlagAffectsGameplay, "Cap on producing buildings it will place (0 = unlimited)"); \
    LUR_CVAR(CvAi##Tier##DefenceFloor, "rps.ai." Pfx ".defence_floor", DF, CVarFlagAffectsGameplay, "Combat buildings to stand up BEFORE chasing the economy target"); \
    LUR_CVAR(CvAi##Tier##BuildCluster, "rps.ai." Pfx ".build_cluster", BC, CVarFlagAffectsGameplay, "Buildings of one type it commits to in quick succession (1 = no clustering)")
// ---- The ladder is now STRICTLY ORDERED BY DESIGN: the better tier always beats the lesser one. ----
// That replaces the old goal of a 77-83% adjacent-rung win rate. It is delivered by a monotonic
// PRODUCTION-VOLUME ladder — queue_depth 8/5/3 and max_buildings unlimited/12/4 for hard/medium/easy —
// which is a stronger and far more tunable ordering than information quality (staleness/precision)
// ever gave: measured 16-0 / 16-0 / 16-0 over 16 matches per pairing, with no upsets to chase.
//
// It does cost the premise recorded below ("every tier has identical actions; only information
// differs"). That premise was written before there was evidence; 16 recorded beginner losses showed
// the SHARED volume knobs were the dominant term in how brutal easy felt, so volume became the axis
// that carries difficulty. Information quality still shapes HOW a tier plays — it just no longer has
// to carry the whole ordering on its own.
//
// Historical note on the old ordering (kept because the reasoning still explains the knobs):
// Tier STRENGTH is ordered by the economy knobs, not only by information quality (measured
// 2026-07-25, 30 matches per pairing). Once the AI could batch-queue and expand, being
// economy-heavy became the dominant strategy — and `easy` accidentally had the best economy of the
// three (soldier_ratio 50 vs hard's 55), so it beat medium 9-1 and even beat hard. So the ladder now
// runs the other way on purpose: soldier_ratio 55 / 65 / 80 and worker_target 45 / 22 / 8 for
// hard / medium / easy. A high soldier_ratio STARVES the economy, which is what makes easy weak.
//
// Note worker_target only bites before contact: the FSM leaves the Building state as soon as the
// enemy fields anything, after which soldier_ratio governs the split. Moving hard's worker_target
// between 28 and 45 changed nothing at all in a real match.
//
// EASY IS THE TEACHING TIER, and that is a different job from "bottom rung" (#155). The app opens
// straight into an Easy match (RpsMain.mm), so it is first contact for every new player — and the
// ladder's own metric is blind to whether a human can learn against it. Tiering easy by AI-vs-AI
// win rate had made it weak by STARVING its economy (worker_target 8 / soldier_ratio 80), which is
// an early rush: a liability against hard (the #154 coin flip) and a massacre against a first-timer.
// Measured with --aibeginner (tier vs a place-a-camp-then-idle player): the first wave reached the
// player's build zone at 102s and wiped them 6/6. worker_target 30 pushes that to ~155s.
//
// That knob is the right one precisely BECAUSE of the note below: worker_target only bites before
// contact — and against a beginner, pre-contact IS the whole match. (allin_lead is inert here for a
// different reason: past worker_target the Building state already produces soldiers exclusively, so
// AllIn changes nothing. Don't reach for it.) Re-measure with --aibeginner, never --aivs, which
// cannot see this failure at all.
//
// Easy's TWO jobs are now TWO SEPARATE KNOBS, and keeping them separate is what makes this tunable:
//   * worker_target 30 = the beginner grace window (pre-contact only) -> wave at ~155s.
//   * soldier_ratio  40 = its rung on the ladder (Reacting only)      -> medium beats it 8-2.
// They are independent because a passive player never fields an army, so the FSM never leaves
// Building and soldier_ratio is never consulted — measured: the 157s window is IDENTICAL at
// soldier_ratio 30/40/50. Tune the window with worker_target, the rung with soldier_ratio.
//
// soldier_ratio 40, NOT the old 80, and the reason the direction flipped matters: the note below
// says a HIGH ratio starves the economy and is what made easy weak. That was true at worker_target
// 8 (no economy AND no savings). At worker_target 30 the economy is already banked before contact,
// so a high ratio is no longer starvation — it is economy-first-then-all-in, which is a genuinely
// GOOD build order, and easy beat medium 6-8 with it. With a healthy opening the weakening
// direction is LOW: never converting the money into army.
//                 OW  WT  ST  PR  CA  JI HY  AL  SR
// Economy PACED TO A REAL BEGINNER (measured from 16 recorded losses, 2026-07-26). The recordings
// showed easy was not out-fighting first-timers, it was out-ECONOMISING them ~3x and then converting
// the bank into 90-220 soldiers in a terminal 30s burst: workers 11/24/45/55 at 60/90/120/150s
// against the beginner's 4.9/8.8/16.4/27.7, and 8-12 buildings against their 1-4. Neither side had
// an army before ~90s, so the old "it rushes" reading was wrong — the flood is funded, not early.
// worker_target 14, queue_depth 2 and max_buildings 4 aim easy's ramp at the beginner curve, which
// is what makes a 200-unit bombardment arithmetically impossible rather than merely discouraged.
// Paced to the BETTER HALF of those 16 (ranked by the economy they actually built), so a real
// challenge survives: that half ran workers 5/11/24/42 and ~2/2/3/5 buildings at 60/90/120/150s,
// with peak armies averaging ~27. Measured result at 24/3/4: workers 11/25/29/29, soldiers
// 0/0/20/40, 4 buildings — against easy's OLD 55 workers, 8-12 buildings and 86 soldiers at 150s.
// max_buildings is the dominant term, not worker_target: production is flat per building (#132), so
// each extra building is worth ~25 soldiers by 150s (measured mb 4/5/6 -> 40/65/81).
// One thing these knobs CANNOT do is match the beginner's 5 workers at 60s — easy sits at 11 in every
// configuration, because the AI decides every tick and simply hits the gold-limited maximum. The
// human's 5 is hesitation, not economics; closing that needs a decision throttle, not a volume cap.
LUR_AI_TIER(Easy,   "easy",   4, 24,  60, 4, 50, 15, 3, 20, 40, 3,  4, 0, 1);
LUR_AI_TIER(Medium, "medium", 4, 22,  20, 2, 20, 6,  2, 15, 65, 5, 12, 1, 2);
// Hard's economy knobs come from a MEASURED human win (2026-07-25 flight recordings, #144): the
// player beat it in 2:49 running 108 workers to its 20 and a 43%-worker army, while hard's
// worker_target of 10 and 70% soldier bias capped its economy at ~30% and starved the compounding
// that made the human's flood possible. Target/ratio now follow the human's shape.
// Cadence 12, not 5: re-deciding every half second thrashes hard's production across types (a
// queued batch of the wrong counter is dead gold). At cadence 5 it lost to easy 3-27; at 12 it beats
// medium 25-5. Slower still is worse again — at 25/40 it starts losing to medium too — so 12 is a
// measured knee, not a preference.
//
// worker_target 22, down from 45 (#157). The mine clearance widening (3 -> 6) made income bound by
// cart TRAVEL rather than miner count — and WorkersPerMine caps 6 diggers per deposit, so a target of
// 45 needed 8 served mines that the wider clearance no longer lets it place. It was buying miners it
// could not employ, and starving its army to do it: at 45 it LOST to medium 3-7. At 22 the ladder is
// hard>medium 14-6, medium>easy 17-3, hard>easy 20-0 (20 matches/pairing). This knob is coupled to
// rps.build.mine_clearance — re-measure both together.
// UNCAPPED against a competent human (2026-07-26, from 5 recordings of the repo owner beating hard).
// He wins the same way every time, and none of it is cleverness — it is SCALE:
//   * zero soldiers until ~120s, banking economy while hard also holds; he reaches 62 workers at
//     104s and 113-131 at 138s against hard's 38 and 71, peaking 121-213 vs hard's 60-121;
//   * 16-34 buildings against hard's 9-24 (production is flat per building, so that IS throughput);
//   * +5 spammed at a 0.2s median cadence, ~78% of 171-354 commands, 703-1470 units queued;
//   * soldier buildings marched forward to Y=212 with hard's base at 240, so his reinforcements
//     appear in the fight while hard's walk the map.
// Hard then LOSES units faster than it replaces them (army 113->93->70) and ends sitting on gold.
// It was never out-thought, it was throttled: worker_target stopped its economy dead at ~94 while his
// compounded to 213. So worker_target 110 (his 138s figure; WorkersPerMine 6 x 48 mines allows ~288,
// so the mines are not the ceiling) and queue_depth 20 so a big bank actually converts.
// queue_depth stays 8, NOT the 20 I tried: measured hard 8/12 vs medium at depth 8, 4/12 at 12 and
// 0/12 at 20. The note above already predicted it — a deep queue commits gold to a TYPE, and hard is
// the tier that re-counters fastest, so it is the tier a deep queue punishes most. Raising it was a
// direct contradiction of measured history and it cost every match.
// defence_floor 3 is what BUYS the greed above. worker_target 110 alone lost to medium (#154's
// unfixed hole): economy-first with no combat capacity standing means a timely attack arrives while
// hard has zero soldier buildings and must start them from scratch, and worker_target is inert after
// contact so no knob could reach it. The floor is capacity, not intent.
LUR_AI_TIER(Hard,   "hard",   5, 110, 0,  1, 12, 2,  1, 10, 55, 8,  0, 5, 3);
#undef LUR_AI_TIER

// ---- AI production/expansion knobs (#144), shared by ALL tiers on purpose ----
// These are ACTION QUALITY, and the design's premise is that every tier has identical actions and
// differs only in the quality of its INFORMATION (staleness/precision/cadence). Making expansion
// per-tier would smuggle in a second handicap axis and make the (already non-transitive, #152)
// ladder harder to tune, so all three tiers expand identically.
//
// Why they exist: #132 made throughput scale with BUILDING COUNT (flat production, no queue
// snowball), but the AI only ever placed a building of a type when it owned NONE — so it was capped
// at one per type, four total, and by six minutes it was banking 17k-26k gold it could never spend
// while its army sat at 15-42 (measured with --aidiag). Capacity is the lever it was missing.
// 8, measured — and the trade-off is real, so it is a knee, not a maximum. Deeper queues convert
// banked gold into army (a 170s match ends on ~18k instead of ~32k), but they also commit gold to a
// TYPE, and a deep queue of the wrong type is dead weight — which penalises the tier that
// re-counters fastest. Measured against opponents that play DIFFERENTLY (a mirror match hides this
// entirely, since both sides pay the same penalty): hard beats easy 8-2 at depth 8, but only 6-4 at
// 12. Before batching existed, depth 6 alone was enough to send hard 0-10.
// How far BEHIND its own leading edge the AI plants combat buildings. 3, down from a hardcoded 8:
// the recordings show the human building at Y=212 against a base at 240, so his units spawn in the
// fight while the AI's walked. This is also the ONLY lever the AI has for "mass before engaging" — it
// emits Place and Queue events and nothing else, so it cannot hold units back; unit movement is
// autonomous steering. Producing at the front is how it arrives massed instead of in a trickle.
// How close an existing camp must be for the AI to consider a deposit ALREADY SERVED. Was a
// hardcoded 18, and that is why the AI only ever had camps at its base (player report, confirmed):
// the test is CHEBYSHEV and the mine columns span X=4..30 on a 34-wide map, so ONE camp at mid-X
// covers X in [-1,35] — the entire width — and marks a whole row served. +/-18 in Y also swallowed
// both starter rows at once (Y=3 and Y=9). The AI therefore stopped expanding after its first camp
// or two, while a human plants a camp on every row the front rolls past.
// 7 is about one column gap (columns sit 5-6 apart), so a camp claims the 2-3 deposits it can
// actually work — WorkersPerMine caps 6 carts per deposit, so claiming six mines with one camp was
// always a fiction, and cart round-trip time grows with the distance it pretended not to have.
LUR_CVAR(CvAiMineServedRadius, "rps.ai.mine_served_radius", F(7), CVarFlagAffectsGameplay,
         "A deposit counts as served if a camp is within this (world units)");
// 8, not the 3 I tried. Building at 3 costs hard ~2 wins in 12 (10 -> 8 vs medium): buildings planted
// that close to the leading edge sit in contested ground and get razed. The human gets away with it
// because he plants forward BEHIND a mass he already has; the AI plants forward and then loses the
// building. Forward production is only safe once there is an army in front of it.
// How many units the AI must be able to STOCK each building of a cluster with before it commits to
// that cluster. This is what makes clustering safe: the first attempt committed on nothing but intent,
// so a poor AI spent its ticks placing empty buildings and queued nothing, and the silence lost it
// 4 of 16 against medium. Gating on "can I fill them" means it only goes quiet when it can afford the
// wave that follows -- which is the whole point of the strategy it is copying.
// MINER queue depth, separate from the combat queue depth and deliberately SHALLOW. Copied from the
// player's opening: he queues only ~2 carts per camp at a time -- enough to keep every camp producing,
// while the gold that a deeper queue would have swallowed accrues toward the NEXT CAMP instead. The
// AI was using its combat depth (8) for carts, so its opening gold sat locked in one camp's queue and
// its second camp came late. Production is flat per building, so early gold spent on CAMPS compounds
// and the same gold spent on a deep cart queue does not: parallel mining beats a long line at one camp.
LUR_CVAR(CvAiMinerQueueDepth, "rps.ai.miner_queue_depth", 2, CVarFlagAffectsGameplay,
         "Carts the AI keeps queued per mining camp (shallow, so gold banks toward the next camp)");
LUR_CVAR(CvAiClusterFillUnits, "rps.ai.cluster_fill_units", 10, CVarFlagAffectsGameplay,
         "Units the AI must be able to afford per building before committing to a cluster");
LUR_CVAR(CvAiFrontSetback, "rps.ai.front_setback", F(8), CVarFlagAffectsGameplay,
         "How far behind its own frontier the AI builds combat buildings (world units)");
LUR_CVAR(CvAiQueueDepth, "rps.ai.queue_depth", 8, CVarFlagAffectsGameplay,
         "Units the AI keeps queued per building before it wants more capacity");
// 200. I lowered this to 130 believing hard hoarded; measured across the finished behaviour it is a
// SHARP optimum and 130 was simply wrong. Once the shallow miner queue was in, eager expansion made
// hard sprawl into 14 buildings with ONE soldier -- all capacity, never converted -- and it lost 4/16
// to medium. Thrift is no better: at 300+ it under-expands into a 19-worker, 3-building economy and
// loses 5/16. At 200 it banks enough to answer with quick, expensive counters and takes 15/16 and
// 16/16. Left SHARED: easy and medium are bounded by their per-tier max_buildings (4 / 12).
//   130 -> 4/16   (wrk 110, sol 1,  bld 14)
//   200 -> 15/16  (wrk 114, sol 47, bld 16)
//   300 -> 5/16   (wrk 19,  sol 4,  bld 3)
LUR_CVAR(CvAiExpandGoldFactor, "rps.ai.expand_gold_factor", 200, CVarFlagAffectsGameplay,
         "Gold needed to add a building, as a percent of its cost (200 = can afford two)");

// ---- Dev-only knobs (#156). NOT AffectsGameplay, and that is the whole point: these never latch
// into CvSnapshot, never enter StateHash, never sync to the peer, and must never appear in the
// LUR_RPS_GAMEPLAY_CVARS X-list below. They change what the BUILD does, not what the SIM computes,
// so two peers may legitimately disagree about them. The console shows them in the same tree as the
// sim tunables, distinguished by the "AG" tag the gameplay ones carry. ----
//
// The flight recorder writes one file per solo match under the app's data dir (#144). It used to be
// LUR_AGENT — absent from anything played — because it is automatic capture that writes files during
// someone's session. It is now a DEV-BUILD DEFAULT with a visible off switch instead: the capture is
// what makes a playtest readable afterwards, and requiring a special build to get it meant the
// interesting match was always the one that wasn't recorded. The safeguards that replace absence are
// that the switch is visible in the console rather than a hidden property, and that Shipping still
// compiles the recorder out entirely (LUR_INTERNAL).
LUR_CVAR(CvFlightRecorder, "rps.dev.flight_recorder", true, CVarFlagNone,
         "Record each match to a timestamped .rec file for later replay");

// The X-macro fragment for one tier's nine ids (all int knobs), used 3x in the gameplay list.
#define LUR_AI_TIER_IDS(IX, Tier)              \
    IX(Ai##Tier##OpenWorkers,  CvAi##Tier##OpenWorkers)  \
    IX(Ai##Tier##WorkerTarget, CvAi##Tier##WorkerTarget) \
    IX(Ai##Tier##Staleness,    CvAi##Tier##Staleness)    \
    IX(Ai##Tier##Precision,    CvAi##Tier##Precision)    \
    IX(Ai##Tier##Cadence,      CvAi##Tier##Cadence)      \
    IX(Ai##Tier##Jitter,       CvAi##Tier##Jitter)       \
    IX(Ai##Tier##Hysteresis,   CvAi##Tier##Hysteresis)   \
    IX(Ai##Tier##AllinLead,    CvAi##Tier##AllinLead)    \
    IX(Ai##Tier##SoldierRatio, CvAi##Tier##SoldierRatio) \
    IX(Ai##Tier##QueueDepth,   CvAi##Tier##QueueDepth)   \
    IX(Ai##Tier##MaxBuildings, CvAi##Tier##MaxBuildings) \
    IX(Ai##Tier##DefenceFloor, CvAi##Tier##DefenceFloor) \
    IX(Ai##Tier##BuildCluster, CvAi##Tier##BuildCluster)


// ---- #112: the AffectsGameplay CVar set, defined ONCE and expanded four ways ----
// The snapshot Sim reads (CvSnapshot), the initial latch from the globals (LatchCvs), the
// 1-byte wire id per CVar (ECvId), and the id->field apply/read used by the peer sync
// (ApplyCvOverride/CvOverrideRaw) all come from this single X-list, so they can never
// drift. FX = a Fixed-typed knob, IX = an int32 knob. (The GameplayId cook — #112's
// remaining half — will GENERATE this list from the registry, lexicographically sorted
// and build-checked against the 256 cap; until then this hand-list is the source, and the
// id is declaration order, which agrees across peers because the build-fingerprint gate
// guarantees identical builds.)
#define LUR_RPS_GAMEPLAY_CVARS(FX, IX)                    \
    FX(SeparationStrength,      CvSeparationStrength)      \
    FX(EnemySeparationStrength, CvEnemySeparationStrength) \
    FX(WSeek,                   CvWSeek)                   \
    FX(WCohSame,                CvWCohSame)                \
    FX(WCohAll,                 CvWCohAll)                 \
    FX(WAlign,                  CvWAlign)                  \
    FX(WPredatorFlee,           CvWPredatorFlee)           \
    FX(WNoise,                  CvWNoise)                  \
    FX(WInterpose,              CvWInterpose)              \
    FX(MaxAccel,                CvMaxAccel)                \
    FX(FlockDamping,            CvFlockDamping)            \
    FX(InRangeDamping,          CvInRangeDamping)          \
    FX(NoiseTimeScale,          CvNoiseTimeScale)          \
    FX(SepRadius,               CvSepRadius)               \
    FX(EnemySepRadius,          CvEnemySepRadius)          \
    FX(CohSameRadius,           CvCohSameRadius)           \
    FX(CohAllRadius,            CvCohAllRadius)            \
    FX(PredatorFleeRadius,      CvPredatorFleeRadius)      \
    FX(AlignRadius,             CvAlignRadius)             \
    FX(InterposeRadius,         CvInterposeRadius)         \
    IX(NoiseOctaves,            CvNoiseOctaves)            \
    FX(NoiseGain,               CvNoiseGain)               \
    FX(NoiseLacunarity,         CvNoiseLacunarity)         \
    IX(CounterMultiplier,       CvCounterMultiplier)       \
    IX(MinerCost,               CvMinerCost)               \
    IX(MinerHp,                 CvMinerHp)                 \
    FX(MinerSpeed,              CvMinerSpeed)              \
    IX(MinerDamage,             CvMinerDamage)             \
    IX(MinerBuild,              CvMinerBuild)              \
    IX(RockCost,                CvRockCost)                \
    IX(RockHp,                  CvRockHp)                  \
    FX(RockSpeed,               CvRockSpeed)               \
    IX(RockDamage,              CvRockDamage)              \
    IX(RockBuild,               CvRockBuild)               \
    IX(PaperCost,               CvPaperCost)               \
    IX(PaperHp,                 CvPaperHp)                 \
    FX(PaperSpeed,              CvPaperSpeed)              \
    IX(PaperDamage,             CvPaperDamage)             \
    IX(PaperBuild,              CvPaperBuild)              \
    IX(ScissorCost,             CvScissorCost)             \
    IX(ScissorHp,               CvScissorHp)               \
    FX(ScissorSpeed,            CvScissorSpeed)            \
    IX(ScissorDamage,           CvScissorDamage)           \
    IX(ScissorBuild,            CvScissorBuild)            \
    IX(DigTicks,                CvDigTicks)                \
    IX(MinerBuildingHp,         CvMinerBuildingHp)         \
    IX(MinerBuildingCost,       CvMinerBuildingCost)       \
    IX(RockBuildingHp,          CvRockBuildingHp)          \
    IX(RockBuildingCost,        CvRockBuildingCost)        \
    IX(PaperBuildingHp,         CvPaperBuildingHp)         \
    IX(PaperBuildingCost,       CvPaperBuildingCost)       \
    IX(ScissorBuildingHp,       CvScissorBuildingHp)       \
    IX(ScissorBuildingCost,     CvScissorBuildingCost)     \
    IX(HomeBaseHp,              CvHomeBaseHp)              \
    IX(BuildingQueueMax,        CvBuildingQueueMax)        \
    FX(BuildingFootprint,       CvBuildingFootprint)       \
    FX(BuildingRepelRadius,     CvBuildingRepelRadius)     \
    FX(BuildingRepelStrength,   CvBuildingRepelStrength)   \
    FX(MineClearance,           CvMineClearance)           \
    FX(MineDigRange,            CvMineDigRange)            \
    FX(MineRepelRadius,         CvMineRepelRadius)         \
    FX(MineRowHome,             CvMineRowHome)             \
    FX(MineRowSafe,             CvMineRowSafe)             \
    FX(InitialFrontier,         CvInitialFrontier)         \
    IX(StartingGold,            CvStartingGold)            \
    LUR_AI_TIER_IDS(IX, Easy)                              \
    LUR_AI_TIER_IDS(IX, Medium)                            \
    LUR_AI_TIER_IDS(IX, Hard)                              \
    FX(AiMineServedRadius,      CvAiMineServedRadius)      \
    IX(AiMinerQueueDepth,       CvAiMinerQueueDepth)       \
    IX(AiClusterFillUnits,      CvAiClusterFillUnits)      \
    FX(AiFrontSetback,          CvAiFrontSetback)          \
    IX(AiQueueDepth,            CvAiQueueDepth)            \
    IX(AiExpandGoldFactor,      CvAiExpandGoldFactor)

// Authoritative gameplay values as POD (memcpy-able, folds into StateHash). Latched from
// the globals once at Sim::Init, then owned by the Sim and mutated only at tick boundaries
// by synced overrides — read inside the sim as S.Cv.Foo (that mark = "synced gameplay
// value", by design).
struct CvSnapshot {
#define FX(Name, Cv) Fixed Name;
#define IX(Name, Cv) int32_t Name;
    LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
};

inline CvSnapshot LatchCvs() {
    CvSnapshot S;
#define FX(Name, Cv) S.Name = Cv.Get();
#define IX(Name, Cv) S.Name = Cv.Get();
    LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
    return S;
}

// #147: the COMPILE-TIME defaults, ignoring every local override (a persisted rps-cvars.cfg,
// a console tweak). The peer cvar-sync's merge resolver is defined RELATIVE to the defaults —
// an id absent from the merged set means "default", and a wall-clock tie resolves to "default"
// — so the baseline both peers overlay the merged set onto must be the defaults, never each
// peer's own (differently overridden) globals. Overlaying onto LatchCvs() silently kept the
// local value for every unmerged id, which is one half of the #147 desync.
// In Shipping the globals ARE the defaults (a CVar holds nothing else), so it folds to LatchCvs.
inline CvSnapshot DefaultCvs() {
#if LUR_SHIPPING
    return LatchCvs();
#else
    CvSnapshot S;
#define FX(Name, Cv) S.Name = Cv.Default();
#define IX(Name, Cv) S.Name = Cv.Default();
    LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
    return S;
#endif
}

// Derive the per-type UnitStats the sim + HUD read from a latched snapshot: the compile-time
// table for the fixed fields (range/cooldown/beats), overlaid with the tunable CVars (#122).
// Shared by Sim::DeriveUnits (from the sim's latched Cv) and the pre-match HUD (straight from
// LatchCvs()), so the two can never disagree.
inline void DeriveUnitStats(const CvSnapshot& Cv, UnitStats Out[UnitCount]) {
    for (int Ty = 0; Ty < UnitCount; ++Ty) Out[Ty] = UnitTable[Ty];
    Out[UnitMiner].Cost = Cv.MinerCost;   Out[UnitMiner].MaxHp = Cv.MinerHp;
    Out[UnitMiner].Speed = Cv.MinerSpeed; Out[UnitMiner].Attack = Cv.MinerDamage;
    Out[UnitMiner].BuildTicks = Cv.MinerBuild;
    Out[UnitRock].Cost = Cv.RockCost;     Out[UnitRock].MaxHp = Cv.RockHp;
    Out[UnitRock].Speed = Cv.RockSpeed;   Out[UnitRock].Attack = Cv.RockDamage;
    Out[UnitRock].BuildTicks = Cv.RockBuild;
    Out[UnitPaper].Cost = Cv.PaperCost;   Out[UnitPaper].MaxHp = Cv.PaperHp;
    Out[UnitPaper].Speed = Cv.PaperSpeed; Out[UnitPaper].Attack = Cv.PaperDamage;
    Out[UnitPaper].BuildTicks = Cv.PaperBuild;
    Out[UnitScissor].Cost = Cv.ScissorCost;   Out[UnitScissor].MaxHp = Cv.ScissorHp;
    Out[UnitScissor].Speed = Cv.ScissorSpeed; Out[UnitScissor].Attack = Cv.ScissorDamage;
    Out[UnitScissor].BuildTicks = Cv.ScissorBuild;
}

// Per-type building health / placement cost from a latched snapshot (#138). One switch so
// the sim (placement HP + cost deduct, #133) and the HUD (button prices, #140) never disagree.
inline int32_t BuildingHpFor(const CvSnapshot& Cv, uint8_t Type) {
    switch (Type) {
        case UnitMiner:   return Cv.MinerBuildingHp;
        case UnitRock:    return Cv.RockBuildingHp;
        case UnitPaper:   return Cv.PaperBuildingHp;
        case UnitScissor: return Cv.ScissorBuildingHp;
        default:          return 0;
    }
}
inline int32_t BuildingCostFor(const CvSnapshot& Cv, uint8_t Type) {
    switch (Type) {
        case UnitMiner:   return Cv.MinerBuildingCost;
        case UnitRock:    return Cv.RockBuildingCost;
        case UnitPaper:   return Cv.PaperBuildingCost;
        case UnitScissor: return Cv.ScissorBuildingCost;
        default:          return 0;
    }
}

// 1-byte within-build wire id per gameplay CVar (Addendum C.0.1). Declaration order here;
// the build-fingerprint gate makes both peers agree (same build -> same list).
enum ECvId : uint8_t {
#define FX(Name, Cv) CvId##Name,
#define IX(Name, Cv) CvId##Name,
    LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
    CvIdCount
};
static_assert(CvIdCount <= 256, "AffectsGameplay CVars exceed the 1-byte wire id space (C.0.2)");

// Set / read one snapshot field by wire id, as a raw int32 (Fixed.Raw, or the int value).
// The value travels raw on the wire (enums-as-int, no float); these two are the id<->field
// bridge the LockstepPeer sync uses.
inline void ApplyCvOverride(CvSnapshot& S, uint8_t Id, int32_t Raw) {
    switch (Id) {
#define FX(Name, Cv) case CvId##Name: S.Name = Fixed{Raw}; break;
#define IX(Name, Cv) case CvId##Name: S.Name = Raw; break;
        LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
        default: break;
    }
}
inline int32_t CvOverrideRaw(const CvSnapshot& S, uint8_t Id) {
    switch (Id) {
#define FX(Name, Cv) case CvId##Name: return S.Name.Raw;
#define IX(Name, Cv) case CvId##Name: return S.Name;
        LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
        default: return 0;
    }
}

#if !LUR_SHIPPING
// Reverse wire map: a CVar's registered name string -> its ECvId, so a runtime (type-
// erased) edit can be routed through LockstepPeer::SetGameplayCvar. Dev-only (uses the
// CVar's Name(), which is dev-only metadata). Returns -1 for a non-gameplay name.
inline int GameplayIdForName(const char* Name) {
#define FX(F, Cv) if (std::strcmp(Name, Cv.Name()) == 0) return CvId##F;
#define IX(F, Cv) if (std::strcmp(Name, Cv.Name()) == 0) return CvId##F;
    LUR_RPS_GAMEPLAY_CVARS(FX, IX)
#undef FX
#undef IX
    return -1;
}
#endif

} // namespace Rps
