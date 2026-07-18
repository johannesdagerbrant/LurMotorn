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

## Build / run

```
powershell -ExecutionPolicy Bypass -File Tools\BleDevRig\build.ps1 -Source BleConnect.cs -Run
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

## Status / next

Scan + connect + GATT read + channel discovery are proven. Next: subscribe to datagram
notifications + write datagrams, then bridge datagrams to the C++ engine
(`WindowsBleTransport : ITransport`, C# radio subprocess ⇄ C++ over a pipe) so the desktop
chess app is a live BLE peer, driven by a one-command `dev-rig.ps1`.
