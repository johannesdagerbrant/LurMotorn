# Phase 3 (#43) — status report, 2026-08-12

Frozen against `master` @ `864a3c2`. **Nothing here is live status.** Per the repo's precedence rule
the issues win on anything current — sequencing, priority, state *or* design. Read this for
orientation and rationale; act from **#43** (and its parent epic **#39**), and re-verify every path
and symbol against HEAD before relying on it.

Scope: this batch covers the day's work, `64f70b5 … 864a3c2` (9 commits, 23 files, +611/−241). The
evening of 2026-08-11 — the two-phone soak, #202, #203, #204, and section A's first landings — is in
`Docs/Journal/2026-08-09/RUN-LOG.md` (the working blow-by-blow, appended through 08-11 20:53) and in
`Docs/Journal/2026-08-11/engine-extraction-status.md`.

---

## 1. Where Phase 3 stands

`Lur::App::GameHost` (`Modules/App`) owns the session + persistence choreography. **Five of six mains
use it**: both chess phone mains, both RPS phone mains, and the chess desktop workbench.

| Main | GameHost | Notes |
|---|---|---|
| `Games/Chess/Android/…/AndroidMain.cpp` | yes | |
| `Games/Chess/iOS/Sources/AppMain.mm` | yes | |
| `Games/RocksPapersScissors/Android/…/RpsMain.cpp` | yes | no record sync (opt-in half unused) |
| `Games/RocksPapersScissors/iOS/Sources/RpsMain.mm` | yes | no record sync |
| `Games/Chess/Desktop/DesktopMain.cpp` | yes | two hosts in ONE process |
| `Games/RocksPapersScissors/Desktop/DesktopMain.cpp` | **no, deliberately** | see §5 |

Section B (platform absorptions + entry points) is **partly done**: three absorptions landed, the
entry points themselves have not started. §6 has the ordered remainder.

---

## 2. What landed today, and why each thing exists

### `64f70b5` — the persistence I silently broke, and the guard against it

Section A's four mains called `View.AttachPersistence(&Host.Sync(), …)` **before**
`EnableRecordSync()`. `Sync()` returned a null `unique_ptr` deref; the view's own
`if (Sync != nullptr)` guard then skipped `SyncManager::OnLink`; without `OnLink` the peer **key** is
never set; and `Persist()` writes under that key — so it no-opped for the life of the process while
every log line, including the MATCH END tally, still read as success.

**Found by a file mtime, not by a log or a test.** Three fixes, because one wasn't enough: reorder the
mains, make `GameHost::Sync()` `LUR_ASSERT` (a silent null deref must become a loud trap), and have
the host call `OnLink` itself — ownership of persistence and ownership of the key belong together.
Plus a test that asserts **bytes on disk**, which nothing had ever done.

Writing that test exposed test pollution too: temp dirs surviving between runs, then one test's
`OnSync` persisting a record the next test adopted. Every case now gets a wiped dir keyed on
`__func__`.

### `4f3bb51` — an agent-only build break, and the compile-check that missed it

A rename of `State.Session` missed `S.Session` inside `#if LUR_AGENT`, and I "verified" with plain
`assembleDebug`, which compiles that block out. **A compile check that doesn't compile the code under
test is not a compile check** — agent-gated code needs `-PlurAgent=ON`.

### `519736f`, `7869874`, `22396f5` — section B: the log sink and the stdio guard

`Lur::App::Platform::InstallLogSink()` and `UnblockStdio()`, one implementation per platform
(`Modules/App/Platform/{Android,Ios}/AppPlatform.*`).

The headline is not the dedup. **Chess never installed an engine log sink on either phone**, so every
`Lur::Log::*` line from inside the engine went to a stdout nothing reads. That same absence cost a
real diagnosis on RPS (2026-07-30): #112's build-fingerprint gate fired, reported through
`Lur::Log::Error`, and the line went nowhere; the pair then played 13 minutes and desynced with "were
the builds even the same?" unanswerable. RPS grew a sink in response, in two copies; chess got none.

Neither verb takes a tag — both read `Lur::Core::LogTag` from `LUR_LOG_TAG` (#42/#200, required, no
default). The two hand-written sinks they replaced had **both hardcoded `"OnlyRps"`**.

