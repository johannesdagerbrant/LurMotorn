# RPS rollback — outcome, and a game/platform-agnostic responsiveness workflow

Frozen snapshot, 2026-08-04. Follow-up to `Docs/Journal/2026-08-03/rps-rollback-refactor-plan.md`
(the plan) — this records what actually happened when we built and shipped it on two phones, the
durable learnings, and how they inform (a) the co-op physics puzzle game decision and (b) a shared,
game- and platform-agnostic networking + responsiveness workflow. **Live status lives in the issues**;
this is history — re-verify code claims against HEAD before acting.

## One-line outcome

Rollback replaced delay-based lockstep in RPS and was verified on an Android↔iPhone pair: **`desync=0`
across a full ~5076-tick match, corrections rare (~0.5–1/s) and cheap (~1 re-simulated tick each),
peer-command lag ~1 tick** — "more responsive than lockstep," and after a focused immediacy pass,
"smooth and snappy." The rollback machinery (snapshot ring + prediction + roll-back + correction blend)
is now built, proven over real BLE, and ready to carry to the physics game. **The single most important
finding for everything downstream: once the netcode is at its floor, responsiveness is a *layered
latency-budget* problem, and every lever that helps lives in the sim/net path — never in the renderer.**

## What we set out to test, and what we found

The plan's thesis: prove rollback *feel* on RPS's cheap, already-deterministic sim before betting the
physics game on the technique, isolating the netcode-feel risk from the fixed-point-physics-authoring
risk. RPS is the *friendly* case (sparse human input, interaction-local authority), so a clean result is
necessary-but-not-sufficient for the physics game and a bad result would be nearly decisive against.

Result: **clean.** And the friendly-case assumptions held exactly as predicted — sparse input meant the
"peer did nothing this tick" prediction was right the large majority of ticks, so most speculated ticks
never rolled back, and the ones that did re-simulated a single tick. That is the crux caveat for the
physics game (below).

## What shipped (the ladder — kept vs reverted)

