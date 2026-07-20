# LurMotorn Planning — README v2 (Agent Entrypoint)

*2026-07-20. Supersedes the previous README.md in `Docs/Planning/`. Written against the repo at `d1dbb76` (RPS shipped, playtested; Workbench + DeviceRig operational). This file plus the seven documents listed in §2 are today's batch; everything else referenced is **already committed** in `Docs/Planning/` — do not re-copy earlier files over possibly-evolved repo versions.*

---

## 1. Precedence (when documents disagree)

1. **The answered decision sheet** (`rps-decision-sheet.md`, header block) — owner rulings, 2026-07-20. Nothing overrides these.
2. **The living design doc** (`rps-rts-netcode-and-unit-system.md`, already in repo) — authoritative for RPS design + netcode. It long ago superseded the historical `rps-rts-design-spec.md` (which still says lumberjacks/trees; the game shipped miners/gold/mines).
3. **The two new execution plans** (`rps-android-perf-stability-plan.md`, `rps-boid-flocking-plan.md`) — authoritative for their problem areas, verified against the code at `d1dbb76`.
4. **Issue files** — per the house doctrine, **where an issue and a plan disagree, the issue wins.** Live example: boids **slice B** was re-decided to implicit Verlet velocity (`Pos − Prev`, decision #1) — the issue reflects this; the plan doc's §3 still describes explicit `Vel` arrays. Trust the issue.
5. **Master roadmap** (`lurmotorn-master-roadmap.md`, in repo) — macro sequencing. Status as read from the tree: Phases 0/0.5 (fixes, Workbench, hybrid rig) and 1–2 (RTS through phones) are **executed**; Phase 3 (balance/tidy) is **in progress — this batch is its work plan**; Phase 4 (GameHost/IGame extraction — #42, now "earned") and Phase 5 (BLE unification) remain ahead.
6. **The two code reviews** (in repo) — authoritative for findings/rationale, written at `#38`. The repo has moved a lot since: **re-verify every code claim against HEAD** before acting on one.
7. Historical/parked: old RTS spec (superseded), CoC epic (parked, wake conditions in the roadmap).

## 2. Today's batch (this bundle)

| File | What it is | Status |
|---|---|---|
| `README.md` | This file — replaces the old planning README | drop-in |
| `rps-decision-sheet.md` | 11 owner decisions **with answers recorded** in the header (incl. 8B big-battle direction + the cart-trek evidence; #9 worker-flee struck from all planning) | authoritative rulings |
| `rps-android-perf-stability-plan.md` | Sluggish input / bad FPS / crashes: 4 verified mechanisms (unwired #69 SimRunner; `-O0` everyday installs; uncapped `Execute`/synchronous `RebuildFromHistory` → ANRs; assert-traps under thermal) + ranked hypotheses + instrumentation-first plan | execute via its issue file |
| `issues-android-perf.md` | Epic + 8 children in dependency order; the #86 execution breakdown. All decision gates resolved (approved) | **file first** |
| `rps-boid-flocking-plan.md` | Soldier flocking design: diagnosis of the bundling, one-gather-pass force stack, house determinism rules | execute via its issue file (slice B: see precedence note above) |
| `issues-boid-flocking.md` | Epic + slices A–D; A is hash-neutral, B is the Verlet variant + `Prev` joins `StateHash`; parked items + wake conditions in the epic | file second |
| `rps-balance-playbook.md` | #84 method: strategy-vs-strategy matrix via `InputFn` schedules + replay-extracted metrics + pass/fail gates; updated for decisions 6B/7B/8B (incl. the new cart-trip metric and the comeback-lever deliverable) | build the harness, then run |
| `issue-economy-scaleup.md` | Single issue: mine **density** first, then total gold — the #84 opening move, from the 2026-07-20 playtest evidence | file with the balance work |

## 3. On arrival — do these in order

1. **Commit this batch** to `Docs/Planning/` (issue files under `Docs/Planning/issues/` per convention). Record the decision-sheet answers as a living-doc changelog entry, and confirm no active doc still lists worker-flee (decision #9: struck).
2. **File the issues** (`===== ISSUE =====` blocks; epics first, paste real numbers into children): perf epic, then boids, then economy scale-up.
3. **Execute Perf 0 immediately** — the half-day zero-code triage (optimized-build A/B + crash-log classification). Its findings are recorded in the perf epic *before* any other perf child proceeds; they may reprioritize.
4. Then, in parallel lanes:
   - **Perf lane:** 1 → 2 → 3 → 4 → 5 → 6 → 7 (dependency order in the epic; 5 and 6 are pre-approved).
   - **Boids lane (desktop-first):** slice A after Perf-1's instrumentation exists (its stress-budget acceptance reads those numbers); then B → C → D.
   - **Balance lane:** build the matrix harness + baseline report, then run `issue-economy-scaleup` as the opening move; the comeback-lever choice (a/b/c in the playbook deliverable) is the one remaining owner micro-decision — ask when it blocks.
5. Boid slices and economy changes are **sim changes**: build-locked as usual (both phones same build; anchor hash alarms mixed builds), never wire-format changes. Slice B and the mine raise are the two deliberate `StateHash` changes — call each out in its commit + changelog.

## 4. Standing doctrines (unchanged, enforced everywhere)

Platform files hold API verbs, never decisions · new sim state is POD, fixed-capacity, assert-loud, in the pinned hash order · order-independent neighbor sums over start-of-tick state, one factored `Add*` helper shared by brute + grid paths, grid≡brute equivalence tested per force · no float/sqrt/allocation in the tick · byte budgets are tested invariants · instrument before optimizing · issues win over plans · every shipped knob change gets a living-doc changelog line.

## 5. Verification rule

Plans in this batch cite `Sim.cpp`/`RpsMain.cpp` line-ish locations true at `d1dbb76`; the reviews cite `#38`. Treat every location as a hint after any commit — re-verify against HEAD before editing. When a verified fact in a plan turns out stale, update the plan doc in the same PR (the docs are living, like everything else here).
