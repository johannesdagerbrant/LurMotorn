# 2026-08-03 — RPS lockstep → rollback: the responsiveness experiment

Frozen snapshot of the discussion that turned a `Fixed`/Box2D determinism question into a concrete
netcode experiment: replace RPS's delay-based lockstep with rollback to kill the felt 300 ms input
delay in linked play, and use the result to inform whether a future co-op physics puzzle game gets a
forked Box2D or a hand-rolled `Fixed` physics engine. **Live status lives in the issues** — this
batch is history; re-verify code claims against HEAD before acting.

- **[rps-rollback-refactor-plan.md](rps-rollback-refactor-plan.md)** — what sparked it (the
  `Fixed` → Box2D-has-no-rollback → physics-puzzle-game chain), what we're testing (does rollback make
  linked RPS feel like solo), what we want to measure (responsiveness, correction frequency/magnitude,
  resim cost, determinism preserved), and the phased implementation plan for the refactor.

## One-line conclusion

The linked-vs-AI delay is real **latency** (place/queue executes at `W + 3 ticks` = 300 ms, on the
player's own action, because lockstep applies all input at `W+Delay`), not a feedback gap — so the
test is a genuine **rollback refactor** of `LockstepPeer` (predict + re-simulate, snapshot = `memcpy`
of the POD `Sim`), replacing lockstep outright (git keeps the old model), verified on two phones. RPS
is the cheap proxy that isolates netcode-feel from the fixed-point-physics-authoring risk of the
eventual puzzle game.
