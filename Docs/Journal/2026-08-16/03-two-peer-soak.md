# How to build the two-peer soak

Frozen against `9668799`. This describes a harness that **does not exist yet**. Verify every symbol
against HEAD before acting on it — paths and APIs drift, and this file will not be updated.

---

## Why

On 2026-08-16 two phones played a real match, recovered from a desync **45 times**, and re-diverged
at the very next anchor every single time. Two candidate causes — cross-platform nondeterminism, or
input-dropping introduced by that day's fix — and **no way to tell them apart**, because there is no
way to run two lockstep peers through a long match anywhere except on two physical phones.

CLAUDE.md claims *"the whole netcode is proven in two windows on one desktop before a phone is in the
room"*. Here is the actual inventory:

| desktop mode | two `LockstepPeer`s? | can it play a match unattended? |
|---|---|---|
| `RunLoopback` (default / `--auto`) | **yes** (20 refs) | **no** — auto-soak unwired: `(void)Auto; (void)Rng; (void)ElapsedNs; (void)AutoAccumNs;  // #137b: auto-soak re-wires to events in #140`. Measured: `ticks A=0 B=0` after 2500 frames. |
| `--aidiag` (`RunAiDiag`) | **no** — one `Rps::Sim`, two `AiController`s | yes, but proves nothing about netcode |
| `--aivs`, `--aiowner`, `--aibeginner` | **no** (0 refs each) | same |

So the only mode with real lockstep cannot drive itself, and every mode that can drive itself has no
lockstep. That is the whole gap, and it is why this class of bug is only ever found on hardware at the
end of a long day.

---

## What it must be

**A host test target, not a desktop flag.** `RunLoopback` needs windows and a Vulkan device; the soak
needs neither. Everything it exercises — `LockstepPeer`, `Sim`, `AiController` — already builds on the
host and is already linked by `rps_net_tests`. Put it beside that.

Suggested: `Games/RocksPapersScissors/Net/Tests/TwoPeerSoak.cpp` → target `rps_two_peer_soak`,
registered with CTest but **not** in the default `build.ps1` gate if it runs long (see *Runtime*).

---

## The design

### The core loop

`NetTests.cpp` already has every piece. Reuse them rather than inventing a second harness — a second
one is how the four touch dispatches happened.

```cpp
struct Outbox { std::vector<std::pair<Lur::Net::EMsgType, std::vector<uint8_t>>> Q; };
static void Enqueue(void* Ctx, EMsgType T, const uint8_t* D, std::size_t N);
static void Deliver(Outbox& From, LockstepPeer& To);
static void DeliverDroppingNthInput(Outbox& From, LockstepPeer& To, int NthInput);
static void DeliverDuplicatingInputs(Outbox& From, LockstepPeer& To);
static bool TamperOneInput(Outbox& From, LockstepPeer& To, uint8_t ForgedTeam);
static void PlaceCampsAndStart(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb);
```

The shape:

```cpp
Outbox Qa, Qb;
LockstepPeer A, B;
A.Init(Seed, /*MyTeam*/ 0, Enqueue, &Qa);      // A = survivor by the device-id tie-break
B.Init(Seed, /*MyTeam*/ 1, Enqueue, &Qb);
PlaceCampsAndStart(A, B, Qa, Qb);

for (uint32_t Tick = 0; Tick < TargetTicks; ++Tick) {
    FeedAiInput(A, Ai0, /*team*/ 0);           // see below
    FeedAiInput(B, Ai1, /*team*/ 1);
    A.Tick(OneTickNs);
    B.Tick(OneTickNs);
    ApplyFaultSchedule(Tick, Qa, Qb, A, B);    // see below; plain Deliver when no fault is due
    CheckConvergence(Tick, A, B);              // see below
}
```

### Driving real input

`AiController` is the input source and needs no view:

```cpp
void Init(uint64_t Seed, uint8_t Team, EAiTier Tier);
void DecideEvents(const Sim& S, uint32_t Tick, InputEvent* Out, int Cap, int& Count);
```

Each peer runs **its own** AI over **its own** sim and queues the result with `QueueLocalEvent`. Do
**not** share one AI between them: the point is two independent input producers, exactly like two
players. `DesktopMain.cpp`'s `SampleSoloVsAi` shows the ordering convention (team 0's events before
team 1's, because that is the Execute order both peers use).

