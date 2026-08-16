# 2026-08-16 — Phase 3 finished, then eight hours on two phones

Frozen snapshot. Everything here was true against `9668799` on the date in the folder name; nothing in
`Docs/` is durable, and the issues win on anything current (CLAUDE.md, *Planning & project state*).

29 commits. The engine-extraction Phase 3 (issue #43) went from "section C not started" to all six
sections done, then the whole thing went onto hardware and the device pass turned out to be worth
more than the refactors it was checking.

| file | what it is |
|---|---|
| `01-session-log.md` | what was done and, more usefully, what each thing cost to find |
| `02-lessons.md` | the ones that generalise. Read this one if you read nothing else. |
| `03-two-peer-soak.md` | how to build the harness that does not exist and should |

## The one-paragraph version

Phase 3 sections C–F landed and are device-verified. The device pass found two real iOS bugs the host
suite structurally could not see (`multipleTouchEnabled` was never set anywhere in the tree, so the
iPhone was physically incapable of the two-finger console gesture; and `LurRebuildViewHost` reset the
replacement view to UIKit defaults, which would have silently undone it on any #73 heal). The iOS
power-cycle fix `b219d2d` was verified working. An owner ruling removed the netcode draw entirely. A
real history-exchange deadlock was located from a hardware capture and fixed. And underneath all of
it sits an **unattributed divergence** — the two phones re-diverge every 10 ticks after every
successful recovery — which cannot currently be diagnosed, because **no harness exists that runs two
lockstep peers headlessly through a real match**. That harness is `03-two-peer-soak.md` and it is the
most valuable thing on this list.

## Open when this was written

- **#210** — the divergence above, plus the reconnect livelock. The blocking netcode issue.
- **#43** — Phase 3 complete; `SimRunner` promotion deliberately held (one consumer, and the rule is
  to promote on the second).
- Owed device checks: the #73 heal, rotation, and the `Lur::Save` **write** path (needs a match to
  finish so a score persists).
