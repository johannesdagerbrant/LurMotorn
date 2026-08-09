# Scope & Strategy

## Key Points

- **Entry point: SETTLED — engine owns the entry, game owns the tick, with GRANULAR per-feature
  opt-out.** (Decision 1, option A + user amendment.)
- Three distinct diseases, three distinct cures — do not conflate them.
- Promotion candidates split into **Tier 1 (relocation, ~2,200 LOC, near-zero design risk)** and
  **Tier 2 (genuine conflict risk, needs judgement)**.

---

## Decision 1 — Entry point ownership: SETTLED

**Choice:** Engine owns `android_main` / `UIApplicationMain` / `WinMain` in
`Modules/App/Platform/*`, written **once per platform**. The game supplies an `IGameApp`; inside
`Tick()` the game drives and calls engine facilities.

**User amendment (load-bearing):** *"if one game wants to do some weird unique thing related to
entry, that game should be able to opt out of the engine default version of the entry feature to
overwrite."*

So the override granularity is **per entry-step, not all-or-nothing**. The engine entry is a
sequence of named steps — create surface, install log sink, poll agent channel, handle background,
apply safe area, pump input, present — and a game overrides *individual steps* while inheriting the
rest. Example: RPS's iOS render-thread parking becomes an override of the **background** step, not
a reason to fork the whole main.

**Guard rail against Unreal-style hook sprawl:** an override must be justified by a real game need
*at the time it is added*, and the default must remain what most games use. **If two of three games
override the same step, the default is wrong — fix the default, don't celebrate the hook.**

**Why this does not violate review §8.** §8 defended *"each app's `main` owns the loop and calls the
engine; the game is the program."* What it objected to was a lifecycle object owning the **frame**.
Here the game still owns the frame — it surrenders only **process startup**, which was never game
logic. It is OS ceremony the game was forced to host because nowhere else existed.
Measured: in `RpsMain.mm` (1,572 lines), genuine game decisions are ~150 lines. **~90% of a phone
main is ceremony.**

Also note #43's existing sketch already carries `OnBackground` and `OnPeerAdopted` callbacks — the
strict-library position had already partially eroded in the repo's own design notes.

---

## The three diseases (framing that drives everything else)

| | Disease | Evidence | Cure |
|---|---|---|---|
| **A** | **Platform layer: 6 drifted copies** | ~11,600 LOC of (game × platform); ~6,000–6,500 mechanically duplicated. `AndroidVulkanSurface.cpp` byte-identical; `BleShim.kt` **299 lines apart**; 12+ one-sided bug fixes in **both** directions; the *same* policy question (BLE role override) answered `LUR_AGENT` in chess and `LUR_INTERNAL` in RPS | **Deduplicate.** Review §1 explicitly blesses this: *"deduplicating the radio layer is compression"* — opposite epistemic status from pre-abstracting |
| **B** | **Game-side: a one-way ratchet** | The two games barely duplicate each other (different genres). Each engine facility exists in **exactly one** game and never comes back. ~1,900–2,300 LOC stranded | **Promote** — but tier by conflict risk |
| **C** | **Reverse leakage: game concepts inside the engine** | `pApplicationName = "OnlyChess"` in the shared Vulkan backend (shipping inside RPS); engine's default BLE UUID **is chess's**; `Session::SendMove` chess-only API RPS never calls; **4 of 9 `ProtocolVersion` bumps are RPS features** | **Clean now** — gets harder later because game #3 inherits the wrong defaults |

---

## Decision 2 — Promotion tiers (user leaning: dedup + leakage + non-conflicting promotions)

User's criterion, adopted: *"promotions that I strongly believe will not need conflicting
implementations between games."*

### Tier 1 — no plausible conflict. Relocation, not abstraction. (~2,200 LOC)

