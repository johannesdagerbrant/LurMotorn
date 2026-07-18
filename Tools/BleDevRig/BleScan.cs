// VS-free WinRT BLE scanner: compiled with the .NET Framework csc.exe (ships with
// Windows), driving Windows.Devices.Bluetooth at runtime. Proves the agentic, VS-free
// path can SEE the Galaxy advertising the LurMotorn service. Events work in C#.
using System;
using System.Collections.Generic;
using System.Threading;
using Windows.Devices.Bluetooth.Advertisement;

class BleScan {
    static readonly Guid LurService = new Guid("4C55524D-4F54-4F52-4E00-5472616E7370");

    static void Main() {
        var seen = new HashSet<ulong>();
        var w = new BluetoothLEAdvertisementWatcher();
        w.ScanningMode = BluetoothLEScanningMode.Active;
        w.Received += (s, e) => {
            string name = e.Advertisement.LocalName;
            bool ours = false;
            foreach (var g in e.Advertisement.ServiceUuids)
                if (g == LurService) ours = true;
            bool match = ours || (!string.IsNullOrEmpty(name) && name.Contains("LurMotorn"));
            if (match && seen.Add(e.BluetoothAddress))
                Console.WriteLine(string.Format("FOUND name='{0}' addr={1:X12} rssi={2} svcUuidPresent={3}",
                    name, e.BluetoothAddress, e.RawSignalStrengthInDBm, ours));
        };
        w.Start();
        Console.WriteLine("scanning 10s for LurMotorn...");
        Thread.Sleep(10000);
        w.Stop();
        Console.WriteLine("done (state=" + w.Status + ")");
    }
}
