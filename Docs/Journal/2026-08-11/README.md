# Journal batch — 2026-08-11

Frozen snapshot against `master` @ `6f450cc`. **Nothing here is live status.** Per the repo's
precedence rule the issues always win on anything current — sequencing, priority, state *or* design.
Read this for orientation; act from epic **#39** and its chain, and re-verify every path and symbol
against HEAD first (a great many moved on 2026-08-09/10/11).

## Documents

| File | What it is |
|---|---|
| `engine-extraction-status.md` | Hand-off report for a fresh agent: where phases 0–2 stand, the doctrines this work established, the traps that cost time, device ops, and what to do next in order |

## Why this batch exists

The extraction ran autonomously across 2026-08-09 → 08-11 and covered Phase 0 (#200, closed),
Phase 1 (#42) and most of Phase 2 (#197). The blow-by-blow record is
`Docs/Journal/2026-08-09/RUN-LOG.md` — that file is the *working* log, appended during execution.
This batch is the distillation: what someone needs to know to continue, without reading the whole
run.

## The three things worth remembering

1. **Failures here are silent and usually blame the radio.** A default BLE UUID inherited from
   another game, a data race that presents purely as latency, a log narrating radio restarts against
   a transport that implements none — each looked like a link problem and was not. The status report
   carries the full table.
2. **The duplication was hiding real defects, in both directions.** Collapsing six BLE backends to
   three surfaced: one game shipping a rig-controllable radio override to players, another with no
   recovery from a wedged stack, a role decidable from a failed GATT read, and an advertise retry
   that only one game had. None were disputed — they were simply in the other file.
3. **A green automated gate is not proof of absence for a timing bug.** The slow-moves regression
   passed an autoplay load test at 49 ms and was reported by a human playing. Both instruments are
   real; the human one caught what the harness could not.

## Standing doctrines this batch adds

- **Required build parameters, never defaults**, for anything genuinely per-app (`LUR_LOG_TAG`,
  `LUR_BLE_SERVICE_UUID`). A default relocates the mistake instead of removing it.
- **Read across the seam rather than duplicating**; a "MUST match" comment is a duplication
  maintained by hope.
- **A false-by-default capability query needs an audit of every implementor**, not just the one that
  lacks the feature.
- **A missing capability is a finding, not a silence** — say it once, rather than logging repairs
  that never happened.
