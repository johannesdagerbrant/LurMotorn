# Journal batch — 2026-08-09

Frozen snapshots against `master` @ `22468e7`. **Nothing here is live status.** Per the repo's
precedence rule, the issues always win on anything current — sequencing, priority, state *or*
design. Read this batch for rationale; act from epic **#39** and its chain. Re-verify every code
claim against HEAD before acting on it: paths and symbols drift.

## Documents

| File | What it is |
|---|---|
| `engine-extraction-plan.md` | The full engine-extraction inventory, decisions and 8-phase plan, produced before starting game #3 (co-op physics puzzles) |

## Why this batch exists

Review #2 (2026-07-17 §1) deferred the extraction behind game #2 on purpose — *"the duplication IS
the specification for the engine"* — and the RPS shells were copy-pasted deliberately so that the
second consumer would earn #42 with evidence. Two games have now shipped in parallel for ~3 weeks
and the roadmap names this extraction as the physics game's prerequisite. This batch is the
measurement of that duplication and the decisions taken from it.

## The three findings worth remembering

1. **Three diseases, not one.** Platform layer = six *drifted* copies (dedup). Game side = a
   **one-way ratchet** where each facility exists in exactly one game and never comes back
   (promote). Engine = **reverse leakage** of game concepts (clean). Different cures; conflating
   them was the main risk to the plan.
2. **The drift is already causing correctness bugs, in both directions.** 12+ fixes exist in one
   game and not the other, and the *same* policy question (the BLE role override) was answered
   `LUR_AGENT` in chess and `LUR_INTERNAL` in RPS — so RPS ships a rig-controllable radio override
   in every build a player runs.
3. **~40–45% of `BleShim.kt` is decision logic, not transcription** — a hand-rolled send queue,
   watchdogs, role escalation and reconnect scheduling living in Kotlin where no host test can
   reach it. That is *why* it drifted.

## Standing doctrines this batch adds

- **Delete all dead code, even if it looks useful. Build it in the game that needs it; promote to
  the engine on the SECOND consumer.** Adopted 2026-08-09. Consequence: this deliberately creates
  game-first facilities, which makes the **promotion pass** load-bearing — it is the only mechanism
  that brings them back.
- **Engine owns the entry point, the game owns the tick**, with per-step override. This does not
  contradict review §8: what §8 defended was the game owning its *frame*, and ~90% of a phone main
  is OS ceremony, not game logic.
- **If two of three games override the same engine default, the default is wrong** — fix the
  default, don't celebrate the hook.
