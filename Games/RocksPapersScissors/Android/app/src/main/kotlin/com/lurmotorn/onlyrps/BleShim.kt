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
        // Mirrors Lur::Transport::BleMaxPeripheralDefers. LOG-ONLY: C++ owns the actual
        // breaker decision (nativeDecideRole), this just labels the line that reports it.
        private const val MAX_FRUITLESS_DEFERS = 2

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
    /** The shared role tie-break. [fruitlessDefers] is how many times we have already deferred
     *  to Peripheral without a peer ever claiming Central; at the threshold C++ breaks the tie
     *  in favour of Central so a both-Peripheral state cannot deadlock (#146). */
    private external fun nativeDecideRole(localId: ByteArray, peerId: ByteArray, fruitlessDefers: Int): Int
    /** Is a device id read off the peer well-formed (32 lowercase hex)? A failed or truncated
     *  GATT read yields bytes that are not an id, and a role decided from those is one the peer
     *  cannot mirror — the deadlock's mechanism (#146). */
    private external fun nativeIsValidDeviceId(id: ByteArray): Boolean
    /** #83: may this central become (or already be) the ONE central we serve? A match is strictly
     *  1:1, and any CCCD subscription used to be taken as the canonical one — so a third device in
     *  the room could redirect a live match's notifications to itself. The rule is shared C++ policy
     *  (Lur::Transport::PeerBinding), host-tested once, because it was wrong in four transports. */
    private external fun nativeAcceptSubscriber(address: String): Boolean
    /** #83: may a datagram from this central enter the engine? Pre-link traffic passes (that is the
     *  handshake); once bound, only the peer's does. */
    private external fun nativeAcceptData(address: String): Boolean
    /** #83: is this the bound peer? Only ITS disconnect ends the match — otherwise an outsider could
     *  kill a live pair simply by leaving. */
    private external fun nativeIsBoundPeer(address: String): Boolean
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

    /** #146: how many times we have connected out, been told "you are the peripheral", and
     *  deferred — with no peer ever arriving to claim Central. Reset the moment a link forms.
     *  Past the threshold the tie-break has forfeited its credibility and we take Central
     *  ourselves, so two peers that both computed Peripheral can't stall forever. */
    private var fruitlessDefers = 0

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
    fun send(bytes: ByteArray, expedited: Boolean) {
        synchronized(sendLock) {
            // #190, which this game never had: an expedited datagram goes to the FRONT, so the
            // one a player is waiting on does not queue behind a keepalive or a bulk resync.
            // Urgency is decided by the engine and passed in — never guessed from the bytes.
            if (expedited) sendQueue.addFirst(bytes) else sendQueue.addLast(bytes)
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

    /** #182: a HARDER reset than resetLink(), escalated to by the net layer once it judges the link
     *  HALF-OPEN — still connected, our writes leave, but the peer's notify path is wedged so nothing
     *  inbound ever arrives. resetLink()/onLinkLost() merely drop+rediscover the LINK; on a wedged BLE
     *  STACK that clears nothing (80 soft resets tried on hardware, only a reboot worked). This tears the
     *  whole radio down and rebuilds it: it refresh()es the client GATT to purge Android's stale
     *  service/subscription cache before closing it, and — unlike onLinkLost — CLOSES AND REOPENS the
     *  GATT SERVER, because the notify path we SERVE as peripheral lives there and re-publishing the
     *  service is the only way to shed a stale subscription. Then discovery restarts from scratch.
     *  Bounded by the Session (MaxRadioRestarts) so it can't itself become the churn that degrades the
     *  radio. May STILL not clear a device-level wedge (the hardware case needed the silent peer to
     *  reboot), so the "LINK STALLED" banner stays the guaranteed floor. */
    @Suppress("unused")
    fun restartRadio() {
        handler.post {
            synchronized(this) {
                if (!started) return@synchronized
                Log.i(TAG, "restartRadio (#182): full radio teardown + rebuild — a soft reset can't " +
                           "clear a wedged BLE stack")
                val wasLinked = linked
                // Client (central side): purge the cached GATT db, then close it fully.
                gattClient?.let { g ->
                    gattRefresh(g)
                    try { g.disconnect(); g.close() } catch (_: SecurityException) {}
                }
                gattClient = null
                clientDatagram = null
                // Server (peripheral side): resetLink/onLinkLost never touch this, but a wedged notify
                // path we serve lives HERE — closing + reopening re-publishes the service and drops any
                // stale subscription (#163 candidate 1). startGattServer() below rebuilds it.
                try { gattServer?.close() } catch (_: SecurityException) {}
                gattServer = null
                serverDatagram = null
                connectedCentral = null
                stopScanning()
                stopAdvertising()
                // Link state resets exactly as a real loss would: engine goes to Searching, role reopens.
                linked = false
                decidedPeripheral = false
                connecting = false
                synchronized(sendLock) { sendQueue.clear(); sendInFlight = false; ++sendWatchdogTok }
                if (wasLinked) nativeOnDisconnected()
                startGattServer()   // fresh server + service
                startDiscovery()    // role-aware rediscovery, same as after a link loss
            }
        }
    }

    /** Clear a BluetoothGatt's cached services/CCCD state via the hidden refresh() method (#182).
     *  Android caches the peer's GATT database and subscription across reconnects; a wedged notify path
     *  can be that stale cache, and refresh() is the only way to drop it — there is no public API, so
     *  reflection is the documented workaround. A failure is non-fatal: the close() that follows still
     *  helps, and the whole escalation is best-effort under a cap. */
    private fun gattRefresh(gatt: BluetoothGatt) {
        try {
            val ok = gatt.javaClass.getMethod("refresh").invoke(gatt) as? Boolean ?: false
            Log.i(TAG, "gatt.refresh() = $ok (#182: purge stale service/subscription cache)")
        } catch (e: Exception) {
            Log.i(TAG, "gatt.refresh() unavailable (${e.javaClass.simpleName}) — relying on close() (#182)")
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

    /**
     * Hand the radio back (#194). This game did not have it, and the fix exists because leaving a
     * registration behind is what the NEXT launch collides with: the advertiser/scanner start earns
     * ALREADY_STARTED, and the app then looks alive while being invisible and deaf. The collision
     * outlives the process, so it is the second launch that breaks, which is what made it hard to
     * attribute.
     *
     * A force-stop cannot run this — but a force-stop is not what we do to players, and a normal
     * exit now leaves nothing behind.
     */
    fun stop() {
        handler.post {
            if (!started) return@post
            started = false
            watchdogHandler.removeCallbacks(discoveryWatchdog)
            stopScanning()
            stopAdvertising()
            try { gattClient?.close() } catch (_: SecurityException) {}
            gattClient = null
            try { gattServer?.close() } catch (_: SecurityException) {}
            gattServer = null
            Log.i(TAG, "BLE stopped - advertiser/scanner/GATT released")
        }
    }

    /** Begin discovery. If we already know the peer (a prior link), we know our role
     *  and act one-sided — the peripheral only advertises, the central only scans —
     *  so the two phones never both connect out at once (the reconnect collision,
     *  issue #17 Step 3). With no cached peer (first pairing) we do the full symmetric
     *  dance (advertise + scan + connect + in-band tie-break). */
    private fun startDiscovery() {
        if (peerId.isNotEmpty()) {
            if (nativeDecideRole(deviceId, peerId, fruitlessDefers) == ROLE_PERIPHERAL) {
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
        // #163: do NOT disturb a connect that is still in flight. This used to clear `connecting`
        // unconditionally, and that is how the central ended up with TWO live GATT connections to the
        // same peer: the first connectGatt was still outstanding (it took ~2 s to reach
        // onServicesDiscovered), the watchdog fired at 8 s, re-armed the scan, and the next scan
        // result walked straight through the `connecting` gate into a second connectGatt. Both
        // completed, both subscribed to notifications, and every frame the peripheral notified was
        // then delivered to the app twice for the life of the link.
        //
        // The in-flight case already has an owner — the 6 s connect watchdog in connectAsCentral,
        // which tears the attempt down and rescans if it never links. This watchdog exists for a
        // different failure (#79: a cached role leaves us one-sided and nothing is being attempted at
        // all), so it has no business pre-empting an attempt that is still running.
        if (gattClient != null) {
            Log.i(TAG, "discovery watchdog: a connect is still in flight — leaving it to its own " +
                       "watchdog (#163)")
            armDiscoveryWatchdog()
            return
        }
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
        fruitlessDefers = 0   // #146: a defer that produced a link was not fruitless
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
            // #83: only the BOUND peer's departure ends the match. `device == connectedCentral` already
            // said that, and nativeIsBoundPeer is the same answer from the shared policy — kept so both
            // gates agree even if connectedCentral is assigned on some future path that skips binding.
            if (newState == BluetoothProfile.STATE_DISCONNECTED && device == connectedCentral &&
                nativeIsBoundPeer(device.address)) {
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
            // #83: only the bound peer's bytes reach the engine. Unfiltered, a third device's writes
            // injected straight into the lockstep stream. Pre-link traffic still passes — that IS the
            // handshake — so this cannot stop a link from forming.
            if (characteristic.uuid == DATAGRAM_UUID && nativeAcceptData(device.address))
                nativeOnReceived(value)
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
            //
            // #83: but only from the BOUND central. This used to assign unconditionally, so any third
            // device in the room could subscribe mid-match and silently take over the notify channel —
            // every outgoing frame redirected to it, the real peer went deaf, and the engine was never
            // told (onLinked no-ops on the `linked` guard). nativeAcceptSubscriber binds the first
            // subscriber and accepts only that one afterwards; the decision is shared C++ policy
            // (Lur::Transport::PeerBinding), not a per-platform copy of the rule.
            if (descriptor.uuid == CCCD_UUID && nativeAcceptSubscriber(device.address)) {
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
        // #163: never let a second client exist alongside the first. `gattClient = g` below used to
        // simply overwrite the field, which does not disconnect the old object — it ORPHANS it, still
        // connected and still subscribed, delivering into this same callback. Two subscriptions means
        // every peripheral notification is handed to the app twice, which the receiver then reads as a
        // 255-frame gap on every tick (the counterpart fix lives in LockstepPeer::OnMessage). Closing
        // first is also what the rest of this file already insists on: a leaked gatt makes the NEXT
        // connectGatt fail with status 133.
        gattClient?.let { old ->
            Log.i(TAG, "central: a client already exists — closing it before connecting again (#163)")
            try { old.disconnect(); old.close() } catch (_: SecurityException) {}
            gattClient = null
            clientDatagram = null
        }
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
            // #146: a role settled from a BAD read is a role the peer cannot mirror — and two
            // peers that both land on Peripheral deadlock with nobody central. A non-SUCCESS
            // status leaves characteristic.value stale (or null), so never decide from it:
            // treat the READ as the failure it is and retry from discovery.
            if (status != BluetoothGatt.GATT_SUCCESS || !nativeIsValidDeviceId(readPeerId)) {
                Log.i(TAG, "central: bad device-id read (status=$status, ${readPeerId.size}B) " +
                    "-> not deciding a role; retrying discovery")
                dropClient(gatt, rescan = true)
                return
            }
            rememberPeer(readPeerId)   // cache for the fast cached-role reconnect next time
            val role = nativeDecideRole(deviceId, readPeerId, fruitlessDefers)
            // Log the actual id STRINGS (they're ASCII hex) so a role-tie-break disagreement is
            // diagnosable from the log — a both-peripheral deadlock means the two sides compared
            // different bytes (#146).
            Log.i(TAG, "read peer id: mine=${String(deviceId, Charsets.US_ASCII)} " +
                "peer=${String(readPeerId, Charsets.US_ASCII)} defers=$fruitlessDefers -> " +
                if (role == ROLE_CENTRAL) "CENTRAL (keep link)" else "PERIPHERAL (defer)")
            if (role == ROLE_CENTRAL) {
                // #146: this is also where the breaker lands — past the defer threshold C++
                // returns Central even though the raw compare said Peripheral, so say so.
                if (fruitlessDefers >= MAX_FRUITLESS_DEFERS)
                    Log.i(TAG, "role tie-break BROKEN after $fruitlessDefers fruitless defers " +
                        "-> forcing CENTRAL (nobody was connecting; #146)")
                enableNotifications(gatt)   // we keep this connection as the live link
            } else {
                // We should be the peripheral: drop this connection and let the peer
                // (the canonical central) connect to our server instead.
                decidedPeripheral = true
                ++fruitlessDefers          // #146: cleared by onLinked; counts unanswered defers
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
