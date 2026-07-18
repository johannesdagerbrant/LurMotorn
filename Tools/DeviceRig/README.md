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

Android is fully headless (adb). iOS has two gates that are Apple's design, not ours —
after each is done **once**, I can drive the rest myself:

| Step | Android | iOS | Headless after… |
|---|---|---|---|
| Install a new build | `adb install -r` | `-Action install` → Sideloadly signs+installs the `.ipa` via its `localhost:28811` `/enqueue` API, reusing a **cached credential token** | one interactive Apple-ID login + 2FA in the Sideloadly GUI (then silent; the Sideloadly daemon auto-refreshes before the 7-day free-cert expiry) |
| Launch the app | `adb monkey` | `-Action launch` → `pymobiledevice3 developer dvt launch` | a RemoteXPC **tunnel** running: `sudo python -m pymobiledevice3 remote tunneld` (needs admin; **one-time per boot**, persists) |
| Arm autoplay | `setprop debug.lur.autoplay 1` | push `Documents/autoplay` marker (`apps push … Documents/autoplay`, container-vend — **not** `--documents`, which 404s on iOS 26) | always |
| Tail engine log | `logcat -s OnlyChess:*` | `syslog live | grep OnlyChess` (the `-m` message match does **not** filter on iOS 26) | always |
| Screenshot | `screencap` | `developer dvt screenshot` | the same tunnel as launch |

So the smooth iOS loop is: **you do two one-time things** — log into Sideloadly once
(2FA), and start `remote tunneld` once per boot with admin — and after that
`device-rig.bat -Action run` installs, launches, arms, plays, and measures without you.

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
