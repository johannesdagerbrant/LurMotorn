# LurMotorn Planning Docs — README & Agent Entrypoint

> **⚠️ The GitHub issues are the source of truth for WORK: tasks, bugs, epics, roadmap, current state.**
> Start at the roadmap tracker issue #12. This folder holds two kinds of documents, marked in the index:
> **LEGACY** — the original July-2026 planning synthesis, frozen, read for rationale/narrative only; and
> **LIVING** — design docs & feature specs, amended **in place** (with a dated changelog note) whenever a
> design decision changes, so they never rot the way state-in-docs does. Either way, when a doc disagrees
> with an issue on *sequencing, priority, or state*, **the issue wins**; when a decision changes the
> *design*, update the living doc — don't let the rationale live only in an issue thread.

*Produced July 16–17, 2026, against the repo at master `#38 fix: adopt a peer on reconnect too, and drive green off GetLinkState`. Written collaboratively (owner + Claude) across one extended planning session covering: BLE/Wi-Fi transport research, two full code reviews from deliberately different value systems, the design of game #2, and a master roadmap sequencing everything.*

---

## 1. Precedence rules (read this before anything else)

**The GitHub issues are the source of truth** for what to do and in what order. These documents are reference for *rationale*, not authority for *plan*. When they disagree, this is the order of authority:

1. **The GitHub issues (and their labels/state) are authoritative for *sequencing, priority, and current state*.** The roadmap tracker #12 indexes them by phase. If a doc here says "P1" or phases something one way but an issue says otherwise, **the issue wins**. (This reverses the original ordering — the `lurmotorn-master-roadmap.md` doc is now a legacy artifact, not the authority.)
2. **The two reviews are authoritative for *findings and rationale*** — what is wrong, where, and why. They were written against `#38`; before executing any fix, **re-verify the claim against HEAD** (the code may have moved).
3. **`rps-rts-design-spec.md` is authoritative for *game #2's design rationale***, including its wire format and deterministic rules — but its *sequencing* defers to the issues.
4. **The issue-draft files are *content* drafts, not priority lists.** Their acceptance criteria and technical detail seeded the real issues; the issues supersede them.
5. Where the reviews disagree with *each other*, that is intentional — Review #2 §8 contains the arbitration table.

## 2. Document index

