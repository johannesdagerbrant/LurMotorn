# 2026-08-04 — RPS rollback shipped + the agnostic responsiveness workflow

Frozen snapshot of the day RPS's delay-based lockstep was replaced with **rollback** and verified on two
phones, plus the immediacy pass that followed and the synthesis of what it all teaches. **Live status is
in the issues** — this batch is history; re-verify code claims against HEAD.

- **[rps-rollback-outcome-and-agnostic-responsiveness-workflow.md](rps-rollback-outcome-and-agnostic-responsiveness-workflow.md)**
  — the outcome (rollback works over BLE, `desync=0`, corrections rare + cheap), the durable learnings
  (snapshots ARE the rewind; responsiveness is a layered latency budget; the renderer must interpolate,
  never predict; send off-grid but execute on-grid), what it means for the co-op physics puzzle game
  (hand-rolled `Fixed` physics + the shared rollback stack; the remaining risk is an isolated fixed-point
  solver, not netcode), and how to generalize it into one game-/platform-agnostic
  networked-deterministic-sim + responsiveness stack that RPS, the physics game, and chess all share.

Builds on `Docs/Journal/2026-08-03/rps-rollback-refactor-plan.md` (the plan this executed).
