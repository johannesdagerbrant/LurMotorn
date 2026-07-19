package com.lurmotorn.onlyrps

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.util.UUID

/**
 * The real Bluetooth Low Energy radio for OnlyRps (issue #3, Android half).
 *
 * Its only job is to move opaque `ByteArray` datagrams between this phone and the
 * peer; all framing/encoding/game logic stays in C++ (the Lur::Transport backend
 * consumes this over JNI).
 *
 * The GATT role is decided IN-BAND (it must be, to interoperate with iOS, which
 * cannot advertise custom data). Both phones run a GATT server, advertise only the
 * service UUID, and scan. On discovering a peer, a phone connects as central and
 * reads the peer's device-id characteristic; C++ `DecideBleRole` (the shared single
 * source of truth) then uses the two ids to settle who keeps the link: the larger
 * id stays central, the smaller drops that connection and serves as peripheral,
 * letting its peer connect to it. Both keep advertising/scanning until the
 * canonical link is up, so it self-corrects. See BleProtocol.h.
 *
 * The device id is PERSISTENT (a GUID minted once by the engine's Modules/Save and
 * kept in Context.filesDir), so the role settled above is STABLE across app
 * restarts — a restarted phone rejoins its peer instead of flipping roles and
 * stranding it (the reconnect-on-restart fix, issue #17).
 */
class BleShim(private val context: Context) {

    companion object {
        private const val TAG = "OnlyRps"

        // MUST match Lur::Transport::BleProtocol (Modules/.../BleProtocol.h).
        // Distinct per-game service UUID (...7371 vs chess's ...7370) so RPS phones only
        // pair with each other, never a chess phone (they share the engine transport).
        private val SERVICE_UUID = UUID.fromString("4C55524D-4F54-4F52-4E00-5472616E7371")
        private val DATAGRAM_UUID = UUID.fromString("4C55524D-4F54-4F52-4E01-446174616772")
        private val DEVICE_ID_UUID = UUID.fromString("4C55524D-4F54-4F52-4E02-4E6F6E636500")
        // Standard Client Characteristic Configuration Descriptor (enables notify).
        private val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")

        private const val ROLE_PERIPHERAL = 0
        private const val ROLE_CENTRAL = 1

        init {
            // Same .so NativeActivity loads (android.app.lib_name = "onlyrps").
            System.loadLibrary("onlyrps")
        }
    }

    // --- JNI: into C++ (defined in AndroidBleTransport.cpp) ---
    private external fun nativeSetShim()
    private external fun nativeOnConnected(asPeripheral: Boolean)
    private external fun nativeOnDisconnected()
    private external fun nativeOnReceived(bytes: ByteArray)
    private external fun nativeDecideRole(localId: ByteArray, peerId: ByteArray): Int
    private external fun nativeLoadOrCreateDeviceId(dir: String): ByteArray
    private external fun nativeLoadPeerId(dir: String): ByteArray
    private external fun nativeSavePeerId(dir: String, bytes: ByteArray)

    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    // The persistent device id (a GUID minted once by Modules/Save, kept in
    // filesDir) — stable across restarts, unlike the old random session nonce, so
    // the BLE role it drives never flips on relaunch (issue #17).
    private val deviceId: ByteArray = nativeLoadOrCreateDeviceId(context.filesDir.absolutePath)

    // The LAST linked peer's id (empty until the first pairing). When present we know
    // our role up front and skip the discovery collision on reconnect (issue #17 Step 3).
    private var peerId: ByteArray = nativeLoadPeerId(context.filesDir.absolutePath)

    private fun rememberPeer(id: ByteArray) {
        if (id.isEmpty() || id.contentEquals(peerId)) return
        peerId = id
        try { nativeSavePeerId(context.filesDir.absolutePath, id) } catch (_: Exception) {}
    }

    private var advertiser: BluetoothLeAdvertiser? = null
    private var scanner: BluetoothLeScanner? = null
    private var gattServer: BluetoothGattServer? = null
    private var serverDatagram: BluetoothGattCharacteristic? = null
    private var connectedCentral: BluetoothDevice? = null   // the peripheral's live link
    private var gattClient: BluetoothGatt? = null
    private var clientDatagram: BluetoothGattCharacteristic? = null

    // Post delayed retries/watchdogs on the main thread.
    private val handler = android.os.Handler(android.os.Looper.getMainLooper())

