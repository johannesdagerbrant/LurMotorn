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
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.util.UUID

/**
 * The real Bluetooth Low Energy radio (issue #3, Android half) — shared by every game.
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
    /** A queued write finished; the engine's send queue releases the next one. */
    private external fun nativeOnSendComplete()
    // Radio events -> the C++ discovery/retry policies (BleStartRetry, BleDiscoveryTimers).
    // We report what the radio DID; C++ decides what happens next and calls the verbs below.
    // The two *StartFailed calls answer "is this failure worth a loud line" (first of a run, yes).
    private external fun nativeOnAdvertiseStartFailed(): Boolean
    private external fun nativeOnScanStartFailed(): Boolean
    private external fun nativeOnAdvertiseStarted()
    private external fun nativeOnScanStarted()
    private external fun nativeOnConnectStarted()
    private external fun nativeOnConnectResolved()
    private external fun nativeScheduleRescan()
    private external fun nativeOnRadioLinked()
    private external fun nativeOnRadioUnlinked()
    private external fun nativeOnRadioStopped()
    private external fun nativeOnServing()
    private external fun nativeOnAdvertising()
    private external fun nativeOnScanning()
    private external fun nativeOnAdvertiseStopped()
    private external fun nativeOnScanStopped()
    private external fun nativeOnAdapterOn()
    private external fun nativeOnAdapterOff()
    // The idempotence guard (#194) now lives in C++ BleRadioState, because a Kotlin bool could be
    // stranded true by an adapter power cycle and then suppress every recovery attempt forever.
    private external fun nativeShouldStartAdvertising(): Boolean
    private external fun nativeShouldStartScanning(): Boolean
    // #206: may WE initiate the connection this round? Discovery is symmetric; initiation is not.
    private external fun nativeShouldConnectOut(haveCachedPeer: Boolean, tieBreakSaysCentral: Boolean): Boolean
    /** #206: bytes this ATT read may return. Negative = INVALID_OFFSET. See GattLongRead.h. */
    private external fun nativeGattReadLength(valueSize: Int, offset: Int, attMtu: Int): Int
    /** The link is gone: the engine drops the send backlog, which is only meaningful to a peer
     *  that received the earlier datagrams. */
    private external fun nativeOnLinkLost()
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

    // Send flow control lives in C++ now (Lur::Transport::BleSendQueue), where host tests can
    // reach it. Android allows only ONE outstanding GATT operation and silently drops a second
    // write issued before the first completes (#72), so sends must be serialized — but WHICH
    // datagram goes next, when to give up waiting for a completion, and what to do with the
    // backlog on link loss are all DECISIONS, and decisions do not belong in a platform file.
    //
    // Keeping them here is what let this queue drift between two games, and it hid a real bug:
    // the watchdog token below guarded the TIMER but not the CALLBACK, so a completion arriving
    // after the watchdog gave up pumped a second datagram into a radio that already had one.
    //
    // What is left here is the radio's own fact — whether a write is outstanding — which the C++
    // side is told about via nativeOnSendComplete().
    private val sendLock = Any()

    // Discovery state (#194). Without this, start/stop are not idempotent and every caller
    // (startDiscovery, the #79 watchdog, the post-connect "ensure findable" path) can
    // double-start - which the stack rejects with ALREADY_STARTED, and which churns the
    // scan/advertise registration hard enough to hit Android's scan-frequency quota and the
    // advertiser-slot limit. Those DO wedge the radio for real.
    private var advertising = false
    private var scanning = false
    // Start-failure backoff, discovery/connect/rescan deadlines and their log rate-limiting all
    // moved to C++ (BleStartRetry, BleDiscoveryTimers) in #197 — they are decisions, and living
    // here is why they drifted between the two games.
    // #203: is our GATT service actually published? Confirmed by onServiceAdded, not assumed from
    // addService() returning. Nothing tracked this before, yet the startup log asserted "serving".
    private var serving = false
    private var adapterReceiver: BroadcastReceiver? = null
    // #206: the ATT MTU of the INCOMING connection (a central reading from our GATT server).
    // BluetoothGattServerCallback.onMtuChanged was never implemented, so the server had no idea how
    // big a response could be and always claimed to have sent the whole value. 23 is the ATT
    // default and the correct assumption until an exchange says otherwise; it is reset per
    // connection so a large MTU can never be carried into a fresh one.
    private var serverMtu = 23
    // #203: the last radio state we reported, so reporting is EDGE-triggered. The old line was
    // effectively level-triggered on the retry loop: it printed on every start ATTEMPT, so a radio
    // that was failing to scan announced itself healthy six times in twenty seconds while the real
    // `scan failed: 2` was deliberately quieted to once. Symmetry matters more than volume here —
    // if the failure is quiet, the recovery must be quiet too, and both must be truthful.
    private var lastRadioState = ""

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
    fun writeRaw(bytes: ByteArray): Boolean {
        synchronized(sendLock) {
            return try {
                val client = gattClient
                val clientCh = clientDatagram
                val central = connectedCentral
                val serverCh = serverDatagram
                if (client != null && clientCh != null) {           // we are central -> write
                    clientCh.value = bytes
                    // Write WITHOUT response (issue #49): drop the ATT ack round-trip per
                    // datagram. The engine's queue still paces us to one outstanding write.
                    clientCh.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                    client.writeCharacteristic(clientCh)            // true if accepted for tx
                } else if (central != null && serverCh != null) {   // we are peripheral -> notify
                    serverCh.value = bytes
                    gattServer?.notifyCharacteristicChanged(central, serverCh, false) ?: false
                } else {
                    false                                           // no link: the caller keeps it
                }
            } catch (e: SecurityException) {
                Log.e(TAG, "send: missing BLE permission", e); false
            }
        }
    }

    /** A send completed (write ack'd locally / notification handed to the stack). Straight to
     *  C++: it owns the queue, so it decides what — if anything — goes next. */
    private fun onSendComplete() = nativeOnSendComplete()

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
        nativeOnAdapterOn()   // the adapter is up (we just checked isEnabled)
        registerAdapterStateReceiver()
        Log.i(TAG, "device id: ${deviceId.size}B, cached peer: ${peerId.size}B")
        startGattServer()
        startDiscovery()
    }

    /**
     * Watch the adapter itself. NOTHING DID, BEFORE — `isEnabled` was checked once at startup and
     * never again, so a Bluetooth off/on under a running app was completely invisible to us.
     *
     * Verified on the Galaxy (2026-08-15): `svc bluetooth disable` then `enable` and the app never
     * advertised or scanned again for the life of the process, while a fresh launch seconds later
     * came up fine. The adapter takes the advertise/scan registrations and the GATT service with
     * it WITHOUT any callback we were listening to, so our started-state stayed true and #194's
     * idempotence guard suppressed every recovery. The 8 s discovery watchdog fired forever into
     * functions that returned immediately.
     *
     * The state clearing is BleRadioState's (tested, shared with iOS, which has the same hole via
     * CBManagerState). This receiver only reports the fact.
     */
    private fun registerAdapterStateReceiver() {
        if (adapterReceiver != null) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
                when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                    // TURNING_OFF, not OFF: the registrations are already going. Acting at OFF
                    // leaves a window where we still believe we are advertising.
                    BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                        advertising = false
                        scanning = false
                        serving = false
                        connecting = false
                        nativeOnAdapterOff()
                        if (linked) onLinkLost()
                        reportRadioState("adapter off")
                    }
                    BluetoothAdapter.STATE_ON -> {
                        nativeOnAdapterOn()
                        // Republish the service and resume discovery: both died with the adapter.
                        // startGattServer() re-confirms `serving`; the discovery watchdog, already
                        // re-armed by nativeOnAdapterOn, keeps trying if this attempt is early.
                        startGattServer()
                        startDiscovery()
                        reportRadioState("adapter on")
                    }
                }
            }
        }
        adapterReceiver = r
        context.registerReceiver(r, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
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
                startAdvertisingNow()
            } else {
                decidedPeripheral = false
                Log.i(TAG, "cached role: CENTRAL — scan + connect, no advertise")
                startScanningNow()
            }
        } else {
            Log.i(TAG, "no cached peer — full discovery (advertise + scan)")
            startAdvertisingNow()
            startScanningNow()
        }
        nativeOnRadioUnlinked()  // #79: arms the discovery watchdog in C++
    }

    /**
     * The discovery watchdog fired in C++ (BleDiscoveryTimers): no link in 8 s, so drop the
     * cached-role gates and resume the symmetric advertise+scan dance (#79).
     *
     * A VERB, not a decision — the deciding happened in tested C++. Posted to the main looper
     * because C++ calls it from the engine thread during Pump(), and every other mutation of the
     * radio flags happens on the looper. Keeping them all on one thread is deliberate: the last
     * regression in this subsystem was a data race that presented purely as latency.
     *
     * #194: idempotent. Before, this pair fired every 8 s against an already-running
     * advertise+scan and earned ALREADY_STARTED on both, every time — the churn behind the
     * wedged radio. It starts only what is actually stopped.
     */
    // NOTE the explicit Unit bodies on all four verbs. `fun f() = handler.post { }` would infer
    // Boolean (Handler.post returns one), the JNI signature would be ()Z, and the C++
    // GetMethodID(…, "()V") would quietly return null — a verb that never fires, with nothing
    // logged. RegisterNatives catches a Kotlin->C++ signature mismatch; this direction has no such
    // guard, so it has to be right by construction.
    fun goSymmetric() {
        handler.post {
            if (linked) return@post
            // #206: DISCOVERY goes symmetric, INITIATION does not. Dropping both gates put this
            // phone and its peer into connect-out mode on the same 8 s cadence — and two BLE
            // devices share ONE LE link, so when the elected peripheral defers and disconnects it
            // tears down the peer's in-flight incoming attempt too. Measured: three wasted rounds
            // per recovery, resolving only when #146's breaker fired.
            //
            // So both sides still advertise + scan (that is what finds a peer at all), but only the
            // side DecideBleRole elects central actually connects out. After
            // SymmetricRoundsBeforeDistrust fruitless rounds the cached id stops being trusted and
            // anyone may initiate — that is #79's guarantee, kept as an escalation.
            val haveCached = peerId.isNotEmpty()
            val tieBreakCentral =
                haveCached && nativeDecideRole(deviceId, peerId, fruitlessDefers) != ROLE_PERIPHERAL
            decidedPeripheral = !nativeShouldConnectOut(haveCached, tieBreakCentral)
            connecting = false
            startAdvertisingNow()
            startScanningNow()
        }
    }

    /** Tear down a stalled outgoing central attempt — the connect watchdog fired in C++. */
    fun abortConnect() {
        handler.post {
            val g = gattClient
            if (!linked && !decidedPeripheral && g != null) {
                Log.i(TAG, "central: connect watchdog -> tearing down and retrying")
                dropClient(g, rescan = true)
            }
        }
    }

    /** Radio verbs the C++ retry policy drives. Posted to the looper — see goSymmetric. */
    fun startAdvertising() { handler.post { if (started && !linked) startAdvertisingNow() } }
    fun startScanning()    { handler.post { if (started && !linked) startScanningNow() } }

    private fun startAdvertisingNow() {
        // #194 idempotence, but asked of BleRadioState rather than a bool we own: an adapter power
        // cycle strands a local flag true and then suppresses every recovery forever (verified on
        // the Galaxy, 2026-08-15 — the phone stayed invisible until the process was restarted).
        if (!nativeShouldStartAdvertising()) return
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
            nativeOnAdvertising()        // ...and the guard's copy, set at the same moment
        } catch (e: SecurityException) {
            Log.e(TAG, "startAdvertising: missing BLE permission", e)
        }
    }

    private fun startScanningNow() {
        if (!nativeShouldStartScanning()) return   // #194 idempotence — see startAdvertisingNow
        val sc = adapter?.bluetoothLeScanner ?: run { Log.e(TAG, "no BLE scanner"); return }
        scanner = sc
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            sc.startScan(listOf(filter), settings, scanCallback)
            scanning = true              // cleared again by onScanFailed
            nativeOnScanning()
        } catch (e: SecurityException) {
            Log.e(TAG, "startScan: missing BLE permission", e)
        }
    }

    // #203: report what the radio IS, never what we hoped a call would do. Reported only where the
    // state is actually KNOWN (a confirming callback, a failure, a teardown) and only when it CHANGES.
    //
    // The line this replaced — "BLE up: serving + advertising + scanning for LurMotorn peers" — was
    // emitted from inside startScanning() whenever startScan() merely failed to THROW, and asserted
    // all three states while having checked none of them: startScan reports failure asynchronously via
    // onScanFailed, startAdvertising can bail out before it ever starts, and nothing tracked the GATT
    // service at all. On 2026-08-11 it printed six times with the adapter switched OFF, which sent a
    // live investigation down the wrong path — the same defect class as the escalation that narrated
    // three radio restarts against a transport with no restart. An unverified success claim is worse
    // than no line, because it actively redirects the reader.
    //
    // `BLE up` is kept as the grep token (CLAUDE.md's log vocabulary: "radio started"), which is the
    // one thing it always did say truthfully. The flags now qualify it, so a partial start reads as
    // partial instead of as health.
    private fun reportRadioState(cause: String) {
        val state = "serving=${if (serving) 1 else 0} advertising=${if (advertising) 1 else 0} " +
            "scanning=${if (scanning) 1 else 0}"
        if (state == lastRadioState) return
        lastRadioState = state
        Log.i(TAG, "BLE up: $state ($cause)")
    }

    private fun stopAdvertising() {
        advertising = false
        nativeOnAdvertiseStopped()
        try { advertiser?.stopAdvertising(advertiseCallback) } catch (_: SecurityException) {}
    }

    private fun stopScanning() {
        scanning = false
        nativeOnScanStopped()
        try { scanner?.stopScan(scanCallback) } catch (_: SecurityException) {}
    }

    // #194: a failed start used to be permanent - one Log.e and the process stayed invisible/deaf
    // for the rest of its life. The retry that fixes it is now Lur::Transport::BleStartRetry, in
    // C++ and under host tests, because THAT is the reason this fix existed in chess and not in
    // RPS: living in Kotlin, nothing could test it and nothing forced the two games to agree.
    //
    // What stays here is the part that is genuinely a platform verb: releasing our own stale
    // registration before a retry. Without it the retry earns the same ALREADY_STARTED forever,
    // which was #194's actual bug — so the policy deliberately does not own it.

    private fun onAdvertiseStartFailed(errorCode: Int) {
        advertising = false
        // ALREADY_STARTED means SOMETHING holds the registration - either our own double
        // start (now prevented) or one a previous process left that the system has not reaped
        // yet. Clearing our side first is what lets the retry actually succeed.
        if (errorCode == AdvertiseCallback.ADVERTISE_FAILED_ALREADY_STARTED) {
            try { advertiser?.stopAdvertising(advertiseCallback) } catch (_: SecurityException) {}
        }
        if (nativeOnAdvertiseStartFailed()) {
            Log.e(TAG, "advertise failed: $errorCode - retrying (repeats quiet until one succeeds)")
        }
        reportRadioState("advertise failed $errorCode")   // #203: say what we now ARE, not just why
    }

    private fun onScanStartFailed(errorCode: Int) {
        scanning = false
        if (errorCode == ScanCallback.SCAN_FAILED_ALREADY_STARTED) {
            try { scanner?.stopScan(scanCallback) } catch (_: SecurityException) {}
        }
        if (nativeOnScanStartFailed()) {
            Log.e(TAG, "scan failed: $errorCode - retrying (repeats quiet until one succeeds)")
        }
        reportRadioState("scan failed $errorCode")        // #203: say what we now ARE, not just why
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
                serving = false          // #203: republished (and re-confirmed) by startGattServer below
                connectedCentral = null
                stopScanning()
                stopAdvertising()
                // Link state resets exactly as a real loss would: engine goes to Searching, role reopens.
                linked = false
                decidedPeripheral = false
                connecting = false
                nativeOnLinkLost()   // C++ owns the queue: it drops the backlog (stale state)
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
            nativeOnRadioStopped()  // every deadline off, both backoffs abandoned
            adapterReceiver?.let { try { context.unregisterReceiver(it) } catch (_: IllegalArgumentException) {} }
            adapterReceiver = null
            stopScanning()
            stopAdvertising()
            try { gattClient?.close() } catch (_: SecurityException) {}
            gattClient = null
            try { gattServer?.close() } catch (_: SecurityException) {}
            gattServer = null
            serving = false
            lastRadioState = ""          // #203: next start reports afresh, not deduped against a dead run
            Log.i(TAG, "BLE stopped - advertiser/scanner/GATT released")
        }
    }

    /** The canonical link is up — stop discovery so the radio settles. */
    private fun onLinked(asPeripheral: Boolean) {
        if (linked) return
        linked = true
        fruitlessDefers = 0   // #146: a defer that produced a link was not fruitless
        nativeOnRadioLinked()   // #79: link up — stop every deadline and cancel the backoffs
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
        nativeOnLinkLost()   // drop stale send state (#72) — the queue lives in C++
        nativeOnDisconnected()
        startDiscovery()   // role-aware: cached peer -> one-sided, no reconnect collision
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {
            advertising = true
            nativeOnAdvertiseStarted()   // clears the backoff; a failure later starts from the top
            reportRadioState("advertise confirmed")   // #203: the callback IS the confirmation
        }
        override fun onStartFailure(errorCode: Int) { onAdvertiseStartFailed(errorCode) }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            nativeOnScanStarted()        // the scanner is demonstrably alive: clear the backoff
            // #203: Android has no onScanSuccess, so a delivered result is the ONLY positive proof
            // that scanning works. If a previous failure had cleared the flag, this is the recovery —
            // and it must be reported, or the log keeps the last thing it said, which was a failure.
            if (!scanning) { scanning = true; reportRadioState("scan live (result delivered)") }
            if (linked || connecting || decidedPeripheral) return
            connecting = true
            Log.i(TAG, "scan: found a LurMotorn peer, connecting as central")
            connectAsCentral(result.device)
        }

        override fun onScanFailed(errorCode: Int) { onScanStartFailed(errorCode) }
    }

    // --- GATT server (every device runs one; the peripheral's is the live link) ---

    /**
     * #206: WHO is connected to our GATT server, right now, from the STACK's point of view.
     *
     * `onMtuChanged` fires twice per incoming iPhone attempt, milliseconds apart on two threads,
     * which reads as the same phone holding TWO concurrent connections — an earlier attempt that
     * was never fully torn down alongside the new one. If the read request and our response end up
     * on different connections that produces exactly the observed signature: the server logs a
     * clean, in-MTU, complete response and the central never receives it.
     *
     * `connectedCentral` is OUR bookkeeping and would not show that; this asks the stack. The
     * address is the identity that matters — two entries with the SAME address is the smoking gun,
     * two different addresses is an unrelated third device (#83 territory).
     */
    private fun connectedCentralsDesc(): String {
        val mgr = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val devices = try {
            mgr?.getConnectedDevices(BluetoothProfile.GATT_SERVER) ?: emptyList()
        } catch (_: SecurityException) { emptyList() }
        val addrs = devices.joinToString(",") { it.address }
        return "n=${devices.size}[$addrs] ours=${connectedCentral?.address ?: "none"}"
    }

    private fun startGattServer() {
        val mgr = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        // #206: CLOSE ANY EXISTING SERVER FIRST. openGattServer registers another instance against
        // the same callback object; overwriting the field leaks the old one, still open and still
        // delivering. Two live servers means every server callback arrives TWICE (visible as
        // identical lines on two threads), the service is published twice, and — the part that
        // actually breaks the link — `gattServer?.sendResponse(...)` only ever answers on the
        // NEWEST instance. A read that arrived on the older one is answered into the void: this
        // side logs a clean, complete, in-MTU response and the central never receives it, which is
        // exactly the `stalled at CharsFound` signature.
        //
        // restartRadio (#182) already closed before rebuilding; the adapter-ON path added in
        // 65595ff did not, so a Bluetooth power cycle left two servers behind. Putting the close
        // HERE makes it impossible for a fourth caller to forget.
        try { gattServer?.close() } catch (_: SecurityException) {}
        gattServer = null
        serverDatagram = null
        serving = false
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
        // #203: addService() is ASYNCHRONOUS — it returning true means "queued", not "published".
        // Nothing observed this before, so "serving" was pure assumption; a service that failed to
        // publish left us invisible to a central with no line anywhere saying so.
        override fun onServiceAdded(status: Int, service: BluetoothGattService) {
            if (service.uuid != SERVICE_UUID) return
            serving = status == BluetoothGatt.GATT_SUCCESS
            if (serving) nativeOnServing()
            if (!serving) {
                Log.e(TAG, "addService FAILED: status=$status - this device cannot be connected to " +
                    "as a peripheral (a central will never find our service)")
            }
            reportRadioState(if (serving) "service published" else "service add failed $status")
        }

        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            // #206: a fresh connection starts at the ATT default until its own exchange says
            // otherwise. Carrying a previous central's negotiated MTU into a new connection is the
            // dangerous direction — it would let us over-fill a response again.
            if (newState == BluetoothProfile.STATE_CONNECTED) serverMtu = 23
            // #206: every arrival and departure at our server, by address. A stale connection that
            // is never torn down is invisible in our own bookkeeping but shows up here.
            val what = when (newState) {
                BluetoothProfile.STATE_CONNECTED -> "CONNECTED"
                BluetoothProfile.STATE_DISCONNECTED -> "DISCONNECTED"
                else -> "state=$newState"
            }
            Log.i(TAG, "server: central $what ${device.address} status=$status " +
                "centrals: ${connectedCentralsDesc()}")
            // #83: only the BOUND peer's departure ends the match. `device == connectedCentral` already
            // said that, and nativeIsBoundPeer is the same answer from the shared policy — kept so both
            // gates agree even if connectedCentral is assigned on some future path that skips binding.
            if (newState == BluetoothProfile.STATE_DISCONNECTED && device == connectedCentral &&
                nativeIsBoundPeer(device.address)) {
                onLinkLost()
            }
        }

        /**
         * #206: THE SERVER'S MTU, which nothing recorded before.
         *
         * BluetoothGattServerCallback has had this callback all along and we never implemented it,
         * so `onCharacteristicReadRequest` had no idea how large a response could be and always
         * returned the whole remaining value. For the 32-byte device id on a default-MTU connection
         * that is a response the link cannot carry, and the central's read never completes — which
         * is what stalled every incoming iPhone connect at the device-id read.
         */
        override fun onMtuChanged(device: BluetoothDevice, mtu: Int) {
            serverMtu = mtu
            // #206: the ADDRESS is the point. This line arrives twice per incoming attempt; whether
            // the two carry the same address decides between "one phone connected twice" and "two
            // different devices".
            Log.i(TAG, "server: mtu=$mtu from=${device.address} (reads chunk at ${mtu - 1}B) " +
                "centrals: ${connectedCentralsDesc()}")
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int, characteristic: BluetoothGattCharacteristic,
        ) {
            // Hand the connecting central our device id so it can run the role tie-break.
            val full = if (characteristic.uuid == DEVICE_ID_UUID) deviceId else ByteArray(0)
            // Honour BOTH halves of a long read — the offset AND the length. The 32-byte id does
            // not fit a default-MTU response (22 bytes), so a central that has not negotiated a
            // larger MTU must issue a Read Blob for the tail.
            //
            // Getting either half wrong has broken the link: #17 ignored the offset and corrupted
            // reassembly; #206 ignored the MTU and returned every remaining byte, so the central
            // never learned there WAS a tail, never blob-read it, and the read never completed at
            // all — no value, no error. On the pair that showed as every outgoing iPhone attempt
            // stalling at the device-id read while this line logged a confident `offset=0 -> 32B`.
            //
            // The arithmetic is Lur::Transport::GattReadLength, in C++ with tests, because five
            // lines that cost two link outages are a decision, not ceremony.
            val len = nativeGattReadLength(full.size, offset, serverMtu)
            if (len < 0) {
                Log.e(TAG, "read at offset=$offset past end of ${full.size}B value -> INVALID_OFFSET")
                try {
                    gattServer?.sendResponse(device, requestId,
                        BluetoothGatt.GATT_INVALID_OFFSET, offset, ByteArray(0))
                } catch (_: SecurityException) {}
                return
            }
            val value = full.copyOfRange(offset, offset + len)
            val more = offset + len < full.size
            // #206: WHICH central asked, and who else is attached. A response is only useful on the
            // connection the request came in on, and the server has been reporting success while the
            // central saw nothing.
            Log.i(TAG, "serve device id: from=${device.address} offset=$offset -> ${value.size}B " +
                "(mtu=$serverMtu of ${full.size}B${if (more) ", tail follows" else ", complete"}) " +
                "centrals: ${connectedCentralsDesc()}")
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
            // Android can silently never call back (a hung connect). The watchdog that catches it
            // is BleDiscoveryTimers::ConnectWatchdogNs in C++; it calls abortConnect() when the
            // attempt has neither linked nor resolved. Deliberately shorter than the discovery
            // watchdog, so a stalled attempt is cleaned up before the symmetric reset lands on it.
            nativeOnConnectStarted()
        } catch (e: SecurityException) {
            Log.e(TAG, "connectGatt: missing BLE permission", e)
            connecting = false
            nativeOnConnectResolved()
            scheduleRescan()
        }
    }

    /** Resume scanning after a short delay. A collided/failed connect must NOT be
     *  retried immediately: the peer (doing its own exploratory connect) needs a
     *  moment to settle into peripheral-only, and the shared LE link needs to finish
     *  tearing down, or the retry collides again / hangs (issue #17 reconnect).
     *
     *  The delay itself is BleDiscoveryTimers::RescanDelayNs — re-requesting supersedes a pending
     *  one rather than queueing a second, so a burst of failures cannot produce a burst of scan
     *  starts (the ALREADY_STARTED shape from #194). The old Handler.postDelayed could. */
    private fun scheduleRescan() = nativeScheduleRescan()

    /** Tear down a client connection. Always closes (a leaked gatt makes the NEXT
     *  connectGatt fail with 133). Resumes discovery unless we're deliberately idle. */
    private fun dropClient(gatt: BluetoothGatt, rescan: Boolean) {
        try { gatt.disconnect(); gatt.close() } catch (_: SecurityException) {}
        if (gattClient == gatt) { gattClient = null; clientDatagram = null }
        connecting = false
        // The attempt is over however it ended — linked, refused, or torn down. Disarm the connect
        // watchdog so a link that just formed is not ripped out moments later.
        nativeOnConnectResolved()
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
                nativeOnRadioUnlinked()  // #79: re-arm from zero — if the peer never comes, go symmetric
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