| File | What it is | Status |
|---|---|---|
| `lurmotorn-master-roadmap.md` | Phases 0 → 5 + parked items; sequences every review item around the RTS as forcing function. Includes Phase 0.5 (Workbench: Windows platform, chess two-window, flight recorder, overlay, fuzz) and the hybrid Windows↔Android BLE rig as a parallel agent-delegable track. Ends with a complete disposition table — every review item has a phase. | **Legacy — read for the "why". Source of truth is the issue tracker (#12).** |
| `lurmotorn-code-review.md` | Review #1, architecture lens. Full audit: the engine/game seam (~1,620 lines of engine code inside `Games/Chess/`), chess leaks in `Modules/Net`, two P0 correctness bugs (Android BLE threading race; `Session::Send` 64-byte cap silently breaking >61-ply resync), §3.5 shared-first doctrine (added by owner request: per-subsystem scorecard, `BleLinkController`+`IBleRadio` design), module-by-module findings, prioritized table. | Authoritative for findings. Amended in place with §3.5; its §8 priorities are superseded by the roadmap. |
| `lurmotorn-review-2-handmade-lens.md` | Review #2, Handmade/data-oriented lens (Blow/Muratori school). Challenges review #1's abstraction-first order ("write game #2 before `IGame`"), fresh catches (zero compiler flags configured; 13 `std::function` sites; `std::string` GUIDs; `std::filesystem` set the iOS-13 floor; no on-disk format version; silent-guard error philosophy), and the iteration-loop agenda (desktop build, flight recorder, soak, fuzz, `LUR_ASSERT`). §8 = disagreement/arbitration table vs review #1. | Authoritative for findings; its roadmap (§7) is folded into the master roadmap. |
| `rps-rts-design-spec.md` | Game #2 (now **RocksPapersScissors**; v1 working title "Sten Sax Skog"). Open 2D field, raidable walking lumberjacks, annihilation win. Deterministic sim rules (tick phases, tie-breaks, Chebyshev movement — no sqrt, no floats), engine-gap table, chess↔RTS overlap table (§9 — this table *is* the future `IGame` contract), build slices 0–3. | **LEGACY** for §7 wire / §2–§3 unit cap / §6 entity layout (superseded, see next row); still authoritative for the *game design*. |
| `rps-rts-netcode-and-unit-system.md` | **v2 decisions (2026-07-19):** lockstep = input-stream authority; per-tick input-or-empty wire + 1-byte event codec (one codec: wire/resync/recorder); tick-denominated time, ceiling/sprint laws, never-discard-lockstep-ticks; chunked resync (blip = cold-rejoin, one mechanism); SoA POD unit arrays + raised unit cap + deterministic spatial grid; the double-buffered snapshot = thread seam + GPU instance buffer (one instanced draw, shader lerp). Parked items with wake conditions. | **LIVING** — amend in place as decisions change. Actionable state: issues #75/#76. |
| `epic-engine-extraction-and-hardening.md` | 1 epic + 10 issue drafts from review #1's roadmap, in `===== ISSUE =====` splittable format. | **Content valid, sequencing stale ×2:** written before the shared-first amendment (so it lacks issues for BLE unification, `Lur::Log`, and TouchEvent/input) and before the master roadmap re-phased everything. Use its bodies; take order and phase from the roadmap; draft the three missing issues from review §3.5 when their phase arrives. |
| `epic-coc-transport-and-stress-test.md` | 1 epic + 5 issue drafts: L2CAP CoC data channel (GATT-bootstrap PSM handoff, framing, capability routing) + device-pair stream stress test. | **PARKED** by the roadmap — the RTS provably doesn't need bandwidth. The hybrid rig partially wakes the measurement half. Wake condition: a game that needs streaming throughput. The technical content remains correct for that day. |
| `issue-captured-pieces.md` | Single chess feature issue: captured-piece trays above/below the board, order derived from the move list (no new stored state). Grounded in real symbols (`ChessRecord::Moves`, `PieceLight/Dark`, `ComputeLayout`). | Valid, unscheduled backlog — roadmap suggests Phase 3 as a palate cleanser. Has one open question (hot-seat/no-identity display). |
| `lurmotorn-networking-lexicon.md` | Glossary of every term/abbreviation used across the planning: BLE stack layers, GATT/L2CAP/CoC/PSM/PHY, platform APIs, transport alternatives, sync models (lockstep/rollback/snapshot), engine terms, tooling — plus a quick-reference numbers table for the test pair. Two 2025-era Wi-Fi Aware facts are flagged "verify". | Reference; no precedence role. |

## 3. Phase → document map (for planning a phase's work)

| Roadmap phase | Read | Existing issue drafts |
|---|---|---|
| 0 — fixes & floor | Review #1 §5.1, §5.2, §7 (Send cap), §5.3–5.4; Review #2 §3.5, §5 | Extraction epic: both P0 issues + core-hardening batch |
| 0.5 — Workbench + hybrid rig | Roadmap Phase 0.5; Review #2 §4 (entire section); Review #1 §3.5 (Log, TouchEvent context) | None yet — draft from roadmap + Review #2 §4 |
| 1 — RTS slices 0–1 | RTS spec §2–§7, §10; Review #1 §3.3 (minimal de-chess) | Extraction epic: de-chess-Net issue (minimal scope only) |
| 2 — RTS slice 2 (phones) | Roadmap Phase 2; Review #1 §3.1; wire/render polish batch | Extraction epic: platform-move issue + polish batch |
| 3 — balance + tidy | RTS spec §11; Review #1 §3.4 | Extraction epic: Pairing/docs issue; `issue-captured-pieces.md` |
| 4 — extraction | Review #1 §3.2, §4; RTS spec §9; Review #2 §8 (GameHost shape) | Extraction epic: GameHost + IGame issues (re-shape per R2 §8: toolbox, not framework) |
| 5 — deepening | Review #1 §3.5; Review #2 §3.1–3.2 | **Missing** — draft BLE-unification, `Lur::Log`, TouchEvent issues from Review #1 §3.5 |

## 4. How an agent should work from these documents

1. Read this README, then the master roadmap end-to-end. Identify the current phase (ask the owner if ambiguous).
2. For that phase, read the mapped sections above **and re-verify every code claim against HEAD** — file paths, line-ish references, and symbol names in the reviews were true at `#38` only.
3. Where issue drafts exist, use their bodies/acceptance criteria; file via the convention below. Where the map says "draft from …", write new issues in the same splittable format.
4. Honor the standing doctrines wherever code is written, regardless of phase: platform files hold API verbs, never decisions (R1 §3.5); new sim/state code is POD, fixed-capacity, assert-loud (R2 §3.3, §5); no chess (or RTS) concepts in `Modules/*`; the wire byte budget is a tested invariant, not a vibe.
5. The two P0 items precede everything. Do not start Workbench or RTS work on top of the known threading race.

**Issue-file convention:** blocks delimited by `===== ISSUE =====`; first line `Title:`; body is Markdown; epics created first, child issues reference `#NN`. Split-and-file with `gh issue create --title … --body-file …` from the repo root.

## 5. Decisions recorded here because they live nowhere else

Made during the planning conversation; no other document carries them:

- **Transport doctrine: BLE-only, seamlessness is a hard constraint.** The proximity-based "walk up and it connects" experience is product-defining. Wi-Fi *hotspot* handoff was investigated and **rejected** (per-session join prompts break the feel). **Wi-Fi Aware** is the future silent-upgrade tier only — it requires both ends, and the current Android test device doesn't support it.
- **Test pair:** iPhone 11 Pro (iOS 26-capable, Wi-Fi Aware-capable) + Samsung Galaxy A14 4G / SM-A145R (Android 13+, Vulkan on Mali-G52, **no** Wi-Fi Aware, L2CAP CoC available). Numbers for this pair: lexicon quick-reference table.
- **Minimum supported devices (as configured):** iPhone 6s / SE-1 (iOS 13 floor — set by `std::filesystem` availability, see Review #2 §3.1) and, practically, Galaxy S7-class (minSdk 26 + per-chipset Vulkan driver + BLE advertiser; budget chips pre-~2018 often lack Vulkan drivers regardless of OS).
- **Dev machine:** Windows laptop with built-in BLE — the hybrid rig needs no dongle. Before long rig sessions: disable the adapter's power management; before trusting rig throughput numbers: check the adapter's LMP version (9+ = BT5). (Also in roadmap Phase 0.5.)
- **Agent-driven testing is a first-class workflow:** the owner drives the Android device from Windows over adb wireless debugging; the hybrid rig and `dev-rig.ps1` are designed around that.
- **Game #3 candidate (crane co-op physics)** is parked behind the RTS + flight-recorder/desync tooling; it requires a deterministic 2D solver — its own project.

## 6. Known gaps & staleness (honesty section)

- The extraction epic predates two rounds of thinking (see its status row). The three §3.5-derived issues (BLE unification, `Lur::Log`, TouchEvent/input) were **never drafted** — deliberate, since their phases are 0.5+/5.
- Reviews cite line-ish locations valid at `#38`; treat every location as a hint, not gospel, after any commit.
- The lexicon's Wi-Fi Aware entries carry a "verify" flag (2025-era facts near the assistant's knowledge edge).
- The stress-test epic's numbers (throughput/latency envelopes) are literature-derived estimates; the hybrid rig / eventual stress harness produces the real ones.
- Review #1's §7 flag on the `Session::Send`/Sync overflow was reasoned from reading, not executed — the Phase-0 issue's first step is the failing test that proves it.
