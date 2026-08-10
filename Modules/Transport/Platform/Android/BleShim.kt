// The Android BLE radio driver — ONE copy, shared by every game.
//
// It used to be copy-pasted per app, because the JNI convention bakes the app's package into the
// exported symbol names (Java_com_lurmotorn_<app>_BleShim_*) and the class therefore had to live in
// the app's own package. The C++ side binds by RegisterNatives now, so this class sits in one fixed
// engine package and each app's Gradle simply adds this directory to its Kotlin source set.
//
// Three things are genuinely per-app and are therefore INJECTED rather than declared here:
//   * the log tag and the native library name — passed to the constructor by the app's activity;
//   * the BLE service UUID — read across the JNI seam from Lur::Transport::BleProtocol, which is
//     its single source of truth.
//
// What belongs in this file: platform API verbs and event forwarding. What does not: decisions. The
// send queue, the start-retry backoff and the discovery deadlines are engine C++ with host tests
// (BleSendQueue / BleStartRetry / BleDiscoveryTimers) precisely because they are decisions — and
// because keeping them here is what let them drift 650 lines between two games.
package com.lurmotorn.engine

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
 * The real Bluetooth Low Energy radio for OnlyChess (issue #3, Android half).
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
class BleShim(private val context: Context, private val TAG: String) {

    companion object {

        // NOT declared here. These are read across the JNI seam from Lur::Transport::BleProtocol,
        // their single source of truth — see the nativeXxxUuid externals below. They used to be
        // duplicated here under a comment reading "MUST match BleProtocol.h", which is a duplication
        // maintained by hope: nothing checked it, and a mismatch fails silently as two phones that
        // never see each other, the hardest BLE symptom to attribute.
        // Standard Client Characteristic Configuration Descriptor (enables notify).
        private val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
        // Mirrors Lur::Transport::BleMaxPeripheralDefers. LOG-ONLY: C++ owns the actual
        // breaker decision (nativeDecideRole), this just labels the line that reports it.
        private const val MAX_FRUITLESS_DEFERS = 2

        private const val ROLE_PERIPHERAL = 0
        private const val ROLE_CENTRAL = 1

        // The native library is loaded by the APP, not here: its name is per-app
        // (android.app.lib_name), and NativeActivity has already loaded it by the time any of this
        // runs. A hardcoded loadLibrary here was one of the two things that pinned this file to one
        // game. Kept as a helper so an app without NativeActivity can still ensure it.
        @JvmStatic
        fun ensureNativeLibrary(name: String) = System.loadLibrary(name)
    }

    // --- JNI: into C++ (defined in AndroidBleTransport.cpp) ---
    /** #146: is a device id read off the peer well-formed (32 lowercase hex)? A failed or truncated
     *  GATT read yields bytes that are not an id, and a role decided from those is one the peer
     *  cannot mirror — the deadlock's mechanism. */
    private external fun nativeIsValidDeviceId(id: ByteArray): Boolean

    // The BLE wire identity, from its single source of truth in C++ (BleProtocol.h).
    private external fun nativeServiceUuid(): String
    private external fun nativeDatagramUuid(): String
    private external fun nativeDeviceIdUuid(): String

    // Resolved once per instance. `by lazy` rather than an init-time read so construction order
    // never matters: JNI_OnLoad has bound these before any instance can exist, but a lazy val also
    // survives someone constructing this earlier than expected.
    private val SERVICE_UUID: UUID by lazy { UUID.fromString(nativeServiceUuid()) }
    private val DATAGRAM_UUID: UUID by lazy { UUID.fromString(nativeDatagramUuid()) }
    private val DEVICE_ID_UUID: UUID by lazy { UUID.fromString(nativeDeviceIdUuid()) }

    private external fun nativeSetShim()
    private external fun nativeOnConnected(asPeripheral: Boolean)
    private external fun nativeOnDisconnected()
    private external fun nativeOnReceived(bytes: ByteArray)
    /** The shared role tie-break. [fruitlessDefers] is how many times we have already deferred to
     *  a peer that then never connected: past the threshold the C++ side BREAKS the tie and hands
     *  us Central, escaping the both-peripheral deadlock (#146). */
    private external fun nativeDecideRole(localId: ByteArray, peerId: ByteArray, fruitlessDefers: Int): Int
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

    // Discovery state (#194). Without this, start/stop are not idempotent and every caller
    // (startDiscovery, the #79 watchdog, the post-connect "ensure findable" path) can
    // double-start - which the stack rejects with ALREADY_STARTED, and which churns the
    // scan/advertise registration hard enough to hit Android's scan-frequency quota and the
    // advertiser-slot limit. Those DO wedge the radio for real.
    private var advertising = false
    private var scanning = false
    private var advRetries = 0
    private var scanRetries = 0
    private var advFailLogged = false      // rate-limit: the retry loop must not bury the log
    private var scanFailLogged = false

    @Volatile private var started = false
    @Volatile private var linked = false
    @Volatile private var connecting = false   // an outgoing central attempt is mid-flight
    @Volatile private var decidedPeripheral = false  // we settled as peripheral; stop connecting out

    // #146: consecutive defers that produced NO link. DecideBleRole is a total order, so two
    // healthy peers always get opposite answers — but on hardware both once settled on Peripheral,
    // so nobody connected and the link never formed. Retrying cannot escape that: it re-runs the
    // same comparison and reaches the same answer. Past MAX_FRUITLESS_DEFERS the shared breaker
    // returns Central regardless, which is the only way out. Cleared by onLinked — a defer that
    // produced a link was not fruitless.
    private var fruitlessDefers = 0

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

    /**
     * Send one datagram to the peer. Enqueued + serialized via flow control (issue #72).
     *
     * An EXPEDITED datagram jumps the queue (issue #190). The queue is otherwise FIFO with no
     * notion of urgency, so the datagram a player is waiting on could sit behind a keepalive or
     * — much worse — behind a multi-datagram resync payload, which is exactly when the queue is
     * deepest and exactly when latency is felt. Each wait is a whole connection interval.
     *
     * Urgency is an ARGUMENT, decided by the engine. It used to be inferred here from the
     * array's LENGTH ("1 byte means a live move"), which put one game's wire format inside this
     * radio shim — and it broke silently the moment that format changed: the move became a
     * framed 2-byte datagram, `size == 1` stopped matching, and this fast path simply stopped
     * happening. Nothing failed; it just got slower, in the one place latency is felt.
     *
     * Reordering is safe for the caller that asks for it: expedited datagrams keep their order
     * among themselves, so one can never overtake another.
     */
    @Suppress("unused")
    fun send(bytes: ByteArray, expedited: Boolean) {
        synchronized(sendLock) {
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

    // --- Called from OnlyChessActivity once permissions are granted ---

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
        Log.i(TAG, "discovery watchdog: no link in 8s — dropping cached-role gates, going symmetric (#79)")
        decidedPeripheral = false
        connecting = false
        // #194: idempotent now. Before, this pair fired every 8 s against an already-running
        // advertise+scan and earned ALREADY_STARTED on both, every time - the churn behind
        // the wedged radio. It now starts only what is actually stopped, which also makes it
        // the long-cadence retry for a start that keeps failing.
        startAdvertising()
        startScanning()
        armDiscoveryWatchdog()  // keep watching until a link forms
    }

    private fun startAdvertising() {
        if (advertising) return          // #194: idempotent - asking twice earns ALREADY_STARTED
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
            advertising = true           // cleared again by onStartFailure
        } catch (e: SecurityException) {
            Log.e(TAG, "startAdvertising: missing BLE permission", e)
        }
    }

    private fun startScanning() {
        if (scanning) return             // #194: idempotent - see startAdvertising
        val sc = adapter?.bluetoothLeScanner ?: run { Log.e(TAG, "no BLE scanner"); return }
        scanner = sc
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            sc.startScan(listOf(filter), settings, scanCallback)
            scanning = true              // cleared again by onScanFailed
            Log.i(TAG, "BLE up: serving + advertising + scanning for LurMotorn peers")
        } catch (e: SecurityException) {
            Log.e(TAG, "startScan: missing BLE permission", e)
        }
    }

    private fun stopAdvertising() {
        advertising = false
        advRetries = 0
        advFailLogged = false
        handler.removeCallbacks(advRetry)
        try { advertiser?.stopAdvertising(advertiseCallback) } catch (_: SecurityException) {}
    }

    private fun stopScanning() {
        scanning = false
        scanRetries = 0
        scanFailLogged = false
        handler.removeCallbacks(scanRetry)
        try { scanner?.stopScan(scanCallback) } catch (_: SecurityException) {}
    }

    // #194: a failed start used to be permanent - one Log.e and the process stayed
    // invisible/deaf for the rest of its life. Retry on a capped backoff while we are still
    // trying to link, so a transient failure heals in about a second instead of needing a
    // human to toggle Bluetooth. The fast retries are capped and then handed back to the 8 s
    // discovery watchdog (#79), which keeps trying for as long as we are unlinked - so there
    // is no give-up state, just a slower cadence.
    private val advRetry = Runnable { if (started && !linked) startAdvertising() }
    private val scanRetry = Runnable { if (started && !linked) startScanning() }

    private fun retryDelayMs(attempt: Int): Long = 400L shl minOf(attempt, 3)   // 0.4 - 3.2 s

    private fun onAdvertiseStartFailed(errorCode: Int) {
        advertising = false
        // ALREADY_STARTED means SOMETHING holds the registration - either our own double
        // start (now prevented) or one a previous process left that the system has not reaped
        // yet. Clearing our side first is what lets the retry actually succeed.
        if (errorCode == AdvertiseCallback.ADVERTISE_FAILED_ALREADY_STARTED) {
            try { advertiser?.stopAdvertising(advertiseCallback) } catch (_: SecurityException) {}
        }
        if (!advFailLogged) {
            advFailLogged = true
            Log.e(TAG, "advertise failed: $errorCode - retrying (repeats quiet until one succeeds)")
        }
        if (advRetries < 5 && started && !linked) {
            handler.postDelayed(advRetry, retryDelayMs(advRetries++))
        }
    }

    private fun onScanStartFailed(errorCode: Int) {
        scanning = false
        if (errorCode == ScanCallback.SCAN_FAILED_ALREADY_STARTED) {
            try { scanner?.stopScan(scanCallback) } catch (_: SecurityException) {}
        }
        if (!scanFailLogged) {
            scanFailLogged = true
            Log.e(TAG, "scan failed: $errorCode - retrying (repeats quiet until one succeeds)")
        }
        if (scanRetries < 5 && started && !linked) {
            handler.postDelayed(scanRetry, retryDelayMs(scanRetries++))
        }
    }

    /**
     * #182: a full radio teardown and rebuild.
     *
     * The engine escalates here once it has concluded the link is HALF-OPEN — connected, our writes
     * leaving, and nothing ever arriving. A soft resetLink provably cannot clear that: on hardware
     * 80 soft resets in a row cleared nothing and only a reboot did. This game had no implementation
     * at all, so the escalation logged three attempts that never happened, which sent a real
     * diagnosis down the wrong path.
     *
     * Bounded by the caller (MaxRadioRestarts), because the #163 lesson is that churn degrades the
     * radio — the recovery must not become the fault.
     */
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
                // Server (peripheral side): a wedged notify path we SERVE lives here, and resetLink
                // never touches it — closing + reopening re-publishes the service and drops any stale
                // subscription. startGattServer() below rebuilds it.
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

    /** Release the radio (#194). A registration left behind is what the NEXT launch collides
     *  with, so a clean exit has to hand it back. A force-stop cannot run this - but a
     *  force-stop is not what we do to players. */
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
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {
            advertising = true
            advRetries = 0
            advFailLogged = false        // a later failure is news again
        }
        override fun onStartFailure(errorCode: Int) { onAdvertiseStartFailed(errorCode) }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            scanRetries = 0
            scanFailLogged = false       // the scanner is demonstrably alive
            if (linked || connecting || decidedPeripheral) return
            connecting = true
            Log.i(TAG, "scan: found a LurMotorn peer, connecting as central")
            connectAsCentral(result.device)
        }

        override fun onScanFailed(errorCode: Int) { onScanStartFailed(errorCode) }
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
            // injected straight into the move stream. Pre-link traffic still passes — that IS the
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
            // #146: a FAILED read leaves characteristic.value stale or null, so never decide from
            // it — treat the read as the failure it is and retry from discovery. Deciding a role
            // from garbage produces one the peer cannot mirror, which is how both sides end up
            // Peripheral and nobody connects.
            if (status != BluetoothGatt.GATT_SUCCESS || !nativeIsValidDeviceId(readPeerId)) {
                Log.i(TAG, "central: bad device-id read (status=$status, ${readPeerId.size}B) " +
                    "-> not deciding a role; retrying discovery")
                dropClient(gatt, rescan = true)
                return
            }
            rememberPeer(readPeerId)   // cache for the fast cached-role reconnect next time
            val role = nativeDecideRole(deviceId, readPeerId, fruitlessDefers)
            // Log the id STRINGS (they are ASCII hex), not just their sizes: a both-peripheral
            // deadlock means the two sides compared DIFFERENT bytes, and only the values show that
            // (#146). Sizes agree in exactly the case that is hardest to diagnose.
            Log.i(TAG, "read peer id: mine=${String(deviceId, Charsets.US_ASCII)} " +
                "peer=${String(readPeerId, Charsets.US_ASCII)} defers=$fruitlessDefers -> " +
                if (role == ROLE_CENTRAL) "CENTRAL (keep link)" else "PERIPHERAL (defer)")
            if (role == ROLE_CENTRAL) {
                // #146: this is also where the breaker lands — past the threshold C++ returns
                // Central even though the raw compare said Peripheral, so say so out loud.
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