`UnblockStdio` was duplicated verbatim in both iOS mains including all sixteen lines of the
`0x8BADF00D` postmortem above it. Every MoltenVK app has that exposure; it is not a game's business.

The two follow-up commits are self-inflicted CI round-trips — see §4.

### `cee2018` — the #73 reattach heal, written once

`LurRebuildViewHost(vc, viewClass)` in `Modules/App/Platform/Ios/IosViewHost.mm` owns the UIKit half:
scene pick, old-window detach, fresh view + `CAMetalLayer`, fresh scene-attached window. Both iOS
mains had ~45 near-identical lines of it, **including two ordering fixes that took rounds on device**:

1. detach the OLD window FIRST — it still holds `rootViewController`, and its later dealloc rips that
   VC's view out of whatever window hosts it *by then*, which re-unhosted the fresh view and made the
   heal loop every 2 s;
2. attach to an EXPLICIT `UIWindowScene` — `initWithFrame:` relies on legacy adoption into the
   implicit scene, exactly what the broken launch never does.

Duplicated fixes of that shape are the argument for section B by themselves: a third game re-derives
them, or more likely re-lives them.

The **renderer** half stays game-side because the two genuinely differ — chess re-inits inline on the
main thread, RPS must park its render thread (#183) and keep the outgoing view alive because the old
`VkSurfaceKHR` wraps its layer and `vkDestroySurfaceKHR` runs later. Reaching the delegate's `window`
through `UIApplicationDelegate`'s `@optional` property is what let the shared code stop knowing game
types.

`LurHasConnectedWindowScene()` exists because RPS checked for a scene **before** parking its render
thread; folding that check into the shared call would have stalled rendering up to a second on every
2 s retry of a heal that isn't possible yet. Callers still handle a nil rebuild — the scene can vanish
between the two calls.

### `e3e95db` — a failed `LUR_ASSERT` must log where someone can read it

`LUR_ASSERT` printed to **stderr**, then trapped. On a phone stderr is nowhere: nothing drains a
sideloaded app's stdio, and `UnblockStdio` deliberately makes those writes *drop*. So "asserts are
DEAFENING in Development" — a stated principle of the config ladder — held on the desktop and was
quietly false on device, where a failed assert trapped in silence leaving only a backtrace.

It had already misled real work: the 2026-07-20 Android perf/stability plan listed
`adb logcat -d | grep -E "ANR|LUR_ASSERT|SIG"` as its evidence step for classifying crashes as
assert-traps. **That grep could never have matched.**

Now routed through `Lur::Log::Error` (whose own no-sink fallback is still stderr, so the host is
unchanged), one line rather than three because os_log and logcat are line-oriented. Tested by calling
`Report` directly — the macro would take the process down, and the ROUTING is the half that was
broken.

### `75afe76` — the initial link was reporting itself as a reconnect

The most consequential fix of the day, and it was found sideways. Converting the desktop workbench
needed a seam (`RecordSync::OnRecordDatagram`, below); its test then showed the observer firing
**twice** for one link-up, from **two** adopt calls.

`Session::Tick` drains the inbox FIRST (#40 — receivers fire on the engine thread), so the handshake
can complete *inside* that `PumpInbox`. The reconnect-edge test below it then reads
`Ready && Connected && !PrevConnected` on the very first tick that ever observed the transport as
connected, and fires. **The `Ready &&` in that condition was there precisely to mean
"post-handshake" — it cannot do that job, because `Ready` becomes true upstream of it, in the same
tick.**

Cost, once a game was listening:

* chess re-sent its **whole per-opponent record on every fresh link** — harmless (`MergeIfNewer` is
  monotonic) and therefore invisible, but a doubled payload on a link whose slimness IS the product;
* every fresh link logged `reconnected — requesting resync`, so the log **asserted a drop that never
  happened**;
* RPS's `OnResync` means "rebase the lockstep timeline" — a **spurious rebase at link-up**, on the
  path that is already #204's prime suspect (recorded there with caveats: unproven as its cause, and
  it does not explain the idle pre-match hash divergence).

Fix: latch `PrevConnected` as already-seen at the moment we go `Ready`.

**And it was load-bearing for a test.** `TestHostWithoutRecordSync` asserted "the resync route reaches
the game's hook" via `RequestResync()` — which had been returning early the whole time, refused by the
#71 resync gate (going Ready arms it; a host with no record sync sends no Sync, so only the ~3 s
fallback lifts it). The assertion passed on the count the **bug** left behind. A test that passes for
the wrong reason is worse than a missing one: it reports coverage.

