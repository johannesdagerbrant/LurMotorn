# RocksPapersScissors (RPS)

Durable orientation for the RTS that is LurMotorn's Phase-2 proving ground — namespace `Rps`,
Android log tag **`OnlyRps`**, package / iOS bundle `com.lurmotorn.onlyrps` (sideload appends
`.L5XBWVZ7N3`). It runs in **bit-perfect lockstep across a real Android↔iPhone BLE link**: two
independent sims (NDK Clang native-Vulkan ↔ Apple Clang MoltenVK) agree at every anchor.

This file is **orientation only — it holds no live status.** It versions with the code, so it can't
drift from HEAD the way a snapshot issue does. Do **not** paste any issue's open/closed status here;
that is what a query is for (see [What's open](#whats-open)). For rules shared across the whole
engine — the `LUR_AGENT`/`LUR_INTERNAL` gates, the agent command grammar, log-reading traps,
build configs — read **`CLAUDE.md`**; this file does not restate them.

## Where things live

```
Games/RocksPapersScissors/
  Core/      pure C++ sim + tunables   (Core/Public/Rps/Tunables.h is the knob list)
  Net/       LockstepPeer, EventCodec, resync, cvar sync, build fingerprint
  Runtime/   SimRunner, Snapshot
  View/      GameView, CameraScroll, HUD, console UI
  Desktop/   DesktopMain.cpp  (onlyrps_desktop — two-window loopback; the workbench)
  Android/   RpsMain.cpp + Kotlin shim
  iOS/       RpsMain.mm
  Content/   Icons/  (cooked to View/Private/IconMasks.h by a LUR_COOK marker)
```

## Build & run

```
powershell -File build.ps1                                   # shared core + host test suites (do this for any core change)
powershell -File scripts\rps-desktop-build.ps1 -Run          # the desktop workbench (two-window loopback)
powershell -File scripts\rps-desktop-build.ps1 -Run -Solo -Ai hard   # one window vs the AI
```

- **Android:** `cd Games/RocksPapersScissors/Android && ./gradlew.bat assembleDebug`
  (add `-PlurAgent=ON` only for a harness build — see CLAUDE.md).
- **iOS:** **macOS CI only** — the `RPS iOS unsigned .ipa` job produces `OnlyRps-unsigned.ipa`;
  there is no local Mac.

`onlyrps_desktop` flags (parsed in `Desktop/DesktopMain.cpp`):

| flag | effect |
|---|---|
| `--solo` / `--auto` / `--autofoe` | one window vs AI / legacy soak / AI drives the far team |
| `--ble` | drive the desktop over a real radio instead of loopback |
| `--stress` / `--nocombat` / `--flockdemo` | perf/behaviour harnesses |
| `--seed <n>` / `--maxticks <n>` / `--matches <n>` / `--frames <n>` | run bounds |
| `--replay <file>` | replay one recording |
| `--recdiff <a> <b>` | diff two peers' recordings (the divergence instrument) |
| `--aivs` / `--aidiag` / `--aibeginner` / `--aiowner` / `--ai <tier>` | AI matchups & tiers |
| `--winw` / `--winh` / `--every` | window + cadence |

## Device ops

```
gh run download <run-id> -n OnlyRps-unsigned-ipa -D dist
powershell -File Tools\DeviceRig\device-rig.ps1 -Game rps -Action install -Peer ios   # headless zsign+install
powershell -File Tools\DeviceRig\device-rig.ps1 -Game rps -Action launch  -Peer ios
powershell -File Tools\DeviceRig\device-rig.ps1 -Game rps -Action pullrec             # BOTH peers' .rec + auto-diff
powershell -File Tools\DeviceRig\device-rig.ps1 -Game rps -Action shot    -Peer ios
```

Log-reading recipes and their traps (never dump unfiltered logcat; never pipe iOS syslog through
`grep > file`) are in **CLAUDE.md**.

## Two-phone pre-flight (each item has cost a wasted run)

1. `badbuild=0` on **both** — a fingerprint mismatch refuses the match; on screen the field now
   shows a throbbing "DIFFERENT BUILD" banner rather than a silent freeze.
2. Matching pre-match `gold=` / `hash=` on both.
3. **`rps.dev.flight_recorder` ON on both** — the Galaxy's `cvars.cfg` fixture persists it `false`,
   which once cost a 22-minute soak its entire recording.
4. **Read `gaps=` on BOTH peers** — a one-directional fault is invisible from the good end.
5. **For a PERF trace, both phones must be built at the SAME `LUR_CONFIG` + optimization.** The
   `LUR_*` macros already match (both default `Development`), but the *optimization* does **not** by
   default: Android's NDK build is Ninja (single-config) so `EngineFlags` couples `-O2` to
   `LUR_CONFIG`, while the iOS build is the Xcode generator (multi-config) where that coupling is
   **ignored** and `xcodebuild -configuration Debug` hard-forces **`-O0`**. On 2026-08-03 this made
   the iPhone's `sim.step` read ~2.5–3 ms vs the Galaxy's ~0.15 ms — a 20× gap that was **purely the
   `-O0` iOS build**, not the sim (the cache-friendly AI sim, #181, is shared `Core/Private/Sim.cpp`
   with no platform `#ifdef` — both phones run it). To compare, build the `.ipa` with
   `xcodebuild -configuration RelWithDebInfo` (`-O2 -g -DNDEBUG`, matches Android `Development`), or
   build **both** at `-DLUR_CONFIG=Shipping` for true ship-perf. See CLAUDE.md's "keep the Xcode
   scheme in sync with `LUR_CONFIG` by hand" caveat / the unfinished iOS tail of #89. (If
   `macos-ci.yml` has since been switched off `-configuration Debug`, this trap is closed — verify.)

## RPS-specific HARD RULES

- **Sim constants and tick order must be IDENTICAL on both phones.** Rebuild+reinstall both
  together; the build-fingerprint gate enforces it by refusing the match before tick 0.
- **Camera/view state is per-device** and may differ freely — it is not sim state.
- **`BleServiceUuid` is per-game** (`LUR_BLE_SERVICE_UUID`, RPS `…5472616E7371`), so RPS never
  cross-links a chess phone. Keep the C++ macro and the Kotlin `SERVICE_UUID` equal.
- **No floats in sim state** — determinism is load-bearing (`Modules/Math` floats are render-only).
- The Galaxy deliberately keeps `files/rps-cvars.cfg` (`starting_gold=750`,
  `miner.building_cost=400`) as a **live fixture** exercising the cvar merge. Don't delete it
  thinking it's a confounder; restore it if you change it.
- `WorldHeight` is **240**: team 0's opening camp is `(17,16)`, team 1's mirror `(17,224)`; an
  out-of-frontier placement is rejected **silently**.

## Driving both phones with no hands

The `LUR_AGENT` harness (`Rps/AgentControl.h`) — grammar, channels, the `linked`-first rule, and the
handover ritual — is documented in **CLAUDE.md** (the "harness that lives behind it (RPS)" section).
It is not duplicated here.

## What's open

There is no status list here, on purpose. Get the live set:

```
gh issue list --state open --label rps           # current RPS work
gh issue list --state open --label rps --label P1   # + a priority filter
```

Sequencing / priority / phase is the roadmap tracker **#12**. Engine-extraction work that RPS
motivated but that is cross-cutting (platform layer, `GameHost`, de-chess `Modules/Net`, `IGame`)
lives under epic **#39**, not the `rps` label.
