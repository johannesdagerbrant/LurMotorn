# DeviceRig — game-agnostic on-device run + debug (Android + iOS)

An **engine** dev instrument (hand-run, never shipped, not tied to any game): it drives a
LurMotorn app on real phones — sign+install, arm the dev autoplayer, launch, tail the
engine log, screenshot, and summarize the **same-frame reply** metric — over `adb`
(Android) and `pymobiledevice3` / Sideloadly (iOS). It speaks only engine terms (peer,
link, autoplay, datagram, same-frame, match) and parses only engine log lines
(`AUTOPLAY …`, `MATCH END …`). A different game reuses it verbatim by editing the `$App`
block at the top of `device-rig.ps1`.

**The autoplayer and the BLE role override are `#if LUR_AGENT`** (#195, #196) — they are remote
control, so they are *absent* from an ordinary build, not merely idle. An APK or `.ipa` built
without the flag has no autoplayer, and this rig will wait forever at `matches ended=0`. Build
`./gradlew assembleDebug -PlurAgent=ON` for Android and `gh workflow run "macOS CI" -f agent=true`
for iOS. The same-frame instrumentation is `#if LUR_INTERNAL` and is present in any Development
build. This rig only toggles and observes them.

## One command

```
Tools\DeviceRig\device-rig.bat -Action cycle -Iterations 3
```

`cycle` is the **fully autonomous loop** (#70): `(fetch →) install-if-changed → [run →
analyze] ×N`, with **zero human interaction per iteration** after the one-time setup
below. `run` is one measured pass on its own — arms both peers, (re)launches, captures
both engine logs, and reports the same-frame tally after N matches. Other actions:
`install`, `arm`, `disarm`, `reset`, `launch`, `tail -Peer <android|ios>`, `shot`,
`status`, `pullrec`.

## `pullrec` — collect BOTH peers' flight recordings, and diff the pairs

```
Tools\DeviceRig\device-rig.bat -Action pullrec -Game rps
```

Pulls every `.rec` off both phones into `dist\rec\{android,ios}\`, pairs the linked
(`rps-vs-*`) captures **by match rather than by filename** — each peer stamps its own
clock and its own per-session ordinal, so one match is `…-073714-1.rec` here and
`…-073715-2.rec` there — and runs `--recdiff` on each pair. Requires the desktop binary
(`scripts\desktop-build.ps1 -Game rps`); without it the pull still happens and the diff is
skipped with a note.

The pairing key is seed + build fingerprint + the footer's `end` line. Captures with **no
`end` line** (the app was killed mid-match — which includes the longest ones) are reported
with their seed rather than paired: every session walks the same seed ladder from
`kMatchSeed`, so seed alone would pair whatever collides. Anything one-sided is named
explicitly — a match only one peer recorded is a finding, not noise.

**Why this is a command and not a `pymobiledevice3` one-liner.** iOS recordings live in
`Library/Application Support`, and the interactive `apps afc` shell word-splits the space
into two paths, reporting `cannot access` for both (on Windows it also dies on an emoji in
its own banner). Hand-listing that directory produced a partial result that read as an
empty one, and hence a wrong bug report (#171) claiming the iPhone recorded nothing — it
had recorded twelve matches, and #159 stayed blocked on a diff that could have been run
the whole time. `apps pull` takes the directory whole and handles the space itself.

## The iOS loop is now autonomous (no admin) — one Apple gate remains

Android is fully headless (adb). iOS is now headless too — the old admin-tunnel gate is
gone. The single genuine exception is signing a **new** binary, which is an Apple security
gate (code signing) that can't be automated away without handling Apple-ID credentials —
something this project deliberately does not do.

| Step | Android | iOS | Headless? |
|---|---|---|---|
| Launch the app | `adb monkey` | `-Action launch` → `developer dvt launch --userspace` | **Yes — no admin.** The iOS 17+ **userspace** tunnel is a pure-Python net stack; no `sudo`/`tunneld`. |
| Arm autoplay | `setprop debug.lur.autoplay 1` | push `Documents/autoplay` marker (`apps push … Documents/autoplay`, container-vend — **not** `--documents`, which `InstallationLookupFailed`s on iOS 26) | **Yes** |
| Tail engine log | `logcat -s OnlyChess:*` | `syslog live \| grep OnlyChess` (the `-m` message match does **not** filter on iOS 26) | **Yes** |
| Screenshot | `screencap` | `developer dvt screenshot --userspace` | **Yes — no admin** (same userspace tunnel as launch). |
| Install a NEW build | `adb install -r` | auto-zsign → `apps install` | **Yes — fully headless** (see below). |

**Launch/screenshot no longer need admin (superseded).** On `pymobiledevice3` 9.33.4,
`developer dvt … --userspace` stands up the iOS 17+ tunnel *in-process* with a userspace
network stack, so it needs **no root/admin** — verified launching + screenshotting the app
from this non-admin shell. The old `sudo remote tunneld` one-time-per-boot step is gone.

**Install of a new binary is fully headless (auto-zsign, the default).** `-Action install`
locates everything itself: `zsign` (PATH or `%LOCALAPPDATA%\LurMotorn\tools`), the free dev
**cert/key PEMs Sideloadly persists** in `%APPDATA%\Sideloadly` (the cert lasts ~1 year),
and the **newest matching provisioning profile re-dumped from the device** each install
(`provision dump`), so Sideloadly's weekly profile renewal is picked up automatically. It
zsigns the CI `.ipa` (rewriting the bundle id to the signed identity) and `apps install`s
it — verified end to end on hardware, zero interaction. Overrides: `-SignedIpa <path>`
(pre-signed) or explicit `-ZsignP12`/`-ZsignProfile`; if signing material is missing the
rig falls back to opening Sideloadly (drag the `.ipa` in — its `-i` flag is unreliable).
`zsign` is a dev-only Tool (MIT), never linked into the app.

`cycle` additionally hashes the `.ipa` and **skips install when unchanged**, so re-running
experiments back-to-back never reinstalls at all.

#### A running app blocks the install — and used to do it invisibly

**`apps install` blocks for as long as the app is RUNNING on the iPhone, with no progress, no
error and no timeout.** The shape a human sees is "the install just sits there until I swipe the
app away, then it finishes instantly"; the shape an agent sees is nothing at all, for as long as
it is willing to wait. Android has no equivalent (`adb install -r` kills and replaces a running
app), so a two-peer install stalls on the iOS half *only* — which is exactly where nobody is
watching. The rig made it worse: it never read the install's exit code, so a failed or killed
install still printed `ios: installed` and reported the headless path had succeeded.

The rig now **detects it, bounds it, and decides on evidence**:

- Before installing it asks whether the app is running (`dvt proclist`). If it is, it **terminates
  it itself** — `dvt signal <pid> 9`. No human, ever: an install step that waits for someone to
  swipe the app away makes the rig undrivable for the unattended two-phone runs it exists for.
  If the terminate fails, the install is refused with `-ForceUninstall` named as the fallback,
  rather than blocking forever.

  **`dvt signal` is the only thing that works.** `dvt kill` and `dvt pkill` ride a DTX request iOS
   26 accepts and ignores — `pkill` even logs `Killing OnlyRps(5099)` while the pid *and start
  time* are unchanged. That is why #168 originally concluded no headless terminate existed.
  `signal` sends a real POSIX signal through a different request; SIGKILL takes the process down
  (verified 2026-08-01, pid 5255 → absent from `proclist`). Install over a running app then takes
  **~32 s, hands-off**, where before it blocked indefinitely until someone swiped.
- The install itself is bounded by `-InstallTimeoutSec` (default 240) and its exit code is read.
- **The CLI's exit is not the source of truth.** Measured 2026-08-01: an install *landed* (the new
  build verifiably ran afterwards) while the CLI sat unreturned for over ten minutes. So on a
  non-clean exit the rig compares the app's installation record before and after — iOS mints a new
  `Bundle/Application` UUID per install — and reports "did not exit cleanly but the bundle
  CHANGED: the new build IS on the phone" rather than a false failure. When the record is
  unavailable it says the result is *inconclusive* instead of guessing.

**There is no headless terminate on iOS.** Measured on iOS 26.5: `dvt kill <pid>` exits 0 and
`dvt pkill OnlyRps` even logs `Killing OnlyRps(5099)`, and the process survives both with an
unchanged pid *and* start time. Only a kill-existing `dvt launch` restarts it, which relaunches
rather than terminates and so is useless before an install. If you want genuinely zero-touch,
`-ForceUninstall` uninstalls first — but it is **destructive**: it wipes the container, taking the
device GUID (stable BLE role + colour), opponent history, and any `.rec` flight recordings you
have not pulled. Off by default for that reason.

`test-install-probes.ps1` covers the parsing and the two safety-critical distinctions — a failed
probe must never read as "app closed", and a bundle id that merely *contains* ours is not a match.
It lifts the real functions out of `device-rig.ps1` via the AST, so it tests the shipped code and
needs no device: `powershell -ExecutionPolicy Bypass -File Tools\DeviceRig\test-install-probes.ps1`.

### One-time / periodic setup (each an unavoidable Apple gate)

1. **Weekly profile renewal**: free Apple accounts get **7-day provisioning profiles**; one
   Sideloadly run (Apple ID + 2FA) renews it, and the rig auto-adopts the fresh profile on
   its next install. This is the ONLY recurring human step.
2. **First-launch Bluetooth allow** — one tap the first time a *fresh install* runs (Apple
   TCC). Same-bundle-id reinstalls keep the grant, so this is per-new-identity, not
   per-build.

After that, `device-rig.bat -Action cycle` fetches, signs, installs, launches, arms, plays,
measures, and repeats with no human touch.

## The no-hang invariant (read this before adding a call)

**No code path in this script may wait on an external service without a bound.** Every call out
to a device — `adb`, `pymobiledevice3`, `zsign` — goes through `Invoke-Bounded`.

This is a rule, not a nicety, because a stall that never returns is *indistinguishable from a
crash*. The caller cannot tell "busy" from "broken", so the only way out has been a human
noticing and swiping the app away — which has been the single biggest time sink in iOS work on
this project. We fixed it for `apps install` (#168) after it cost a session, and months later
found `dvt screenshot` doing exactly the same thing (#179). Fixing instances does not fix the
class.

Three things make a timeout *recoverable* rather than contagious:

1. **Kill the tree, not the process.** `Invoke-Bounded` uses a real pid and `taskkill /T`. The
   direct child is python or adb; orphaning it leaves the device's service socket held open, and
   then the *next* call hangs too — one stall becomes a dead rig until something gets rebooted.
2. **Reap orphans from the job-based helpers.** `Invoke-BoundedPmd` / `Invoke-BoundedInstall` use
   `Start-Job`, and `Stop-Job` kills the child PowerShell but not its python grandchild.
   `Kill-StrayPmd` sweeps those, scoped by start time so a syslog tail you started earlier is
   never touched.
3. **Never let a timeout look like success.** Some actions deliberately continue past a bounded
   failure (a screenshot is not worth aborting a test run for), so the script prints a red
   summary of everything it killed and **exits 2**. Without that, a run ends showing only its
   successes and the operator concludes the device is healthy.

Current bounds: adb 30s, pymobiledevice3 60s, `developer dvt` 45s, `dvt screenshot` 20s, zsign
90s, `apps install` 240s, `apps list` 150s. They are generous enough that a healthy call never
trips one — pick the same way if you add another: several times the slowest healthy measurement.

## App config (`$App` in device-rig.ps1)

```
LogTag         OnlyChess                              engine log tag the app emits
AndroidPackage com.lurmotorn.onlychess
IosBundleId    com.lurmotorn.onlychess.L5XBWVZ7N3     sideload appends the signer suffix
AutoplayProp   debug.lur.autoplay                     Android engine autoplay toggle (setprop)
AutoplayMarker autoplay                               iOS: Documents/<marker> engine autoplay toggle
```

No Apple-ID credential is stored or handled anywhere in the rig — signing uses the local
cert/key PEMs + device-dumped profile; the Apple ID is only ever typed into Sideloadly
itself during the weekly profile renewal.

## Relationship to BleDevRig

`Tools/BleDevRig/` is the Windows↔Android BLE rig (a WinRT radio + `WindowsBleTransport`,
for developing the radio under a debugger). `DeviceRig` is the phone↔phone (Android↔iOS)
run/debug rig — no PC in the link. Both are engine instruments under `Tools/`.
