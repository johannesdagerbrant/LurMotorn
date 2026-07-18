// VS-free WinRT BLE central connect test (csc, no VS). Scan for the LurMotorn
// service, connect unpaired, read the device-id characteristic, and confirm the
// datagram characteristic + its properties. Uses IAsyncOperation.Status/GetResults
// directly (no await extension -> avoids .NET-Framework winmd/facade type-identity
// pitfalls).
using System;
using System.Threading;
using Windows.Foundation;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

class BleConnect {
    static readonly Guid Service   = new Guid("4C55524D-4F54-4F52-4E00-5472616E7370");
    static readonly Guid DevIdChar = new Guid("4C55524D-4F54-4F52-4E02-4E6F6E636500");
    static readonly Guid DataChar  = new Guid("4C55524D-4F54-4F52-4E01-446174616772");

    static T Wait<T>(IAsyncOperation<T> op) {
        while (op.Status == AsyncStatus.Started) Thread.Sleep(5);
        if (op.Status != AsyncStatus.Completed) throw new Exception("async failed: " + op.Status);
        return op.GetResults();
    }

    static bool HasService(BluetoothLEAdvertisement ad) {
        foreach (var g in ad.ServiceUuids) if (g == Service) return true;
        return false;
    }

    static ulong g_addr;
    static readonly ManualResetEventSlim g_found = new ManualResetEventSlim(false);

    static int Main() {
        var w = new BluetoothLEAdvertisementWatcher { ScanningMode = BluetoothLEScanningMode.Active };
        w.Received += (s, e) => {
            if (!g_found.IsSet && HasService(e.Advertisement)) { g_addr = e.BluetoothAddress; g_found.Set(); }
        };
        w.Start();
        Console.WriteLine("scanning for LurMotorn service...");
        bool ok = g_found.Wait(15000);
        w.Stop();
        if (!ok) { Console.WriteLine("NOT FOUND"); return 1; }
        Console.WriteLine(string.Format("peer addr={0:X12} — connecting (unpaired)...", g_addr));

        var dev = Wait(BluetoothLEDevice.FromBluetoothAddressAsync(g_addr));
        if (dev == null) { Console.WriteLine("FromBluetoothAddressAsync null"); return 1; }
        Console.WriteLine("device name='" + dev.Name + "' status=" + dev.ConnectionStatus);

        var svc = Wait(dev.GetGattServicesForUuidAsync(Service));
        Console.WriteLine("GATT service: " + svc.Status + " count=" + svc.Services.Count);
        if (svc.Services.Count == 0) { dev.Dispose(); return 1; }

        var chr = Wait(svc.Services[0].GetCharacteristicsForUuidAsync(DevIdChar));
        Console.WriteLine("device-id char: " + chr.Status + " count=" + chr.Characteristics.Count);
        if (chr.Characteristics.Count > 0) {
            var rd = Wait(chr.Characteristics[0].ReadValueAsync());
            Console.WriteLine("read: " + rd.Status);
            if (rd.Status == GattCommunicationStatus.Success) {
                var reader = DataReader.FromBuffer(rd.Value);
                var b = new byte[rd.Value.Length];
                reader.ReadBytes(b);
                Console.WriteLine("PEER DEVICE ID = " + System.Text.Encoding.ASCII.GetString(b));
            }
        }

        var dch = Wait(svc.Services[0].GetCharacteristicsForUuidAsync(DataChar));
        Console.WriteLine("datagram char: " + dch.Status + " count=" + dch.Characteristics.Count);
        if (dch.Characteristics.Count > 0)
            Console.WriteLine("datagram props: " + dch.Characteristics[0].CharacteristicProperties);

        dev.Dispose();
        return 0;
    }
}