### `864a3c2` — the desktop workbench adopts GameHost

Its own header called it "deliberately a THIRD copy-pasted chess main (Phase-4 extraction evidence)".
That copy is gone, and it had already drifted: its own `SendRecord`, its own hijack-guard spelling,
its own MATCH END line.

It also earns its keep as a **test of the extraction**, in three ways the phone mains cannot:

1. **two hosts in one process** — nothing in `GameHost` may be static, and now something exercises
   that;
2. **it compiles locally** — every other consumer is a phone main gated on CI, so this is the first
   fast-loop compiler over the seam;
3. **it runs the whole flow headlessly** — `--frames 3000 --auto` plays a dozen matches: each host
   adopts once, both print the SAME MATCH END line with the same tally, and totals accumulate across
   runs, which is persistence proving itself rather than being asserted.

New seam it forced: **`RecordSync::OnRecordDatagram`**. The host owns `EMsgType::Sync` outright, so a
game that also wants to *see* those bytes had nowhere to stand — and the workbench's flight recorder
had its **only** `DatagramIn` call site inside the handler the host replaces. Adopting the host would
have quietly shrunk every recording while all the logs still read fine. It fires for **rejected**
records too: "record everything" means the refused ones especially, since that is the file you want
when a pairing goes wrong.

---

## 3. Device verification (2026-08-12, ~20:10)

Chess installed on both phones from `864a3c2`, non-agent, fingerprints matching. Android↔iPhone link
formed in ~2.1 s.

**Verified:**

* **The extracted #73 UIKit rebuild ran on the iPhone and healed a real never-composited launch.**
  ```
  OnlyChess: #73 reattach: view unhosted - rebuilding window+view+layer on scene state=0
  OnlyChess: #73 reattach: re-init ok (drawable 1125x2436, appActive=1)
  ```
  The first line is the new shared function — its wording differs from the copy it replaced
  ("…+layer" vs "…+layer+renderer"), which is what makes it evidence rather than a coincidence. The
  app then rendered, linked and played.
* **`75afe76` on both phones.** Per side: `READY` once, `peer linked … -> adopt (go live)` **once**,
  one `recv msg type=7` (the record), and **zero** `reconnected — requesting resync`. Before the fix:
  two adopts, two records, one false "reconnected". (`resync received — moves enabled` does appear and
  is a *different* line — the #71 gate clearing when the peer's record lands.)
* **#203's radio-state logging**, as designed:
  `BLE up: serving=1 advertising=0 scanning=0 (service published)` then
  `scanning=1 (scan live (result delivered))`.

**NOT verified — and read this before claiming it:** the log sink's payoff. I first took
`Vulkan renderer up: 1080x2408` as proof; **it is not** — the Vulkan backend has its own
`Vk::PlatformLog` seam straight to logcat/os_log (`Modules/Render/Platform/*/VulkanSurface.*`), so
that line was always visible.

Chasing it properly found something worth knowing: **chess exercises no routine engine `Lur::Log`
path at all.** The engine's only call sites are `Lur/Core/CVarConfig.h` (chess never loads a
cvars.cfg) and `Lur/Core/Assert.h`. So for chess the sink is **latent** — its concrete payoff today is
that a failed assert reaches the platform log, which is exactly why `519736f` and `e3e95db` only pay
off together.

The probe for that (call `Lur::Assert::Detail::Report` directly on device — same routing, **no trap**,
so no crashing build is left behind) was built and then blocked: the Galaxy dropped off wireless ADB
mid-install (its debugging port rotates; neither the old port nor mDNS found it). The probe is
reverted and never reached the device. **This is the one open verification item; see #43.**

---

## 4. Method lessons from today

1. **A success-shaped signal is not evidence.** Today's tally: a MATCH END tally printed while nothing
   persisted; a test green on a count the bug produced; a doubled record send invisible because the
   merge is monotonic; an assert "deafening" into a discarded pipe; a renderer log line that proved
   something it had no connection to. Every one of them *looked* right.