| Facility | Now in | LOC | Why no conflict |
|---|---|---|---|
| Rollback coordinator + prediction | RPS/Net | ~600 | Already designed 2026-08-04. Chess = lockstep mode; the **mode selector** is the difference |
| Desync recovery + lost-frame repair | RPS/Net | ~230 | GUID-tie-break survivor replay is genre-neutral |
| CVar replication at a stamped tick | RPS/Net | ~130 | "Apply value V at agreed tick T" |
| `SimRunner` (sim thread + fixed timestep) | RPS/Runtime | 190 (~170 generic) | Parameterized: tick rate, catch-up cap, input fn |
| SPSC snapshot mailbox | RPS/Runtime | ~35 | It's a data structure |
| Bounded thread-safe inbox | **3 near-copies** | ~40 | Consolidating 3→1 is pure win |
| `AlphaAt` interpolation factor | RPS/Runtime | ~8 | One formula — **encodes the never-extrapolate law** |
| Deterministic CSR spatial grid | RPS/Core | ~90 | **Physics needs it as broadphase** |
| Nearest-feasible-point projection | RPS/Runtime | ~60 | **Physics needs it** — constraint projection |
| Correction smoother | RPS/View | ~110 | Physics may need *more* on top, not different |
| `CameraScroll` | RPS/View | 53 | Physics likely wants zoom — additive |
| Agent command parser | RPS/Core | 152 | Parser generic, **verb table is the argument** |
| Build-fingerprint gate | RPS/Net | ~25 | |
| `Fixed` `F()`/`FRound()` | RPS/Core | ~20 | Belongs in `Modules/Sim/Fixed.h` outright |
| Sprite batching / instanced draw | RPS/View | ~130 | Renderer facility in a game folder |
| GUI sub-layer ordering | RPS/View | ~90 | Renderer-level z-order concept |
| Gradient/disc mesh, `Hsv`/`Srgb`/`FlatMat` | RPS/View | ~130 | Pure helpers |
| RG8 shade+coverage upload | **both** | ~46/~90 | Near-duplicate already |
| "Time ago" formatting | Chess | 10 | |
| Store-key enumeration / sidecar | Chess | 148 | Generic persistence plumbing |
| Safe-area insets | RPS real, Chess stopgap | ~50 | **Actually disease A** — it's OS-supplied platform data |

### Tier 2 — genuine conflict risk (the actually-speculative set)

| Facility | Risk | Read |
|---|---|---|
| **Audio `SfxLibrary`** | **High** | Chess = *one-shot*: classify → variant w/ no-immediate-repeat → pitch/gain jitter. Physics wants **continuous, parameterized** sound (machinery hum, impact scaled by collision energy). Different shapes. → promote the one-shot variation picker only |
| **Opponent selector** | **High on row model, none on plumbing** | Chess = persistent async opponents, your-turn-first; RPS = one live peer + AI tiers. Both hand-roll the same widget-driving boilerplate; both independently learned to re-point by *semantic* selection. → promote rows-as-data model, not row semantics |
| **Dev console assembly** (~690) | **Low–med** | Devtools doctrine already decrees *no per-game hook*, so conflict is disallowed by policy. Real risk: RPS's numpad-first/colour-picker layout suits a phone RTS more than a desktop physics workflow |
| **Flight recorder unification** | **Med — recommend DON'T merge** | Not drifted copies: they record **different layers**. `Modules/Core` = *datagrams* (transport). RPS = *sim input + hashes* (replayable). Physics wants the second. → keep both, name the layer split, promote RPS's as the sim-layer recorder |
| **Alpha-step material LUT** (×5 sites) | — | **Do not promote.** It is a *workaround* for immutable renderer materials. Promoting cements the wart. → fix the renderer to allow mutable tints |
| **Agnostic W-L-D tally** | **Low** | Layouts differ essentially; both invented the same principle. A small primitive has no conflict |
| **Status banner mechanism** | **Low** | Mechanism generic, content per game |
| **Localization** | — | **Neither game has it.** Greenfield speculation — do not build |

---

## Decision 3 — Ratchet cure: SETTLED

**Chosen: (1) promotion pass per milestone, (2) `Docs/NewGame.md` as a forcing function.**
**Rejected: CI lint, engine-first rule.**

Rationale for the pair chosen: the promotion pass is the **only** cure that catches semantic leakage
and the *"built in RPS, needed by physics"* case a lint cannot see. `NewGame.md` doubles as
onboarding and as a test — anything game #3 needs that the doc can't point at is, by definition, a
missing engine feature.

**Residual risk, accepted:** both cures depend on discipline, and discipline is what already failed —
`"OnlyChess"` sat hardcoded in the shared Vulkan backend (shipping inside the RPS app) for weeks
because nothing mechanical was watching, and the ~600-line platform budget is aspirational.
**Mitigation:** `NewGame.md` carries an explicit *"the engine must not name a game"* rule, so the
check lands in front of a human at the moment it matters.

---

## Decision 3 — original context (kept for the record)

CI lint explained to the user: a CI script that **fails the build** on rule violation (grep for game
names in `Modules/`, enforce the ~600-line platform budget). Value: converts doctrine nobody can
violate accidentally into something mechanical — `"OnlyChess"` has been shipping inside the RPS app.
Limit: **catches strings, not meaning.** Would NOT have caught `ProtocolVersion` bumped for RPS
features, or `MaterialDesc::Gamma` existing only for chess. A floor, not a solution.

## Open Questions

- Which Tier 2 items make the cut.
- Which ratchet cures go in the plan.
- Netcode coordinator shape: templates vs interface (Tier 1, but the *how* is undecided).
