// VS-free WinRT BLE central radio (csc, no VS). The full data pipe behind the
// WindowsBleTransport: scan for the LurMotorn service, connect UNPAIRED, read the
// peer's persistent device-id, subscribe to datagram notifications, and then relay
// datagrams both ways over stdio so a C++ host (MinGW, which can't link WinRT) can
// drive a real BLE link to the unmodified Android app.
//
// Why a subprocess: C# WinRT can't be linked into the MinGW C++ exe, so the
// architecture is subprocess + IPC. This process is central-ONLY (Windows'
// peripheral mode is flaky; the Android side plays peripheral). It never advertises
// and runs no GATT server, so the phone always settles as the peripheral naturally.
//
// IPC framing (binary, length-prefixed) — see WindowsBleTransport.cpp for the peer:
//   Each frame: [1 byte tag][4 byte LE length][length bytes payload]
//   Radio -> host (stdout):  'C' connected  (payload = peer device-id ASCII)
//                            'D' datagram in (payload = datagram bytes)
//                            'X' disconnected(payload empty)
//   host -> radio (stdin):   'D' datagram out(payload = datagram bytes to send)
// stdout carries ONLY these binary frames; ALL human-readable logs go to stderr so
// they never corrupt the datagram stream.
//
// .NET-Framework csc gotcha (see BleConnect.cs): the `await` GetAwaiter extension
// trips on winmd/facade type identity, so we drive IAsyncOperation<T> via
// .Status/.GetResults() through the Wait<T> helper — never `await`.
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using Windows.Foundation;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

class BleRadio {
    static readonly Guid Service   = new Guid("4C55524D-4F54-4F52-4E00-5472616E7370");
    static readonly Guid DevIdChar = new Guid("4C55524D-4F54-4F52-4E02-4E6F6E636500");
    static readonly Guid DataChar  = new Guid("4C55524D-4F54-4F52-4E01-446174616772");

    // --- Drive a WinRT async op without `await` (see file header). ---
    static T Wait<T>(IAsyncOperation<T> op) {
        while (op.Status == AsyncStatus.Started) Thread.Sleep(5);
        if (op.Status != AsyncStatus.Completed) throw new Exception("async failed: " + op.Status);
        return op.GetResults();
    }

    static void Log(string m) { Console.Error.WriteLine("radio: " + m); Console.Error.Flush(); }

    // --- Binary IPC on the raw stdout/stdin streams (no text-mode CRLF mangling). ---
    static Stream g_out;
    static readonly object g_outLock = new object();

    static void SendFrame(char tag, byte[] payload) {
        if (payload == null) payload = new byte[0];
        var hdr = new byte[5];
        hdr[0] = (byte)tag;
        int n = payload.Length;
        hdr[1] = (byte)(n & 0xFF); hdr[2] = (byte)((n >> 8) & 0xFF);
        hdr[3] = (byte)((n >> 16) & 0xFF); hdr[4] = (byte)((n >> 24) & 0xFF);
        lock (g_outLock) {
            g_out.Write(hdr, 0, 5);
            if (n > 0) g_out.Write(payload, 0, n);
            g_out.Flush();
        }
    }

    // The datagram characteristic of the CURRENT link, or null when disconnected.
    // Set on connect, cleared on drop; read by the stdin pump to write outbound.
    static volatile GattCharacteristic g_data;

    // Signalled when the current link drops, so the main loop rescans + reconnects.
    static readonly ManualResetEventSlim g_dropped = new ManualResetEventSlim(false);

    // Connection-quality counters (write RTT = one ATT round trip; see StdinPump).
    static long g_txCount, g_rxCount, g_txBytes, g_rxBytes;
    static double g_txMs, g_txMin = double.MaxValue, g_txMax;

    // Keep the connection-parameters request referenced so the OS honours the fast
    // interval for the life of the link (issue #68); GC'ing it drops the preference.
    static object g_connReq;

    static int Main() {
        g_out = Console.OpenStandardOutput();
        Log("central radio starting (scan -> connect -> relay); Ctrl-C to stop");

        // One background thread pumps host->radio outbound datagrams for the app's
        // whole lifetime, independent of connect/reconnect cycles.
        var pump = new Thread(StdinPump) { IsBackground = true };
        pump.Start();

        // Reconnect loop: survive the dev-rig's `svc bluetooth disable/enable` chaos.
        // Each iteration is one full link lifetime (scan -> connect -> serve -> drop).
        while (true) {
            try {
                RunOneLink();
            } catch (Exception e) {
                Log("link error: " + e.Message);
            }
            g_data = null;
            g_dropped.Reset();
            Thread.Sleep(500);  // brief backoff before rescanning
            Log("rescanning for peer...");
        }
    }

    // Scan until the phone is seen advertising our service; returns its BLE address.
    static ulong ScanForPeer() {
        ulong addr = 0;
        var found = new ManualResetEventSlim(false);
        var w = new BluetoothLEAdvertisementWatcher { ScanningMode = BluetoothLEScanningMode.Active };
        w.Received += (s, e) => {
            if (found.IsSet) return;
            foreach (var g in e.Advertisement.ServiceUuids)
                if (g == Service) { addr = e.BluetoothAddress; found.Set(); return; }
        };
        w.Start();
        Log("scanning for LurMotorn service...");
        found.Wait();  // block until the phone advertises (it may not be up yet)
        w.Stop();
        return addr;
    }