2. **Scripted structural edits need a read-back.** Two failed the same way today: a rename that missed
   `S.Session`, and a deletion that cut at a `for` loop's closing brace instead of the function's,
   leaving a stray `}` in both iOS mains. For files only CI compiles, **check brace balance against
   the last known-good commit before pushing** — that check passed clean on the third attempt and on
   `cee2018`, which went green first try.
3. **`AppPlatform.mm` needed `<initializer_list>` explicitly** where the mains it moved out of got it
   transitively. A green `build.ps1` says nothing about the `.mm` files.
4. **Order the work by what it needs, not by what it resembles.** The absorptions that landed were the
   **handle-free** ones. Everything left in section B needs the platform handle (`android_app*`, the
   `UIView`) that the entry point owns — so they want the entry point *first*, rather than each
   growing a `void*` parameter. That was not obvious up front and is the single most useful planning
   note in this report.
5. **Don't check in build artifacts.** `519736f` committed a 1.8 MB AGENT `.ipa` at
   `Games/Chess/Android/dist/` — `gh run download -D dist` had inherited a shell cwd left behind by a
   `cd …/Android && ./gradlew` a few commands earlier, and the root-anchored `/dist/` ignore did not
   cover it. Untracked and deleted here; `.gitignore` now ignores `dist/` unanchored plus
   `*.ipa/*.apk/*.aab`. The blob remains in history (not worth a rewrite on a pushed solo trunk).

---

## 5. Decisions worth not re-litigating

* **The RPS desktop workbench does NOT adopt GameHost.** It holds no `Store`, no device id and no
  `SyncManager` — just `Session` + the #160 routing table + a resync hook, about three lines. Its two
  peers use **hardcoded literal GUIDs** (`rps-peer-a` / `rps-peer-b`) so team assignment is
  deterministic; `GameHost` always derives the id via `LoadOrCreateDeviceId`. Converting it would mean
  adding a device-id override to the engine to serve one dev tool, in exchange for removing three
  lines. Wrong trade — the gap is a decision, not an oversight.
* **`GameHost`'s record half stays opt-in.** RPS found that (08-11): its `ScoreBook` is not an
  `ISaveState`, is never sent over the wire, and its resync means "rebase", not "re-adopt". If the
  second game has to fight the seam, fix the seam.
* **Init → EnableRecordSync → view wiring → Start is load-bearing ordering**, not style. `64f70b5` is
  what happens when it slips.

---

## 6. What to do next, in order

1. **Close the one open verification** (§3): reconnect the Galaxy (wireless debugging port rotates —
   needs the user, or USB), install chess, and confirm a `Lur::Assert::Detail::Report` line reaches
   logcat. ~2 minutes once the device is reachable.
2. **Section B's entry points.** Note what already exists: `Lur::Platform::Window` covers the Win32
   ceremony, so "Windows entry point" is largely done. What is not: `android_main` / ALooper /
   `APP_CMD_*`, and `UIApplicationMain` / `CADisplayLink`. **Land it converting one main at a time** —
   a shared entry point with no consumer is dead engine code, which this repo's own rule forbids
   (promote on the second consumer).
3. **The remaining absorptions, after the entry point** (they need its handle): pause/resume +
   persist-on-background, swapchain resize, safe-area insets, save-dir discovery. Note that
   `GameHost::OnBackground()` is a no-op without record sync, so RPS's missing `APP_CMD_PAUSE` costs
   nothing *today* — don't "fix" it into a behaviour change without deciding what RPS should do when
   backgrounded mid-match.
4. **Sections C–F** (unchanged from the 08-11 report): thread topology as config; input pipeline +
   multi-touch + recognizers; the agent channel (4 copies); `std::filesystem` → `fopen` +
   `-fno-exceptions`.
5. **#204** is unblocked but not solved: make `recdiff` conclusive first (a `.rec` format change),
   because a rebase and a genuine divergence currently look identical in the evidence we collect. The
   spurious-rebase fix (§2, `75afe76`) removed one candidate cause without proving it was *the* cause.

## 7. Awaiting the user

* **#197 (Phase 2 gate).** The two-phase soak ran and I flagged the gate green on 08-11; the user has
  not ruled on it. Do not close it unilaterally.
* Agent builds for RPS remain installed on both phones **on purpose** — CLAUDE.md now says the
  handover close-out happens only on an explicit request, because tearing the rig down per issue was
  slowing real work.