    // Send flow control (issue #72). Android allows only ONE outstanding GATT operation:
    // a second write/notify issued before the previous one's completion callback is
    // SILENTLY DROPPED. Under autoplay (a move every turn + keepalives + resync) that
    // dropped nearly every move, so state only propagated via the slower resync. We
    // serialize sends here: enqueue, issue one, and issue the next only on
    // onCharacteristicWrite / onNotificationSent. No added network time — writes stay
    // WRITE_NO_RESPONSE, just paced to the connection interval instead of overrunning it.
    private val sendQueue = ArrayDeque<ByteArray>()
    private val sendLock = Any()
    private var sendInFlight = false
    private var sendWatchdogTok = 0

    @Volatile private var started = false
    @Volatile private var linked = false
    @Volatile private var connecting = false   // an outgoing central attempt is mid-flight
    @Volatile private var decidedPeripheral = false  // we settled as peripheral; stop connecting out

    // Discovery watchdog (#79): cached-role one-sidedness is keyed to a peer identity
    // we have NOT verified this session — if the peer re-rolled its GUID
    // (reset/reinstall), advertise-only leaves BOTH phones deaf forever. Any unlinked
    // stretch longer than the watchdog drops the gates and resumes the symmetric
    // dance; the fresh in-band tie-break then re-caches the true role.
    private val watchdogHandler = Handler(Looper.getMainLooper())
    private val discoveryWatchdog = Runnable { onDiscoveryWatchdog() }

    init {
        nativeSetShim()
    }

    /** Permissions this device needs to advertise/scan/connect over BLE. */
    fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(
                Manifest.permission.BLUETOOTH_ADVERTISE,
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
            )
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    // --- Called FROM C++ (the Lur::Transport BLE backend) ---

    /** Send one datagram to the peer. Enqueued + serialized via flow control (issue #72). */
    @Suppress("unused")
    fun send(bytes: ByteArray) {
        synchronized(sendLock) {
            sendQueue.addLast(bytes)
            pumpSendLocked()
        }
    }

    /** Issue the next queued datagram iff none is outstanding. Call under sendLock. */
    private fun pumpSendLocked() {
        if (sendInFlight || sendQueue.isEmpty()) return
        val bytes = sendQueue.first()
        val issued = try {
            val client = gattClient
            val clientCh = clientDatagram
            val central = connectedCentral
            val serverCh = serverDatagram
            if (client != null && clientCh != null) {           // we are central -> write
                clientCh.value = bytes
                // Write WITHOUT response (issue #49): drop the ATT ack round-trip per
                // datagram. Flow control (below) still paces us to one outstanding write.
                clientCh.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                client.writeCharacteristic(clientCh)            // true if accepted for tx
            } else if (central != null && serverCh != null) {   // we are peripheral -> notify
                serverCh.value = bytes
                gattServer?.notifyCharacteristicChanged(central, serverCh, false) ?: false
            } else {
                sendQueue.clear()                               // no link -> drop the backlog
                false
            }
        } catch (e: SecurityException) {
            Log.e(TAG, "send: missing BLE permission", e); false
        }
        if (issued) {
            sendQueue.removeFirst()
            sendInFlight = true
            // Watchdog: if the completion callback never fires (dropped link), don't stall
            // the queue forever — clear + resume after a bounded wait.
            val tok = ++sendWatchdogTok
            handler.postDelayed({
                synchronized(sendLock) {
                    if (sendInFlight && tok == sendWatchdogTok) { sendInFlight = false; pumpSendLocked() }
                }
            }, 300)
        }
        // If not issued (stack momentarily busy / returned false), leave it queued; the
        // in-flight completion (or the watchdog) will call pumpSendLocked again.
    }

    /** A send completed (write ack'd locally / notification handed to the stack). */
    private fun onSendComplete() {
        synchronized(sendLock) {
            sendInFlight = false
            ++sendWatchdogTok                                   // invalidate the pending watchdog
            pumpSendLocked()
        }
    }

    /** Called FROM C++ when the net keepalive times out: treat the silently-gone peer
     *  as a link loss now, instead of waiting out the BLE supervision timeout. */
    @Suppress("unused")
    fun resetLink() {
        handler.post {
            if (linked) { Log.i(TAG, "net keepalive timeout -> forcing link reset"); onLinkLost() }
        }
    }

    // --- Called from OnlyRpsActivity once permissions are granted ---

    fun onPermissionsReady() {
        if (started) return
        val a = adapter
        if (a == null || !a.isEnabled) {
            Log.e(TAG, "BLE unavailable (no adapter or Bluetooth off)")
            return
        }
        started = true
        Log.i(TAG, "device id: ${deviceId.size}B, cached peer: ${peerId.size}B")
        startGattServer()
        startDiscovery()
    }