Rollback core (the plan's Phases 1–5, all on `master`, host-green, two-phone verified):

- **Snapshot ring + peer predictor + confirmed-tick** — `memcpy` ring of the POD `Sim`, "peer idle"
  prediction, confirmed = highest tick both peers' real input is known.
- **Execution model replaced** — apply local input at the head immediately (the 300 ms scheduling delay
  is gone), speculate the peer forward, roll back + re-simulate on a misprediction. `ProtocolVersion`
  8→9 (wire frame identical; execution semantics changed, so a rollback build refuses a lockstep one).
- **Correction smoothing (View)** — ease a rolled-back unit into its corrected position; provably
  dormant in normal play (the sim sets `Prev=Pos` each step, so the snapshot chain `Pos_N == Prev_{N+1}`
  holds and the offset is exactly 0 unless a rollback actually happened).
- **Safety nets reconciled** — anchors hash *confirmed* state only (a speculative hash could be rolled
  back and would false-desync the peer); `#161`/`#163` recovery and `#162` ceiling-stall carry over
  (they replay input history → model-independent); resync re-bases onto the confirmed frontier.

Then an immediacy pass, targeting two *distinct* latency chains the player named — (A) tap→own-screen,
(B) tap→opponent-screen. What survived and what did not is itself the lesson:

| Change | Verdict | Why |
|---|---|---|
| **queue-on-press** (register input on press-down, not release) | **KEPT** | Earliest input edge; shaves the whole press duration. No downside (a button press *is* a queue). |
| **wire-only send-early** (send the frame ahead of the wall-tick boundary, but keep local execution on-grid) | **KEPT** | Peer gets the input ~50 ms sooner (B) with no local render change — the batch is *sent* early but *parked* and only simulated when its wall tick arrives. |
| **BLE tuning** | already optimal | write-without-response (#49), connection-priority HIGH (#68), MTU 247. iOS-central interval is Apple-locked. Nothing to do. |
| **render extrapolation** (show `Pos + velocity·alpha`, the predicted-now position) | **REVERTED** | Predicts ahead every frame → overshoots at every velocity discontinuity (carts hard-stopping/reversing snap back = jitter). Unfixable from the view. |
| **produce-ahead** (advance the *local* sim a tick early on input) | **REVERTED** | Borrows a tick then skips one at the next boundary = a ~100 ms window with no new sim state = a render freeze. queue-on-press made it fire constantly = jitter. |
| **tick-rate increase (10→20 Hz)** | deliberately skipped | Would rebalance every tick-denominated tunable + double sim cost. A real option, not a tweak — out of scope for a feel pass. |

Net final state: rollback + queue-on-press + wire-only send-early + plain Verlet interpolation. Smooth
and snappy, `desync=0`.

## Measurements (real hardware, Android↔iPhone, one ~5076-tick / ~8.5-minute match)

- **Rollbacks:** Galaxy 374, iPhone 248 → ~0.5–0.7/s. Rare, exactly as the sparse-input argument
  predicted.
- **Re-sim cost:** **exactly 1.00 tick per rollback.** The head only ever ran ~1 tick ahead of the
  confirmed frontier, so a correction rewinds 1 tick and replays 1. The snapshot-ring / rewind cost is
  effectively free, and correction *magnitude* is bounded to ≤1 tick of slow-unit movement (≈sub-pixel —
  which is why corrections were invisible without even needing the smoother).
- **Peer-command lag (`spec` = head − confirmed):** avg ~1 tick, peak 2 (100–200 ms). That is the
  10 Hz + BLE floor; the netcode is not the limiter.
- **Determinism:** `desync=0` across 600+ re-simulations between the two phones — the strongest
  determinism evidence we have. Fixed-point + fixed-timestep + POD state held under heavy resim.

`sizeof(Sim)` = 194 KB; the ring is `RollbackHorizon+2` = 18 snapshots ≈ 3.4 MB, one heap allocation,
`memcpy` per tick — noise on a phone.

## Durable learnings (the generalizable principles)

1. **Snapshots ARE the rewind — not an alternative to it.** A forward-only state machine can't be
   un-stepped; "rewind to tick T" is only implementable as "restore a saved state at T." The requirement
   this imposes is the whole discipline: **sim state must be POD / trivially-copyable** (`memcpy`
   snapshot, one-pass hash), enforced by `static_assert(std::is_trivially_copyable)`. No pointers, no
   heap, no floats in sim state.

2. **Responsiveness is a layered latency budget; attack each layer at its own layer.** tap→own-screen and
   tap→opponent-screen are separate chains, each = input-sampling (tick granularity) + sim/net path
   (rollback + send) + render (interpolation). Decompose before optimizing; a lever aimed at the wrong
   layer backfires.

3. **All responsiveness belongs in the sim/net path. The renderer must only INTERPOLATE, never PREDICT.**
   This is the day's hardest-won lesson. Render-side motion prediction (extrapolation) overshoots at
   every velocity discontinuity and cannot be fixed from the view (the stop is unknowable at the last
   moving frame). The renderer's contract is: faithfully interpolate the sim's (Verlet-integrated) state,
   `mix(Prev, Pos, alpha)`. Immediacy comes from getting real input into the sim sooner and to the peer
   sooner — not from guessing where things will be.

4. **Send off-grid; execute on-grid.** The renderer assumes a steady tick cadence, so producing sim ticks
   off the wall-clock grid (to apply input early) creates freezes/hitches. The fix that keeps both
   immediacy and smoothness: decouple the *wire send* (which may run ahead) from *local execution* (which
   stays on the fixed-timestep grid). Wire-only send-early does exactly this.

5. **Rollback cost is governed by the game, not the netcode:** cost ≈ (correction frequency) × (resim
   depth) × (sim-step cost). RPS wins on all three (sparse input → rare, ~1-tick head lead → shallow,
   trivial sim). Each is a *game* property. This is the lens for judging any new game.

6. **Under rollback, detection/recovery is no longer simultaneous across peers** — they reach the *same
   verdict* a beat apart (independent wall clocks). Design for **consistency, not synchrony**: the
   outcome must match on both screens, the timing need not.

7. **When a change makes things worse, remove the thing you added — don't add compensating state.** The
   extrapolation regression was chased with velocity buffers; the right move was to delete the
   extrapolation. (Recorded as a working principle.)

## What this means for the co-op physics puzzle game

The plan's open decision was **fork Box2D** (float; cross-platform-deterministic as of 3.1 but *no*
rollback snapshot API, and a large hidden solver state to snapshot) vs **hand-roll physics in `Fixed`**
(determinism and a trivially-correct `memcpy` snapshot fall out by construction). Today's result
**strengthens the hand-rolled `Fixed` case, decisively:**

- The rollback stack is **built and proven over real BLE**, and it is entirely game-agnostic in spirit
  (it operates on ticks, POD snapshots, input history, and hashes). Reusing it costs the physics game
  ~nothing on the netcode axis.
- Fixed-point + POD state gives the snapshot **for free** and gives determinism **by construction** —
  precisely the two things Box2D would fight us on (float determinism is per-toolchain fragile, and
  there is no snapshot API to `memcpy`). Forking also breaks the no-libraries rule.
- So the physics game's *netcode* risk is retired. The **remaining new risk is a single, isolated SIM
  problem: authoring a stable fixed-point constraint solver** — which is exactly the risk we set out to
  isolate from netcode-feel, and now have.

But RPS was the *friendly* case, and the physics game is the adversarial one on every axis that drives
rollback cost (learning #5). Concretely, before committing:

- **Correction frequency ↑↑.** Continuous, direct manipulation (dragging a crane/machine) means the
  peer's input mispredicts on *many* ticks, not a few per second. Expect frequent rollbacks.
- **Correction magnitude ↑↑.** Densely-coupled bodies mean one corrected input moves *many* bodies —
  bigger, more visible pops. Correction smoothing (dormant in RPS) becomes load-bearing, and the
  render-only smoother may not be enough → the plan's diegetic masking ("machines rev up," a settle
  animation) is the informed fallback, and should be prototyped alongside.
- **Resim depth × step cost ↑↑.** A physics step is far heavier than RPS's, and dense input means a
  deeper head lead. The worst-case `resim_ticks × step_cost` must fit the frame budget — this is the
  number to measure first on the `Fixed` prototype, because it, not determinism, is what could make
  rollback infeasible.

**Recommendation:** hand-rolled `Fixed` physics + the shared rollback stack; prototype the constraint
solver and *immediately* measure correction frequency/magnitude/resim-cost under continuous two-hand
manipulation, with the diegetic-masking fallback ready. If corrections read badly even after smoothing,
that is the signal to lean on masking rather than abandon rollback.

## The generalized workflow: one networked-deterministic-sim + responsiveness stack

Goal (the user's ask): the physics game **and** chess share the same code and are each as fast as they
can be. Today's rollback lives in `Games/RocksPapersScissors/Net/LockstepPeer` and knows `Rps::Sim`
directly — it is proven but not yet shared. The generalization:

### 1. A game-agnostic rollback coordinator in `Modules/Net`

Lift the coordinator out of the game and parameterize it on what the game provides. The coordinator owns
everything that is pure tick/hash/input bookkeeping — **all of it game-agnostic today already, just
entangled with RPS types:** the snapshot ring, the "peer idle" predictor seam, confirmed-tick, the
speculate/roll-back loop, confirmed-only anchors + cross-check, `#161`/`#163`/`#162` recovery, resync,
and the per-tick wire framing (batch + low-byte sequence).

The game supplies, behind a small interface (or as template parameters):

- **`Sim`** — POD / trivially-copyable state, with `Step(Sim&, const Input* combined, int n)`
  (deterministic, fixed-timestep) and `uint64_t StateHash() const`. The `static_assert(is_trivially_
  copyable)` is the contract that makes the ring a `memcpy`.
- **`Input`** — a POD event, a batch codec (encode/decode), and the trivial predictor ("no input").
- **Tunables** — tick rate, rollback horizon, send-lead.

RPS becomes a thin instantiation (`Sim = Rps::Sim`, `Input = Rps::InputEvent`). Chess supplies its board
`Sim` + move `Input`. Same coordinator, same wire, same recovery, same determinism verification.

### 2. Execution modes on one coordinator

The coordinator supports both **rollback** (speculate + roll back — for real-time games: RPS, the
physics game) and **lockstep** (confirmed-only advance — the degenerate case for turn-based/latency-
tolerant games: chess). Chess doesn't *need* rollback, but it gets the shared transport, session, wire,
recovery, and determinism harness for free, and picks the mode that fits. "Fastest it can be" for chess
is dominated by the transport + not-round-tripping, both already shared; rollback would buy it nothing,
and the mode selector says so explicitly.

### 3. The responsiveness workflow (game- and platform-agnostic), as a checklist

1. **Sim:** deterministic, fixed-point (`Modules/Sim::Fixed`), fixed-timestep, POD state. No floats in
   sim state, no heap in the tick. (`Modules/Math` floats are render-only.)
2. **Netcode:** the shared coordinator — speculate + snapshot + roll back; anchors on *confirmed* state;
   recovery by input-history replay. Game-agnostic.
3. **Input:** register on the earliest gesture edge the semantics allow (press-down for a discrete
   action); **send on the wire immediately** (off-grid), **execute on the fixed-timestep grid**.
4. **Render:** interpolate the sim's Verlet state (`mix(Prev,Pos,alpha)`); **never extrapolate**;
   correction-blend eases the rare rollback jump and is 0 otherwise. Platform-specific backend
   (Vulkan/MoltenVK), shared *policy*.
5. **Transport:** game-agnostic `ITransport`/BLE, tuned once for latency (write-without-response,
   connection priority, MTU) — shared.

### 4. Platform-agnostic by the existing architecture

The engine already isolates the three hardware-touching seams (transport, render, input) behind common
C++ interfaces, and the sim/net/serialization core compiles identically on host/Android/iOS. The
rollback coordinator is pure core C++ → it inherits that portability with no per-platform work. The only
platform-specific pieces are the render backend (shared interpolation *policy*, per-OS Vulkan) and the
transport backend (already shared interface, already tuned).

## Open questions / next steps

- **Physics `Fixed` prototype:** measure `resim_ticks × step_cost` under continuous two-hand
  manipulation against the frame budget — the go/no-go number for rollback on the physics game.
- **Correction magnitude under dense coupling:** does render-side smoothing suffice, or is diegetic
  masking required? Prototype both.
- **Refactor `LockstepPeer` → `Modules/Net` coordinator** parameterized on `Sim`/`Input`; re-point RPS
  onto it; evaluate chess on it (lockstep mode). Sequence and priority live in the issues.
- **Tick rate** remains the untried big lever for raw responsiveness; revisit only with a rebalance plan.