    static void RunOneLink() {
        ulong addr = ScanForPeer();
        Log(string.Format("peer addr={0:X12} — connecting (unpaired)...", addr));

        var dev = Wait(BluetoothLEDevice.FromBluetoothAddressAsync(addr));
        if (dev == null) { Log("FromBluetoothAddressAsync null"); return; }

        var svc = Wait(dev.GetGattServicesForUuidAsync(Service));
        if (svc.Status != GattCommunicationStatus.Success || svc.Services.Count == 0) {
            Log("no GATT service: " + svc.Status); dev.Dispose(); return;
        }
        var s = svc.Services[0];

        // Ask the OS for the shortest connection interval it allows (issue #68): the
        // central owns the interval, and it's the dominant latency term (baseline write
        // RTT was interval-bound, min 26ms vs avg 111ms). Needs Windows 10 2004+.
        try {
            var req = dev.RequestPreferredConnectionParameters(
                BluetoothLEPreferredConnectionParameters.ThroughputOptimized);
            g_connReq = req;  // keep referenced so the preference persists
            Log("connection params -> ThroughputOptimized: " +
                (req != null ? req.Status.ToString() : "null"));
        } catch (Exception e) {
            Log("connection-params request unavailable (needs Win10 2004+): " + e.Message);
        }

        // Read the peer's persistent device-id (drives colour/stats + role sanity).
        string peerId = "";
        var idc = Wait(s.GetCharacteristicsForUuidAsync(DevIdChar));
        if (idc.Status == GattCommunicationStatus.Success && idc.Characteristics.Count > 0) {
            var rd = Wait(idc.Characteristics[0].ReadValueAsync(BluetoothCacheMode.Uncached));
            if (rd.Status == GattCommunicationStatus.Success) {
                var b = new byte[rd.Value.Length];
                DataReader.FromBuffer(rd.Value).ReadBytes(b);
                peerId = System.Text.Encoding.ASCII.GetString(b);
            }
        }
        Log("peer device id = '" + peerId + "'");

        // Grab the datagram characteristic and turn on notifications. On the phone,
        // a central enabling notifications (the CCCD write) IS the "link is live"
        // signal, so this must succeed before we announce Connected.
        var dc = Wait(s.GetCharacteristicsForUuidAsync(DataChar));
        if (dc.Status != GattCommunicationStatus.Success || dc.Characteristics.Count == 0) {
            Log("no datagram characteristic: " + dc.Status); dev.Dispose(); return;
        }
        var data = dc.Characteristics[0];

        data.ValueChanged += (s2, e2) => {
            var b = new byte[e2.CharacteristicValue.Length];
            DataReader.FromBuffer(e2.CharacteristicValue).ReadBytes(b);
            g_rxCount++; g_rxBytes += b.Length;
            Log(string.Format("RX notify {0}B (total {1} pkt / {2}B)", b.Length, g_rxCount, g_rxBytes));
            SendFrame('D', b);          // datagram in -> host
        };
        var cccd = Wait(data.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue.Notify));
        if (cccd != GattCommunicationStatus.Success) {
            Log("enable notify failed: " + cccd); dev.Dispose(); return;
        }

        // Watch for the link dropping (phone off, out of range, bt toggled).
        dev.ConnectionStatusChanged += (d, o) => {
            if (d.ConnectionStatus == BluetoothConnectionStatus.Disconnected) {
                Log("peer disconnected");
                g_dropped.Set();
            }
        };

        g_data = data;
        SendFrame('C', System.Text.Encoding.ASCII.GetBytes(peerId));  // link live -> host
        Log("LINKED: notifications on, relaying datagrams");

        g_dropped.Wait();               // serve until the link drops
        SendFrame('X', null);           // disconnected -> host
        g_data = null;
        try { dev.Dispose(); } catch {}
    }

    // Read length-prefixed 'D' frames from the host and write them to the peer.
    static void StdinPump() {
        var stdin = Console.OpenStandardInput();
        var hdr = new byte[5];
        while (true) {
            if (!ReadExact(stdin, hdr, 5)) { Log("host stdin closed — exiting"); Environment.Exit(0); }
            int n = hdr[1] | (hdr[2] << 8) | (hdr[3] << 16) | (hdr[4] << 24);
            var payload = new byte[n];
            if (n > 0 && !ReadExact(stdin, payload, n)) { Log("host stdin truncated — exiting"); Environment.Exit(0); }
            if ((char)hdr[0] != 'D') continue;   // only datagrams flow host->radio today

            var data = g_data;
            if (data == null) { Log("drop outbound datagram: no link"); continue; }
            try {
                var writer = new DataWriter();
                writer.WriteBytes(payload);
                // The phone's datagram characteristic is WRITE (with response), so the
                // async completes on the ATT write RESPONSE — one BLE connection-interval
                // round trip. Timing it is a real per-datagram link-latency probe.
                var sw = Stopwatch.StartNew();
                var res = Wait(data.WriteValueWithResultAsync(writer.DetachBuffer(),
                                                              GattWriteOption.WriteWithResponse));
                sw.Stop();
                double ms = sw.Elapsed.TotalMilliseconds;
                if (res.Status != GattCommunicationStatus.Success) {
                    Log("write failed: " + res.Status);
                } else {
                    g_txCount++; g_txBytes += payload.Length;
                    g_txMs += ms; if (ms < g_txMin) g_txMin = ms; if (ms > g_txMax) g_txMax = ms;
                    Log(string.Format("TX {0}B rtt={1:F1}ms (n={2} avg={3:F1} min={4:F1} max={5:F1})",
                                      payload.Length, ms, g_txCount, g_txMs / g_txCount, g_txMin, g_txMax));
                }
            } catch (Exception e) {
                Log("write threw: " + e.Message);
            }
        }
    }

    static bool ReadExact(Stream s, byte[] buf, int n) {
        int off = 0;
        while (off < n) {
            int r = s.Read(buf, off, n - off);
            if (r <= 0) return false;
            off += r;
        }
        return true;
    }
}
