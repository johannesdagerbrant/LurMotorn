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
Tools\DeviceRig\device-rig.bat -Action run -Matches 3
```

Arms both peers, (re)launches, captures both engine logs, and reports the same-frame
tally after N matches. Other actions: `install`, `arm`, `disarm`, `reset`, `launch`,
`tail -Peer <android|ios>`, `shot`, `status`.

## Bringing the iOS loop closer to Android

Android is fully headless (adb). iOS is *mostly* headless after some one-time setup —
with an honest exception for install:

| Step | Android | iOS | Headless? |
|---|---|---|---|
| Launch the app | `adb monkey` | `-Action launch` → `pymobiledevice3 developer dvt launch` | **Yes**, once a RemoteXPC **tunnel** is up: `sudo python -m pymobiledevice3 remote tunneld` (needs admin; **one-time per boot**, persists) |
| Arm autoplay | `setprop debug.lur.autoplay 1` | push `Documents/autoplay` marker (`apps push … Documents/autoplay`, container-vend — **not** `--documents`, which `InstallationLookupFailed`s on iOS 26) | **Yes** |
| Tail engine log | `logcat -s OnlyChess:*` | `syslog live \| grep OnlyChess` (the `-m` message match does **not** filter on iOS 26) | **Yes** |
| Screenshot | `screencap` | `developer dvt screenshot` | **Yes**, via the same tunnel as launch |
| Install a NEW build | `adb install -r` | `-Action install` opens Sideloadly at the `.ipa` | **No — assisted (GUI).** See below. |

**Install is honestly not headless, and here's why (learned this session):** Sideloadly's
CLI just raises the already-running GUI instance — its `--silent`/`--enqueue` flags are
ignored whenever an instance holds `localhost:28811` (always). The only headless path is
POSTing a plist (`appleId` + a cached `passwordToken`) to its **private** `/enqueue` API —
fragile and credential-sensitive, so we don't automate it. **But you rarely need it:** the
Sideloadly **daemon auto-refreshes** the installed build's signature before the 7-day
free-cert expiry, so a re-install is only required when the `.ipa` itself changes (new
code) — a quick GUI drag-drop/confirm (first time also does Apple-ID login + 2FA).

So the practical smooth loop is: **start `remote tunneld` once per boot (admin)**, and
after any new-build install, `device-rig.bat -Action run` **launches, arms, plays, and
measures without you** — re-installs only when the code changed.

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