    /** Begin discovery. If we already know the peer (a prior link), we know our role
     *  and act one-sided — the peripheral only advertises, the central only scans —
     *  so the two phones never both connect out at once (the reconnect collision,
     *  issue #17 Step 3). With no cached peer (first pairing) we do the full symmetric
     *  dance (advertise + scan + connect + in-band tie-break). */
    private fun startDiscovery() {
        if (peerId.isNotEmpty()) {
            if (nativeDecideRole(deviceId, peerId) == ROLE_PERIPHERAL) {
                decidedPeripheral = true          // known peripheral: never connect out
                Log.i(TAG, "cached role: PERIPHERAL — advertise + serve, no scan")
                startAdvertising()
            } else {
                decidedPeripheral = false
                Log.i(TAG, "cached role: CENTRAL — scan + connect, no advertise")
                startScanning()
            }
        } else {
            Log.i(TAG, "no cached peer — full discovery (advertise + scan)")
            startAdvertising()
            startScanning()
        }
        armDiscoveryWatchdog()  // #79: one-sidedness may not outlive the watchdog
    }

    private fun armDiscoveryWatchdog() {
        watchdogHandler.removeCallbacks(discoveryWatchdog)
        if (!linked) watchdogHandler.postDelayed(discoveryWatchdog, 8000)
    }

    @Synchronized
    private fun onDiscoveryWatchdog() {
        if (linked) return
        Log.i(TAG, "discovery watchdog: no link in 8s — dropping cached-role gates, going symmetric (#79)")
        decidedPeripheral = false
        connecting = false
        startAdvertising()
        startScanning()
        armDiscoveryWatchdog()  // keep watching until a link forms
    }

    private fun startAdvertising() {
        val adv = adapter?.bluetoothLeAdvertiser ?: run { Log.e(TAG, "no BLE advertiser"); return }
        advertiser = adv
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setConnectable(true)
            .setTimeout(0)
            .build()
        // Service UUID only — iOS can advertise no more than this, so neither do we.
        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(false)
            .addServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        try {
            adv.startAdvertising(settings, data, advertiseCallback)
        } catch (e: SecurityException) {
            Log.e(TAG, "startAdvertising: missing BLE permission", e)
        }
    }

