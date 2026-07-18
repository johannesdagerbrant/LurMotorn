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

### One-time / periodic setup (each an unavoidable Apple gate)

1. **Weekly profile renewal**: free Apple accounts get **7-day provisioning profiles**; one
   Sideloadly run (Apple ID + 2FA) renews it, and the rig auto-adopts the fresh profile on
   its next install. This is the ONLY recurring human step.
2. **First-launch Bluetooth allow** — one tap the first time a *fresh install* runs (Apple
   TCC). Same-bundle-id reinstalls keep the grant, so this is per-new-identity, not
   per-build.

After that, `device-rig.bat -Action cycle` fetches, signs, installs, launches, arms, plays,
measures, and repeats with no human touch.

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
