# TraceAndroid — real perf numbers from the phone, playing the PC (issue #101)

A game-agnostic capture harness. It runs the **installed** app on the Android device
while the device plays a **live match against the PC** (the PC is a real BLE opponent),
and pulls the numbers that matter off the phone over the capture window. The discipline:
**run it before AND after every perf task** so every change has a real before/after —
otherwise "it's faster now" is a guess (see #89, shipped with no measured win).

## One command

```
Tools\TraceAndroid\trace-android.bat -Label after-perf2 -DurationSec 60
```

That: launches the phone app, launches `onlyrps_desktop --ble --auto` as the opponent,
waits for the BLE link, then over the window captures and writes `runs\<ts>\report.md`
(+ appends `runs\history.csv`, tagged with the git SHA).

## What it captures

- **App scopes** — the `TRACE …` line the app emits on the LOCKSTEP cadence
  (`sim.step`, `sim.acq/move/atk`, `render.capture/view`, and the `input.toApply` /
  `ble.toApply` latencies once #101-D lands), as `name=avgMs/maxMs`. Needs a
  Development build (`LUR_TRACE=1`).
- **Frame pacing** — `dumpsys gfxinfo`: rendered fps, jank %, frame-time
  p50/p90/p95/p99, missed-vsync.
- **Thermal** — `/sys/class/thermal/thermal_zone*/temp` peak, start→end.
- **Memory** — `dumpsys meminfo` TOTAL PSS.
- **Liveness** — final lockstep tick + desync flag from the `LOCKSTEP` line.

## Prerequisites

- The phone on wireless ADB (see CLAUDE.md), app installed optimized
  (`gradlew installDebug`, which is now optimized-Development, #89).
- The PC peer + radio built: `scripts\rps-desktop-build.ps1` and
  `Tools\BleDevRig\build.ps1 -Source BleRadio.cs`.

## Flags

| flag | meaning |
|---|---|
| `-App rps\|chess` | which game (default rps) |
| `-DurationSec N` | capture window (default 60) |
| `-Label <str>` | tag the run/CSV row (e.g. `before-perf4`) |
| `-NoPeer` | don't launch the PC opponent (a match is already running, or capture-only) |
| `-Fresh` | `pm clear` + re-grant BLE perms before launch (fresh advertising identity) |
| `-Serial <s>` | pin the adb device (default: the single ip:port device) |
| `-RadioExe` / `-PeerExe` | override the radio / PC-peer paths |

## Relationship to the other rigs

`BleDevRig` is the raw Windows↔Android BLE link (radio + `WindowsBleTransport`);
`DeviceRig` is phone↔phone run/debug. **TraceAndroid** reuses BleDevRig's radio to make
the PC an opponent and adds the perf capture + report on top — it's the measurement
instrument the perf epic (#86) runs between tasks.