    private fun startScanning() {
        val sc = adapter?.bluetoothLeScanner ?: run { Log.e(TAG, "no BLE scanner"); return }
        scanner = sc
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            sc.startScan(listOf(filter), settings, scanCallback)
            Log.i(TAG, "BLE up: serving + advertising + scanning for LurMotorn peers")
        } catch (e: SecurityException) {
            Log.e(TAG, "startScan: missing BLE permission", e)
        }
    }

    private fun stopAdvertising() {
        try { advertiser?.stopAdvertising(advertiseCallback) } catch (_: SecurityException) {}
    }

    private fun stopScanning() {
        try { scanner?.stopScan(scanCallback) } catch (_: SecurityException) {}
    }

    /** The canonical link is up — stop discovery so the radio settles. */
    private fun onLinked(asPeripheral: Boolean) {
        if (linked) return
        linked = true
        watchdogHandler.removeCallbacks(discoveryWatchdog)  // #79: link up, stop watching
        stopScanning()
        stopAdvertising()
        nativeOnConnected(asPeripheral)
    }

    /** The live link dropped — reset role state and resume discovery so it re-forms. */
    @Synchronized
    private fun onLinkLost() {
        if (!linked) return
        linked = false
        decidedPeripheral = false
        connecting = false
        connectedCentral = null
        clientDatagram = null
        try { gattClient?.close() } catch (_: SecurityException) {}
        gattClient = null
        synchronized(sendLock) { sendQueue.clear(); sendInFlight = false; ++sendWatchdogTok }  // drop stale send state (#72)
        nativeOnDisconnected()
        startDiscovery()   // role-aware: cached peer -> one-sided, no reconnect collision
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartFailure(errorCode: Int) { Log.e(TAG, "advertise failed: $errorCode") }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (linked || connecting || decidedPeripheral) return
            connecting = true
            Log.i(TAG, "scan: found a LurMotorn peer, connecting as central")
            connectAsCentral(result.device)
        }

        override fun onScanFailed(errorCode: Int) { Log.e(TAG, "scan failed: $errorCode") }
    }

    // --- GATT server (every device runs one; the peripheral's is the live link) ---

    private fun startGattServer() {
        val mgr = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        try {
            val server = mgr.openGattServer(context, gattServerCallback)
            gattServer = server
            val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)

            val datagram = BluetoothGattCharacteristic(
                DATAGRAM_UUID,
                // WRITE_NO_RESPONSE alongside WRITE (issue #49): let a central write
                // datagrams without the ATT ack round-trip. NOTIFY carries the reverse path.
                BluetoothGattCharacteristic.PROPERTY_WRITE or
                    BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE or
                    BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattCharacteristic.PERMISSION_WRITE,
            )
            datagram.addDescriptor(
                BluetoothGattDescriptor(
                    CCCD_UUID,
                    BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE,
                )
            )
            val deviceIdCh = BluetoothGattCharacteristic(
                DEVICE_ID_UUID,
                BluetoothGattCharacteristic.PROPERTY_READ,
                BluetoothGattCharacteristic.PERMISSION_READ,
            )
            service.addCharacteristic(datagram)
            service.addCharacteristic(deviceIdCh)
            server.addService(service)
            serverDatagram = datagram
        } catch (e: SecurityException) {
            Log.e(TAG, "openGattServer: missing BLE permission", e)
        }
    }

    private val gattServerCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_DISCONNECTED && device == connectedCentral) {
                onLinkLost()
            }
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int, characteristic: BluetoothGattCharacteristic,
        ) {
            // Hand the connecting central our device id so it can run the role tie-break.
            val full = if (characteristic.uuid == DEVICE_ID_UUID) deviceId else ByteArray(0)
            // Honor the read offset: the id (32 bytes) exceeds a default-MTU ATT read
            // (~22 bytes), so a central that hasn't negotiated a larger MTU issues a
            // LONG read — a second request at offset>0. We must return only the bytes
            // from that offset on; returning the full array corrupts the central's
            // reassembly (this stranded the iOS<->Android role tie-break, issue #17).
            val value = if (offset in 0..full.size) full.copyOfRange(offset, full.size) else ByteArray(0)
            Log.i(TAG, "serve device id: uuid=${characteristic.uuid} offset=$offset -> ${value.size}B")
            try {
                gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value)
            } catch (_: SecurityException) {}
        }

        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray,
        ) {
            if (characteristic.uuid == DATAGRAM_UUID) nativeOnReceived(value)
            if (responseNeeded) {
                try { gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null) }
                catch (_: SecurityException) {}
            }
        }

        // Peripheral notification handed off to the stack -> send the next queued datagram
        // (flow control for the peripheral direction, mirrors onCharacteristicWrite; #72).
        override fun onNotificationSent(device: BluetoothDevice, status: Int) {
            onSendComplete()
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice, requestId: Int, descriptor: BluetoothGattDescriptor,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray,
        ) {
            // The canonical central enabling notifications IS the "link is live" signal.
            if (descriptor.uuid == CCCD_UUID) {
                connectedCentral = device
                onLinked(asPeripheral = true)
                Log.i(TAG, "peripheral: central linked")
            }
            if (responseNeeded) {
                try { gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null) }
                catch (_: SecurityException) {}
            }
        }
    }

    // --- GATT client (central) ---

    private fun connectAsCentral(device: BluetoothDevice) {
        // Stop scanning before connecting: on many Android BLE stacks a scan running
        // concurrently with connectGatt causes the connect to fail (GATT status 133)
        // or hang with no callback — the reconnect stall we hit (issue #17). We resume
        // scanning if the connect fails (see onConnectionStateChange).
        stopScanning()
        try {
            Log.i(TAG, "central: connectGatt -> ${device.address}")
            val g = device.connectGatt(context, false, gattClientCallback, BluetoothDevice.TRANSPORT_LE)
            gattClient = g
            // Watchdog: Android can silently never call back (a hung connect). If this
            // attempt hasn't linked or been resolved within a few seconds, tear it down
            // and retry cleanly.
            handler.postDelayed({
                if (!linked && !decidedPeripheral && gattClient === g) {
                    Log.i(TAG, "central: connect watchdog -> tearing down and retrying")
                    dropClient(g, rescan = false)
                    scheduleRescan()
                }
            }, 6000L)
        } catch (e: SecurityException) {
            Log.e(TAG, "connectGatt: missing BLE permission", e)
            connecting = false
            scheduleRescan()
        }
    }

    /** Resume scanning after a short delay. A collided/failed connect must NOT be
     *  retried immediately: the peer (doing its own exploratory connect) needs a
     *  moment to settle into peripheral-only, and the shared LE link needs to finish
     *  tearing down, or the retry collides again / hangs (issue #17 reconnect). */
    private fun scheduleRescan() {
        handler.postDelayed({
            if (started && !linked && !decidedPeripheral && !connecting) startScanning()
        }, 1500L)
    }

    /** Tear down a client connection. Always closes (a leaked gatt makes the NEXT
     *  connectGatt fail with 133). Resumes discovery unless we're deliberately idle. */
    private fun dropClient(gatt: BluetoothGatt, rescan: Boolean) {
        try { gatt.disconnect(); gatt.close() } catch (_: SecurityException) {}
        if (gattClient == gatt) { gattClient = null; clientDatagram = null }
        connecting = false
        if (rescan) scheduleRescan()
    }

    private val gattClientCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            try {
                if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                    // The central owns the connection interval, and the interval is the
                    // dominant latency term on the link (issue #68: ~100 ms move-RTT is
                    // mostly interval, not payload). HIGH asks for ~11.25-15 ms intervals.
                    // Best-effort: the peer/controller may negotiate it down. When iOS is
                    // the central the interval is iOS-managed (~15-30 ms) and cannot be
                    // requested — documented as best-effort in #68.
                    val fast = gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    Log.i(TAG, "central: connected, conn priority HIGH=$fast, requesting MTU")
                    gatt.requestMtu(247)
                    return
                }
                // A failed connect (status != SUCCESS) OR a disconnect. MUST close the
                // gatt here — otherwise the client interface leaks and every later
                // connectGatt fails with 133, which is exactly what stalled reconnect.
                Log.i(TAG, "central: down status=$status newState=$newState -> close")
                val wasLiveLink = linked && gatt == gattClient
                try { gatt.close() } catch (_: SecurityException) {}
                if (gatt == gattClient) { gattClient = null; clientDatagram = null }
                connecting = false
                if (wasLiveLink) onLinkLost()   // restarts discovery itself
                else scheduleRescan()           // delayed retry so the collision settles
            } catch (_: SecurityException) {}
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            Log.i(TAG, "central: mtu=$mtu status=$status, discovering services")
            try { gatt.discoverServices() } catch (_: SecurityException) {}
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val service = gatt.getService(SERVICE_UUID) ?: run {
                Log.i(TAG, "central: peer has no LurMotorn service (status=$status), dropping")
                dropClient(gatt, rescan = true); return
            }
            clientDatagram = service.getCharacteristic(DATAGRAM_UUID)
            val deviceIdCh = service.getCharacteristic(DEVICE_ID_UUID) ?: run {
                Log.i(TAG, "central: peer has no device-id characteristic, dropping")
                dropClient(gatt, rescan = true); return
            }
            Log.i(TAG, "central: services discovered, reading peer device id")
            try { gatt.readCharacteristic(deviceIdCh) } catch (_: SecurityException) { dropClient(gatt, rescan = true) }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int,
        ) {
            if (characteristic.uuid != DEVICE_ID_UUID) return
            val readPeerId = characteristic.value ?: ByteArray(0)
            rememberPeer(readPeerId)   // cache for the fast cached-role reconnect next time
            val role = nativeDecideRole(deviceId, readPeerId)
            Log.i(TAG, "read peer id: mine=${deviceId.size}B peer=${readPeerId.size}B -> " +
                if (role == ROLE_CENTRAL) "CENTRAL (keep link)" else "PERIPHERAL (defer)")
            if (role == ROLE_CENTRAL) {
                enableNotifications(gatt)   // we keep this connection as the live link
            } else {
                // We should be the peripheral: drop this connection and let the peer
                // (the canonical central) connect to our server instead.
                decidedPeripheral = true
                stopScanning()
                startAdvertising()  // ensure findable even if we began in cached-central mode
                dropClient(gatt, rescan = false)  // we're peripheral now; wait for the peer
                Log.i(TAG, "central attempt -> we are peripheral; deferring to peer")
                armDiscoveryWatchdog()  // #79: if the peer never comes, go symmetric again
            }
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid == CCCD_UUID) {
                onLinked(asPeripheral = false)
                Log.i(TAG, "central: linked + notifications on")
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == DATAGRAM_UUID) nativeOnReceived(characteristic.value)
        }

        // Central write completed (even WRITE_NO_RESPONSE fires this) -> send the next
        // queued datagram. This is the flow control that stops moves being dropped (#72).
        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (characteristic.uuid == DATAGRAM_UUID) onSendComplete()
        }
    }

    private fun enableNotifications(gatt: BluetoothGatt) {
        val ch = clientDatagram ?: return
        try {
            gatt.setCharacteristicNotification(ch, true)
            val cccd = ch.getDescriptor(CCCD_UUID)
            cccd?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            gatt.writeDescriptor(cccd)
        } catch (_: SecurityException) {}
    }
}
