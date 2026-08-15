# The checked-in guidance had drifted — what was wrong, and what it cost

Frozen 2026-08-15 against `master` @ `d72478e`. History, not status.

The brief was "fix any inconsistency or outdated information or contradiction in issues and
markdowns, except the frozen journals." What follows is what was actually found, because the
*pattern* is more useful than the list: **the rationale in this repo has aged well; the paths,
layouts and instructions have not.** Read old prose for why; never for where.

## The architecture block was wrong in seven independent ways

`CLAUDE.md` and the root `README.md` carried near-identical module tables, both stale:

| Claim | Reality |
|---|---|
| `Modules/Pairing` | deleted (#200) |
| `Modules/DevConsole` | deleted (2026-08-01) |
| `Modules/Net` — "session, clock-sync" | `ClockSync` deleted; only `Session.h` remains |
| `Modules/Math` — "vec/mat/quat" | no `Quat.h`; `Vec` / `Mat4` / `Spring` |
| iOS — "Swift shim" | **no Swift anywhere in the tree**; ObjC++ throughout |
| nine modules absent | App, Audio, Core, DevGui, Hud, Platform, Save, Text, Trace |
| RPS and the `Desktop/` harnesses absent | both shipped |

`CLAUDE.md` is the file whose header says its instructions override default behaviour. It described
an engine that stopped existing weeks ago.

## The gate description was wrong in five places, all post-#195

`Tools/DeviceRig/README.md`, `device-rig.ps1`'s header, `device-rig.ps1`'s `$AndroidRole` comment,
both games' Android READMEs, and `Tools/BleDevRig/README.md` each said the on-device autoplayer is
`LUR_INTERNAL`. It is `LUR_AGENT` (#195), so it is **absent** from an ordinary build.

That one has teeth: it is precisely the "rig waits forever at `matches ended=0`" trap, and every
document a person would consult while hitting it told them the opposite.

## The tracker was violating its own rule

`CLAUDE.md` says: *never mirror another issue's open/closed status into prose — point to a live
query instead*, and cites #80 rotting as the reason. Tracker **#12** was doing exactly that, with a
hand-maintained checkbox list that had #44 marked open days after it closed. Epic **#39** had the
same pattern plus literal `#NN` placeholders never filled in.

Both now carry queries. #39 also gained the 7-phase table, which had only ever lived in the journal.

## An ambiguity nobody had named

**"Phase N" means two different ladders.** The roadmap phases (`phase-*` labels, #12) and the engine
extraction phases 0–7 (#39) are unrelated numbering. Extraction Phase 2 is BLE unification; roadmap
Phase 2 is "RTS on phones". Written down in `CLAUDE.md`, #12, #39 and the RPS README.

## Skeleton-era text still describing shipped features

`Games/Chess/Android/README.md` and `Games/Chess/iOS/README.md` still said "stub Vulkan renderer",
"BLE transport + Vulkan rendering are **stubs with TODOs**", "**No renderer / Vulkan / MoltenVK**
yet — graphics are deferred (issue #9)", and pointed at a `Sources/IosBleTransport.mm` that moved
into the engine in #42. The cited issue numbers were wrong too (#8 is rollback netcode, #9 is glTF).
The iOS build recipe targeted the **simulator**; CI builds a device `.ipa`.

The RPS README described the netcode as **lockstep**. It has been rollback since #8 — and the class
is still called `LockstepPeer`, which is worth a warning rather than a correction.

## A "verify this" note, settled

The RPS README asked a reader to check whether `macos-ci.yml` had been switched off
`-configuration Debug`. It has not: all five jobs still force it, so **every CI `.ipa` is `-O0`**.
Stated, with #198 named as the owner, instead of passing the question on.

## One contradiction in the code itself

`CreateBleTransport(EBleRole)` was ignored by both backends (`/*Role*/`) while all four mains passed
**contradictory** values — Android `Central`, iOS `Peripheral` — reading as if the platform assigned
the role. It does not; `DecideBleRole` tie-breaks at runtime from the two device GUIDs, which is
what keeps the peer relationship symmetric. Parameter removed; the enum stays (it is
`DecideBleRole`'s return type and the `LUR_AGENT` override's type). `Ble.h` also still claimed the
drivers "live in the app builds" and that "the pairing module selects roles" — one module deleted,
the other relocated in #42.

## The rule worth keeping

Every one of these was written true and went stale silently, because nothing forces prose to track
code. The two mechanisms that *did* hold up: **required compile definitions with no default**
(`LUR_LOG_TAG`, `LUR_BLE_SERVICE_UUID` — a game that forgets fails to build) and **tests**. Prose is
the weakest form of invariant in this repo; prefer a `#error` or a test whenever the choice exists.
