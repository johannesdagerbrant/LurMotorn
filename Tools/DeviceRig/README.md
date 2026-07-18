# DeviceRig — game-agnostic on-device run + debug (Android + iOS)

An **engine** dev instrument (hand-run, never shipped, not tied to any game): it drives a
LurMotorn app on real phones — sign+install, arm the dev autoplayer, launch, tail the
engine log, screenshot, and summarize the **same-frame reply** metric — over `adb`
(Android) and `pymobiledevice3` / Sideloadly (iOS). It speaks only engine terms (peer,
link, autoplay, datagram, same-frame, match) and parses only engine log lines
(`AUTOPLAY …`, `MATCH END …`). A different game reuses it verbatim by editing the `$App`
block at the top of `device-rig.ps1`.

The autoplayer + same-frame instrumentation are `#if LUR_INTERNAL` in the app (a
Development build). This rig only toggles and observes them.

## One command

```
Tools\DeviceRig\device-rig.bat -Action cycle -Iterations 3
```

`cycle` is the **fully autonomous loop** (#70): `(fetch →) install-if-changed → [run →
analyze] ×N`, with **zero human interaction per iteration** after the one-time setup
below. `run` is one measured pass on its own — arms both peers, (re)launches, captures
both engine logs, and reports the same-frame tally after N matches. Other actions:
`install`, `arm`, `disarm`, `reset`, `launch`, `tail -Peer <android|ios>`, `shot`,
`status`.

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
| Install a NEW build | `adb install -r` | `-SignedIpa`/`-ZsignP12` → `apps install` (headless), else Sideloadly GUI (assisted) | **Yes if signed; assisted otherwise.** See below. |

**Launch/screenshot no longer need admin (superseded).** On `pymobiledevice3` 9.33.4,
`developer dvt … --userspace` stands up the iOS 17+ tunnel *in-process* with a userspace
network stack, so it needs **no root/admin** — verified launching + screenshotting the app
from this non-admin shell. The old `sudo remote tunneld` one-time-per-boot step is gone.

**Install of a new binary — three routes, most-headless first:**
1. **`-SignedIpa <path>`** → `pymobiledevice3 apps install` — fully headless, no GUI.
2. **`-ZsignP12 <p12> -ZsignProfile <mobileprovision>`** → the rig `zsign`s the unsigned CI
   `.ipa` with a **persisted free dev cert**, then `apps install` — fully headless for the
   7-day life of that cert. `zsign` is a dev-only Tool (MIT), never linked into the app.
3. **Neither** → opens Sideloadly at the `.ipa` (**assisted** — the one human touch; first
   run also does the Apple-ID login + 2FA). We don't drive Sideloadly's private `/enqueue`
   API — it's credential-sensitive and fragile.

**You rarely re-install at all:** `cycle` hashes the `.ipa` and **skips install when it's
unchanged** — the Sideloadly daemon keeps an unchanged build's signature fresh until the
7-day cert expiry. So re-running experiments back-to-back is entirely hands-off; only a
genuinely new binary hits the signing gate.

### One-time setup (minimal, each an unavoidable Apple gate)

1. **Sideloadly login once** (or drop in a persisted free dev cert for zsign) — needed
   because Apple requires a signing identity + 2FA. Renew the free cert **weekly** (Apple's
   7-day free-provisioning limit).
2. **First-launch Bluetooth allow** — one tap the first time a *fresh install* runs (Apple
   TCC). Unchanged installs keep the grant, so this is per-new-binary, not per-iteration.
3. ~~Admin tunnel per boot~~ — **no longer required** (userspace tunnel).

After that, `device-rig.bat -Action cycle` launches, arms, plays, measures, and repeats
with no human touch — re-signing only when the code actually changes.

## App config (`$App` in device-rig.ps1)

```
LogTag         OnlyChess                              engine log tag the app emits
AndroidPackage com.lurmotorn.onlychess
IosBundleId    com.lurmotorn.onlychess.L5XBWVZ7N3     sideload appends the signer suffix
AutoplayProp   debug.lur.autoplay                     Android engine autoplay toggle (setprop)
AutoplayMarker autoplay                               iOS: Documents/<marker> engine autoplay toggle
```

Apple ID is **not** stored here — pass `-AppleId` (used once; Sideloadly then caches a token).

## Relationship to BleDevRig

`Tools/BleDevRig/` is the Windows↔Android BLE rig (a WinRT radio + `WindowsBleTransport`,
for developing the radio under a debugger). `DeviceRig` is the phone↔phone (Android↔iOS)
run/debug rig — no PC in the link. Both are engine instruments under `Tools/`.