Note the AI holds until `S.HasMinerCamp(0)`, so `PlaceCampsAndStart` must run first or nothing happens
— that is precisely the `ticks A=0` failure the existing `--auto` produces.

### The assertion that matters

Everything else is secondary to this:

```cpp
// Once both peers have executed the same tick, their state MUST be identical.
if (A.ExecTick() == B.ExecTick())
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
```

**Do not assert only that both are still running.** On 2026-08-16 both phones were `started=1`,
`desync=0`, rendering at 60 fps, and playing *different games* — Android at tick 3320 with 32 units,
iPhone at 3150 with 16. "Alive" was true throughout. Convergence is the property; liveness is not a
proxy for it.

Track and report, per run:
- ticks reached, and whether the match resolved
- desync count, recovery rounds, gap recoveries
- **first divergent tick** and both hashes — that is the number a debugging session starts from
- `AwaitingResync()` on the **survivor**, which must never be true (the #210 invariant)

### The fault schedule

A soak that only delivers cleanly proves determinism and nothing else. Drive the faults deterministically
from the tick number and the seed, so a failure is reproducible from two integers:

| fault | helper | what it exercises |
|---|---|---|
| clean delivery | `Deliver` | baseline determinism |
| single-direction drop | `DeliverDroppingNthInput` | #163 gap detect + repair |
| **simultaneous drop, both directions** | both, same tick | **#210's deadlock** — a one-way drop cannot express it |
| duplicate frames | `DeliverDuplicatingInputs` | #163 duplicate suppression |
| forged input | `TamperOneInput` | anchor cross-check + recovery |
| silence for N ticks | drop everything | `CeilingStall` — must HOLD, never conclude |
| long silence then resume | drop, then deliver | cold rejoin / `RebuildFromHistory` |

Two rules learned the hard way today:

1. **The simultaneous case is not optional.** #210 was invisible to every single-direction test in the
   suite, and a one-way drop *cannot* produce it: both peers must gap at once for the survivor to end
   up waiting on the loser.
2. **`ResultDraw` must never appear** from the netcode (owner ruling, 2026-08-16). Assert it, because
   it is the one outcome that used to make a broken run look finished.

### Runtime and shape

Two targets, or one with a `--long` argument:

- **gate run** (in `build.ps1`): a few thousand ticks, one seed, the full fault schedule. Seconds.
- **soak run** (manual / CI nightly): tens of seeds × tens of thousands of ticks, seeds derived from a
  counter so a failure reproduces exactly. Minutes.

`Date.now()`-style entropy is banned here for the usual reason: a soak that cannot be replayed from
its seed is a bug report you cannot act on.

---

## What it will NOT catch, and what to do about that

**Cross-platform nondeterminism.** Both peers run one binary, so identical floating-point behaviour,
identical struct layout, identical compiler. If the 2026-08-16 divergence is NDK-clang vs Apple-clang,
this harness will stay green through all of it.

That is still the right harness to build first, because it answers the question by **elimination**: if
the soak is green over millions of ticks with the full fault schedule, the phone divergence is not the
netcode logic, and the search moves to the sim's determinism across toolchains.

For that second question the tool already exists and should be used: the **flight recorder + `--recdiff`**
(`device-rig.ps1 -Action pullrec -Game rps`) compares two peers' `.rec` files tick by tick and names
the first divergent tick. Pair it with a deliberate cross-compiler check — build the same `rps_core`
with the NDK toolchain and with the host compiler, run identical input through both, and compare
`StateHash()` per tick. A one-tick difference localises to a function; 3000 ticks of drift does not.

---

## Where to start

1. Copy the `Outbox`/`Deliver`/`PlaceCampsAndStart` helpers out of `NetTests.cpp` into a shared header
   in that directory — both suites should use one copy, per the promotion rule (`NetTests` is the
   second consumer the moment the soak exists).
2. Get the clean-delivery loop converging for 10 000 ticks with two AIs. If that is not green, stop —
   everything else is noise until it is.
3. Add the fault schedule one row at a time, simultaneous-drop **first**, since that is the one with a
   known historical failure to reproduce.
4. Wire it into `build.ps1` at the short duration.
5. Only then re-open the attribution question on #210.

And the rule that would have saved this whole day: **after each assertion you add, break the code it
covers and watch it fail.** Three separate test files written today were green against deliberately
sabotaged implementations before that step was applied.
