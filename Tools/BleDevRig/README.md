# BleDevRig — Windows↔Android BLE dev rig (issue #58)

A **VS-free** Windows BLE **central** that speaks the LurMotorn GATT protocol to an
**unmodified Android** phone, so the desktop can be a real second peer for developing,
debugging, and optimizing the Bluetooth networking. Loopback has no radio; two phones
have no debugger — this rig has both.

## Why C# + WinRT (and why it's still VS-free)

A Windows BLE central needs active advertisement scanning + connect-to-advertiser, which
only the **WinRT** API (`Windows.Devices.Bluetooth`) provides (the classic Win32 GATT C
API only works on already-paired devices). WinRT can't be consumed from MinGW, and
PowerShell 5.1 can't subscribe to WinRT events / deadlocks on async. So these tools are
**C#**, compiled with the **.NET Framework `csc.exe` that ships with Windows** — no
Visual Studio, no SDK install, no license. `build.ps1` finds `csc` + the SDK winmds.

## Validated on hardware (Galaxy A14, app advertising)

- `BleScan.cs` — scans; found the phone advertising our service (`svcUuidPresent=True`).
- `BleConnect.cs` — connects **unpaired**, reads the device-id characteristic (the peer's
  persistent GUID), and confirms the datagram characteristic exists with props
  `Write, Notify`. **No pairing needed.**

## The one-command loop

```
Tools\BleDevRig\dev-rig.bat      REM build + install + play + force a reconnect + capture
```

`dev-rig.ps1` builds the radio + the `onlychess_desktop --ble` endpoint, (re)connects
wireless adb, installs the app and forces it to advertise as a peripheral, launches the
desktop peer, forces ≥1 link-death reconnect (`svc bluetooth disable/enable`), tails both
logs into `.logs\`, and pulls the flight recorder on exit. Options: `-Serial`, `-SkipBuild`,
`-SkipInstall`, `-Reconnects N`, `-ReconnectAfterSec S`, `-DurationSec S`.
See operational notes (adapter power management, LMP version) in `scripts\README.md`.

## Pieces

- **`BleRadio.cs` → `BleRadio.exe`** — the full central radio: scan → connect (unpaired)
  → read peer device-id → subscribe to datagram notifications → relay datagrams both ways
  over stdio (length-prefixed binary frames; logs on stderr). Reconnect loop survives BT
  toggling. This is the subprocess `WindowsBleTransport` drives.
- **`WindowsBleTransport`** (`Games/Chess/Desktop/`) — the C++ `ITransport` that spawns
  `BleRadio.exe`, bridges datagrams over the pipe, and lifts inbound events onto the engine
  thread via `EventInbox`/`Pump()` (issue #40). Lives in the app build (not the
  cross-platform `Modules/Transport`) because it's a Windows-only dev instrument that spawns
  a subprocess — like `AndroidBleTransport` lives in the Android app.
- **`BleScan.cs` / `BleConnect.cs`** — the original bring-up probes (scan; connect + read +
  channel discovery), kept as the minimal reproducers.

## Manual run (without the full loop)

```
powershell -ExecutionPolicy Bypass -File Tools\BleDevRig\build.ps1 -Source BleRadio.cs
build-desktop\Games\Chess\Desktop\onlychess_desktop.exe --ble Tools\BleDevRig\BleRadio.exe
```

The Android app must be **advertising** (foreground, fresh identity so it serves as a
peripheral). Agentic setup over adb (no manual taps):

```
adb shell pm clear com.lurmotorn.onlychess
adb shell pm grant com.lurmotorn.onlychess android.permission.BLUETOOTH_ADVERTISE
adb shell pm grant com.lurmotorn.onlychess android.permission.BLUETOOTH_CONNECT
adb shell pm grant com.lurmotorn.onlychess android.permission.BLUETOOTH_SCAN
adb shell monkey -p com.lurmotorn.onlychess -c android.intent.category.LAUNCHER 1
```

## Protocol (must match `Modules/Transport/Public/Lur/Transport/BleProtocol.h`)

- Service  `4C55524D-4F54-4F52-4E00-5472616E7370`
- Datagram `4C55524D-4F54-4F52-4E01-446174616772` (central writes; peripheral notifies)
- Device-id `4C55524D-4F54-4F52-4E02-4E6F6E636500` (central reads; drives `DecideBleRole`)

Because the PC is central-**only** and never advertises, the phone always settles as the
peripheral naturally — no GATT server on the PC is needed. The PC learns the peer's GUID
from the device-id read; peer identity for colour/stats still flows through the engine's
own `Hello` handshake over the datagram channel.

## Status

The whole central data path is wired: scan → connect → read → subscribe → **relay
datagrams**, bridged into a live desktop chess peer over `WindowsBleTransport`, all driven
by `dev-rig.ps1`. Central data path proven on hardware (Galaxy A14). Remaining: soak +
throughput measurement against the phone radio.
