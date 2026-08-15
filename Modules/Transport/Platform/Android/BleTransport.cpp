// Bridges Lur::Transport::ITransport to the Kotlin BleShim over JNI (issue #3, Android half).
// ONE copy, shared by every game — this and BleShim.kt beside it are the whole Android radio.
//
// The Kotlin side owns the radio API (advertise/scan/GATT); this side is the platform-neutral seam
// the engine speaks to: Send() pushes a datagram to BleShim.send over JNI, and the BleShim's radio
// callbacks land in the native methods below, which feed bytes and state back to the engine.
//
// It was per-game because the JNI symbol convention bakes the app's package into the exported
// names. The natives bind by RegisterNatives against a fixed engine class now (see the table at the
// end of the file), so nothing here names an app: the log tag arrives as LUR_LOG_TAG and the BLE
// UUIDs are served to Kotlin from BleProtocol.h.
#include <jni.h>
#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#if LUR_AGENT
#include <sys/system_properties.h>   // agent role override via debug.lur.role (#196)
#endif

#include <chrono>

#include "Lur/Core/LogTag.h"
#include "Lur/Transport/BleSendQueue.h"
#include "Lur/Transport/BleStartRetry.h"
#include "Lur/Transport/BleDiscoveryTimers.h"
#include "Lur/Transport/BleRadioState.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/BleProtocol.h"
#include "Lur/Transport/EventInbox.h"

// The app's log tag, from LUR_LOG_TAG (Lur/Core/LogTag.h #errors if an app forgot to set it).
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, Lur::Core::LogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, Lur::Core::LogTag, __VA_ARGS__)

namespace Lur::Transport {
namespace {

// The engine send queue and the discovery/retry policies live further down, next to the JNI helpers
// they need. These are declared here because the transport class comes first in the file and drives
// all of them.
void TickPolicies();                                                     // advance every deadline
void EnqueueOutbound(const uint8_t* Data, std::size_t Size, bool Expedited);
void DropOutboundBacklog();                                              // link lost

// The BLE radio callbacks (below) fire on Binder threads; they Push into Inbox, and
// the engine thread drains it via Pump() (called from Session::Tick). So ReceiverFn
// and Connected are only ever touched on the engine thread — the documented contract
// (issue #40). Inbox is the one thread-crossing point.
class AndroidBleTransport : public ITransport, public EventInbox::Sink {
public:
    void Send(const uint8_t* Data, std::size_t Size) override;
    void SendExpedited(const uint8_t* Data, std::size_t Size) override;
    void RestartRadio() override;                              // #182 hard reset
    bool CanRestartRadio() const override { return true; }     // ...and we really have one

private:
    void SendWithPriority(const uint8_t* Data, std::size_t Size, bool Expedited);
public:
    void SetReceiver(Receiver NewReceiver) override { ReceiverFn = std::move(NewReceiver); }
    bool IsConnected() const override { return Connected; }
    void ResetLink() override;
    void Pump() override {
        Inbox.Drain(*this);   // engine thread: dispatch queued events
        TickPolicies();       // ...and advance every deadline on the same cadence
    }

    // EventInbox::Sink — invoked by Drain() on the engine thread, in arrival order.
    void OnConnected() override    { Connected = true; }
    void OnDisconnected() override { Connected = false; }
    void OnDatagram(const uint8_t* Data, std::size_t Size) override {
        if (ReceiverFn) ReceiverFn(Data, Size);
    }

    EventInbox Inbox;              // Binder threads Push; the engine thread Drains
    Receiver   ReceiverFn;
    bool       Connected = false;  // engine-thread only (mutated in OnConnected/OnDisconnected)
};

// One link to one peer — local multiplayer is strictly 1:1.
AndroidBleTransport g_Transport;

// JNI plumbing cached at load / shim-registration time.
JavaVM*   g_Vm         = nullptr;
jobject   g_Shim       = nullptr;  // global ref to the Kotlin BleShim
jmethodID g_WriteMethod = nullptr; // BleShim.writeRaw([B)Z — one datagram, "did it take it"
jmethodID g_RestartMethod = nullptr;  // BleShim.restartRadio()V (#182 hard reset)
jmethodID g_ResetMethod = nullptr; // BleShim.resetLink()V
// The four radio VERBS the policies below drive. Each is a bare platform action with no decision
// in it — "start advertising", not "start advertising if it seems like a good idea".
jmethodID g_StartAdvertisingMethod = nullptr;  // BleShim.startAdvertising()V
jmethodID g_StartScanningMethod    = nullptr;  // BleShim.startScanning()V
jmethodID g_GoSymmetricMethod      = nullptr;  // BleShim.goSymmetric()V   — drop cached-role gates
jmethodID g_AbortConnectMethod     = nullptr;  // BleShim.abortConnect()V  — tear a stalled attempt

// Get a JNIEnv for the calling thread, attaching it if necessary. android_main
// runs on a native thread the JVM doesn't know about, so Send() may need attach.
JNIEnv* EnvForThisThread() {
    if (g_Vm == nullptr) return nullptr;
    JNIEnv* Env = nullptr;
    const jint Rc = g_Vm->GetEnv(reinterpret_cast<void**>(&Env), JNI_VERSION_1_6);
    if (Rc == JNI_EDETACHED) {
        if (g_Vm->AttachCurrentThread(&Env, nullptr) != JNI_OK) return nullptr;
    }
    return Env;
}

void AndroidBleTransport::SendWithPriority(const uint8_t* Data, std::size_t Size, bool Expedited) {
    // Straight into the engine queue, which issues it when the radio is free. Urgency is passed,
    // never inferred: the Kotlin used to guess it from the array's LENGTH ("1 byte means a live
    // move"), which put one game's wire format inside the radio shim and broke silently when that
    // format changed.
    EnqueueOutbound(Data, Size, Expedited);
}

void AndroidBleTransport::Send(const uint8_t* Data, std::size_t Size) {
    SendWithPriority(Data, Size, false);
}

void AndroidBleTransport::SendExpedited(const uint8_t* Data, std::size_t Size) {
    SendWithPriority(Data, Size, true);
}

// The net keepalive timed out — the peer is silently gone. Force the Kotlin radio to
// drop the (dead) link and resume discovery, rather than waiting out the BLE
// supervision timeout (10-20s) for a disconnect callback. This makes a killed-peer
// drop detected in ~5s on Android too, matching iOS.
// #182: the SOFT ResetLink below provably cannot clear a wedged BLE stack — on hardware 80 soft
// resets in a row cleared nothing and only a reboot did. This is the harder escalation the session
// reaches for once it has concluded the link is half-open: tear the whole radio down and rebuild it.
// Chess had no implementation at all, so the session's escalation logged three attempts that never
// happened.
void AndroidBleTransport::RestartRadio() {
    if (g_Shim == nullptr || g_RestartMethod == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;
    Env->CallVoidMethod(g_Shim, g_RestartMethod);
}

void AndroidBleTransport::ResetLink() {
    if (g_Shim == nullptr || g_ResetMethod == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;
    Env->CallVoidMethod(g_Shim, g_ResetMethod);
}

} // namespace

ITransport* CreateBleTransport() { return &g_Transport; }

} // namespace Lur::Transport

using namespace Lur::Transport;

// ---- The dumb radio, and the engine queue that drives it ----
//
// BleShim used to own a send queue: ~60 lines of ordering, an in-flight flag, a 300 ms Handler
// watchdog and a token to invalidate stale timers. All of that is policy — which datagram goes
// next, how long to wait for a completion, what to do with the backlog when the link dies — and
// living in Kotlin is what let it drift between two games AND hide a real bug: the token guarded
// the timer but not the callback, so a completion arriving after the watchdog gave up pumped a
// second datagram into a radio that already had one, which a BLE stack answers by silently
// dropping one of them.
//
// Now Kotlin exposes one verb, writeRaw(bytes) -> "did the radio take it", and Lur::Transport::
// BleSendQueue decides the rest, under host tests.
namespace Lur::Transport {
namespace {

class AndroidRadio final : public IBleRadio {
public:
    bool Write(const uint8_t* Data, std::size_t Size) override {
        if (g_Shim == nullptr || g_WriteMethod == nullptr) return false;
        JNIEnv* Env = EnvForThisThread();
        if (Env == nullptr) return false;
        jbyteArray Arr = Env->NewByteArray(static_cast<jsize>(Size));
        Env->SetByteArrayRegion(Arr, 0, static_cast<jsize>(Size),
                                reinterpret_cast<const jbyte*>(Data));
        const jboolean Took = Env->CallBooleanMethod(g_Shim, g_WriteMethod, Arr);
        Env->DeleteLocalRef(Arr);
        return Took == JNI_TRUE;
    }
};

AndroidRadio  g_Radio;
BleSendQueue  g_SendQueue;

// The queue's watchdog is denominated in nanoseconds so it is host-testable, but ITransport::Pump()
// carries no time. Rather than change that signature for every backend, measure here: Pump() is
// called once per Session::Tick, which is exactly the cadence the deadline wants.
void EnqueueOutbound(const uint8_t* Data, std::size_t Size, bool Expedited) {
    g_SendQueue.SetRadio(&g_Radio);
    g_SendQueue.Enqueue(Data, Size,
                        Expedited ? EBleSendPriority::Expedited : EBleSendPriority::Normal);
}

void DropOutboundBacklog() { g_SendQueue.OnLinkLost(); }

// The discovery deadlines and the start-failure backoff, both host-tested, both previously
// hand-rolled in Kotlin with Handler.postDelayed and manual cancellation.
//
// This is the control INVERSION the send-queue cutover did not need: the driver no longer decides
// when to retry or when to go symmetric, it ASKS. Kotlin reports what the radio did; these decide
// what happens next; Pump() applies the answer. That is what makes the behaviour reachable from a
// host test — and it is why RPS could silently never advertise after a failed first attempt while
// chess healed (#194): the logic lived where nothing could test it, so it existed in one game only.
BleStartRetry      g_AdvRetry;
BleStartRetry      g_ScanRetry;
BleDiscoveryTimers g_Timers;
// What the radio is actually doing. Owns the idempotence guard #194 added, which used to be a
// Kotlin bool that an adapter power cycle could strand true — see BleRadioState.h for the hardware
// finding. Asking it before every start is what makes the recovery possible.
BleRadioState      g_Radio_State;

// Call one of BleShim's radio verbs. Safe from the engine thread because the Kotlin side posts the
// body onto the main looper — see the note on those methods. We only cross JNI here.
void CallShimVerb(jmethodID Method) {
    if (g_Shim == nullptr || Method == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;
    Env->CallVoidMethod(g_Shim, Method);
}

}  // namespace

// Named entry points for the JNI thunks, which live at global scope and cannot see the anonymous
// namespace above.
void OnOutboundSendComplete() { g_SendQueue.OnSendComplete(); }
void OnOutboundLinkLost()     { g_SendQueue.OnLinkLost(); }

// Radio events, reported by Kotlin. Each is a fact about what the radio DID; none of them decide
// anything. The bool returns answer "is this worth a loud log line" so a retry storm cannot bury
// the first failure, which is the one that matters.
bool OnAdvertiseStartFailed() {
    g_Radio_State.OnAdvertiseStopped();   // we are NOT advertising: let the retry through
    return g_AdvRetry.OnStartFailed();
}
bool OnScanStartFailed() {
    g_Radio_State.OnScanStopped();
    return g_ScanRetry.OnStartFailed();
}
void OnAdvertiseStarted()     { g_Radio_State.OnAdvertising(); g_AdvRetry.OnStarted(); }
void OnScanStarted()          { g_Radio_State.OnScanning();    g_ScanRetry.OnStarted(); }
void OnServing()              { g_Radio_State.OnServing(); }
// "We asked and have not been told otherwise" — the state the idempotence guard reads. Reported at
// REQUEST time, not at confirmation: the old Kotlin bool was set the same moment, and it has to be,
// or the 8 s watchdog double-starts into the gap before the success callback lands.
void OnAdvertising()          { g_Radio_State.OnAdvertising(); }
void OnScanning()             { g_Radio_State.OnScanning(); }
void OnAdvertiseStopped()     { g_Radio_State.OnAdvertiseStopped(); }
void OnScanStopped()          { g_Radio_State.OnScanStopped(); }

// May we start? The driver asks before touching the radio, instead of consulting a bool it owns.
bool ShouldStartAdvertising() { return g_Radio_State.ShouldStartAdvertising(); }
bool ShouldStartScanning()    { return g_Radio_State.ShouldStartScanning(); }

// The adapter came or went. THE POWER CYCLE IS THE POINT: everything the radio was holding —
// advertise registration, scan registration, the GATT service — went with it, so every
// started-state has to be forgotten or #194's idempotence guard suppresses the recovery forever.
// Nothing listened for this at all before (2026-08-15 hardware finding).
void OnAdapterOn() {
    g_Radio_State.OnAdapterOn();
    g_Timers.OnUnlinked();      // re-arm discovery from zero; the watchdog does the restarting
    LOGI("BLE adapter ON — every started-state was cleared, discovery re-armed");
}
void OnAdapterOff() {
    g_Radio_State.OnAdapterOff();
    g_AdvRetry.Cancel();        // retrying into a dead adapter earns errors and leaves registrations
    g_ScanRetry.Cancel();
    LOGE("BLE adapter OFF — radio gone; not advertising, not scanning, not serving");
}
void OnConnectStarted()       { g_Timers.OnConnectStarted(); }
void OnConnectResolved()      { g_Timers.OnConnectResolved(); }
void RequestRescan()          { g_Timers.ScheduleRescan(); }

// Link state drives every deadline at once: retrying or scanning into a live link is pure churn,
// and churn is what degraded the radio in #163/#194.
void OnRadioLinked() {
    g_Timers.OnLinked();
    g_Radio_State.OnLinked();
    g_AdvRetry.Cancel();
    g_ScanRetry.Cancel();
}
void OnRadioUnlinked() {
    g_Timers.OnUnlinked();
    g_Radio_State.OnUnlinked();
}

// Shutting down. Same quiescing as a link-up — every deadline off, both backoffs abandoned — but it
// gets its own name because "linked" would be a lie at the one moment someone reading a shutdown
// log can least afford one.
void OnRadioStopped() {
    g_Timers.OnLinked();     // the "no deadline is armed" state
    g_AdvRetry.Cancel();
    g_ScanRetry.Cancel();
}

namespace {

void TickPolicies() {
    // ONE clock for every deadline. Pump() is called once per Session::Tick, which is the cadence
    // all of these want; measuring here rather than threading time through ITransport::Pump() keeps
    // that signature unchanged for every other backend. Three separate statics would be three
    // clocks that could disagree after a stall.
    static bool Started = false;
    static std::chrono::steady_clock::time_point Last;
    const auto Now = std::chrono::steady_clock::now();
    if (!Started) { Started = true; Last = Now; return; }   // first call establishes the baseline
    const auto Delta = std::chrono::duration_cast<std::chrono::nanoseconds>(Now - Last).count();
    Last = Now;
    const auto ElapsedNs = static_cast<uint64_t>(Delta);

    g_SendQueue.Tick(ElapsedNs);

    // A fast retry came due. The Kotlin verb releases our own stale registration before starting,
    // which is the ALREADY_STARTED remedy the policy deliberately does not own.
    if (g_AdvRetry.Tick(ElapsedNs))  CallShimVerb(g_StartAdvertisingMethod);
    if (g_ScanRetry.Tick(ElapsedNs)) CallShimVerb(g_StartScanningMethod);

    // Several can be due at once after a long stall (an app suspended and resumed), so each is
    // handled rather than only the first.
    const BleDiscoveryTimers::Actions Act = g_Timers.Tick(ElapsedNs);
    if (Act.AbortConnect) {
        LOGI("connect watchdog: attempt neither linked nor resolved in 6s — tearing it down");
        CallShimVerb(g_AbortConnectMethod);
    }
    if (Act.GoSymmetric) {
        LOGI("discovery watchdog: no link in 8s — dropping cached-role gates, going symmetric (#79)");
        CallShimVerb(g_GoSymmetricMethod);
    }
    if (Act.Rescan) CallShimVerb(g_StartScanningMethod);
}

}  // namespace
}  // namespace Lur::Transport


// The BLE wire identity, served to Kotlin from its single source of truth (BleProtocol.h).
//
// The Kotlin used to declare all three UUIDs itself, under a comment reading "MUST match
// Lur::Transport::BleProtocol". That is a duplication maintained by hope: nothing checks it, and a
// mismatch does not fail loudly — two phones simply never see each other, which is the hardest BLE
// symptom to attribute. Reading them across the seam makes drift impossible rather than discouraged.
// #146: is a device id read off the peer well-formed (32 lowercase hex)? A failed or truncated GATT
// read yields bytes that are not an id, and a role decided from those is one the peer cannot mirror
// — which IS the deadlock's mechanism. Guarding the read is the other half of the breaker.
// The radio finished the outstanding write; the engine queue releases the next datagram. Note it
// takes NO argument: which datagram goes next is the queue's decision, not the radio's.
static void Ble_nativeOnSendComplete(JNIEnv*, jobject) {
    Lur::Transport::OnOutboundSendComplete();
}

// The link is gone. The backlog goes with it: a datagram stream is only meaningful to a peer that
// received the earlier ones, so delivering it on reconnect would hand the peer stale state ahead of
// the resync meant to reconcile them.
static void Ble_nativeOnLinkLost(JNIEnv*, jobject) {
    Lur::Transport::OnOutboundLinkLost();
}

// --- JNI: radio events feeding the C++ discovery/retry policies (#194, #197). ---
//
// Kotlin says what the radio DID; C++ decides what follows. The two *StartFailed thunks return
// whether this failure deserves a loud line — first of a run yes, repeats no — so the retry loop
// cannot bury the message that matters.
static jboolean Ble_nativeOnAdvertiseStartFailed(JNIEnv*, jobject) {
    return Lur::Transport::OnAdvertiseStartFailed() ? JNI_TRUE : JNI_FALSE;
}
static jboolean Ble_nativeOnScanStartFailed(JNIEnv*, jobject) {
    return Lur::Transport::OnScanStartFailed() ? JNI_TRUE : JNI_FALSE;
}
static void Ble_nativeOnAdvertiseStarted(JNIEnv*, jobject) { Lur::Transport::OnAdvertiseStarted(); }
static void Ble_nativeOnScanStarted(JNIEnv*, jobject)      { Lur::Transport::OnScanStarted(); }
static void Ble_nativeOnConnectStarted(JNIEnv*, jobject)   { Lur::Transport::OnConnectStarted(); }
static void Ble_nativeOnConnectResolved(JNIEnv*, jobject)  { Lur::Transport::OnConnectResolved(); }
static void Ble_nativeScheduleRescan(JNIEnv*, jobject)     { Lur::Transport::RequestRescan(); }
static void Ble_nativeOnRadioLinked(JNIEnv*, jobject)      { Lur::Transport::OnRadioLinked(); }
static void Ble_nativeOnRadioUnlinked(JNIEnv*, jobject)    { Lur::Transport::OnRadioUnlinked(); }
static void Ble_nativeOnRadioStopped(JNIEnv*, jobject)     { Lur::Transport::OnRadioStopped(); }
static void Ble_nativeOnServing(JNIEnv*, jobject)          { Lur::Transport::OnServing(); }
static void Ble_nativeOnAdvertising(JNIEnv*, jobject)      { Lur::Transport::OnAdvertising(); }
static void Ble_nativeOnScanning(JNIEnv*, jobject)         { Lur::Transport::OnScanning(); }
static void Ble_nativeOnAdvertiseStopped(JNIEnv*, jobject) { Lur::Transport::OnAdvertiseStopped(); }
static void Ble_nativeOnScanStopped(JNIEnv*, jobject)      { Lur::Transport::OnScanStopped(); }
static void Ble_nativeOnAdapterOn(JNIEnv*, jobject)        { Lur::Transport::OnAdapterOn(); }
static void Ble_nativeOnAdapterOff(JNIEnv*, jobject)       { Lur::Transport::OnAdapterOff(); }
static jboolean Ble_nativeShouldStartAdvertising(JNIEnv*, jobject) {
    return Lur::Transport::ShouldStartAdvertising() ? JNI_TRUE : JNI_FALSE;
}
static jboolean Ble_nativeShouldStartScanning(JNIEnv*, jobject) {
    return Lur::Transport::ShouldStartScanning() ? JNI_TRUE : JNI_FALSE;
}

static jboolean Ble_nativeIsValidDeviceId(JNIEnv* Env, jobject, jbyteArray Id) {
    const jsize Len = Env->GetArrayLength(Id);
    std::string S(static_cast<std::size_t>(Len), 0);
    if (Len > 0) Env->GetByteArrayRegion(Id, 0, Len, reinterpret_cast<jbyte*>(S.data()));
    return Lur::Save::IsValidDeviceId(S) ? JNI_TRUE : JNI_FALSE;
}

static jstring Ble_nativeServiceUuid(JNIEnv* Env, jobject) {
    return Env->NewStringUTF(std::string(BleServiceUuid).c_str());
}
static jstring Ble_nativeDatagramUuid(JNIEnv* Env, jobject) {
    return Env->NewStringUTF(std::string(BleDatagramCharacteristicUuid).c_str());
}
static jstring Ble_nativeDeviceIdUuid(JNIEnv* Env, jobject) {
    return Env->NewStringUTF(std::string(BleDeviceIdCharacteristicUuid).c_str());
}


// --- JNI: BleShim hands C++ a durable reference to itself + caches send(). ---
static void JNICALL Ble_nativeSetShim(JNIEnv* Env, jobject Self) {
    if (g_Shim != nullptr) Env->DeleteGlobalRef(g_Shim);
    g_Shim = Env->NewGlobalRef(Self);
    jclass Cls = Env->GetObjectClass(Self);
    g_WriteMethod = Env->GetMethodID(Cls, "writeRaw", "([B)Z");  // dumb write; queue is C++
    g_RestartMethod = Env->GetMethodID(Cls, "restartRadio", "()V");  // #182
    g_ResetMethod = Env->GetMethodID(Cls, "resetLink", "()V");
    // Radio verbs driven by the C++ discovery/retry policies. Dumb actions; the deciding is ours.
    //
    // GetMethodID FAILS SILENTLY from our side: a mismatch leaves the id null, CallShimVerb returns
    // early, and the phone simply never retries or never goes symmetric — invisible, and shaped
    // exactly like the #194 bug this cutover exists to kill. RegisterNatives guards the Kotlin->C++
    // direction by refusing the library load; nothing guards this direction, so check it by hand and
    // say so LOUDLY. The classic way to trip it is a Kotlin verb that returns something: an
    // expression body over Handler.post infers Boolean, making the real signature ()Z.
    struct Verb { const char* Name; jmethodID* Out; };
    const Verb Verbs[] = {
        {"startAdvertising", &g_StartAdvertisingMethod},
        {"startScanning",    &g_StartScanningMethod},
        {"goSymmetric",      &g_GoSymmetricMethod},
        {"abortConnect",     &g_AbortConnectMethod},
    };
    for (const Verb& V : Verbs) {
        *V.Out = Env->GetMethodID(Cls, V.Name, "()V");
        if (*V.Out == nullptr) {
            // GetMethodID raised NoSuchMethodError; clear it or the next JNI call misbehaves.
            if (Env->ExceptionCheck()) Env->ExceptionClear();
            LOGE("JNI: BleShim.%s()V not found — the BLE discovery/retry policy cannot drive the "
                 "radio. Advertise/scan will NOT be retried and the 8s watchdog will do nothing. "
                 "Check the Kotlin returns Unit, not Boolean.", V.Name);
        }
    }
}

// --- JNI: the shared, cross-platform role tie-break (single source of truth). ---
static jint JNICALL Ble_nativeDecideRole(JNIEnv* Env, jobject /*Self*/,
                                                      jbyteArray LocalId, jbyteArray PeerId,
                                                      jint Defers) {
// FORCED STATE OVER A SYSTEM PROPERTY, SO LUR_AGENT (issue #196) — the same channel shape as
// the autoplay hook #195 moved, and note it is re-read on EVERY role decision rather than once
// at startup, so a stale property keeps applying for the life of the install.
#if LUR_AGENT
    // Dev role override (issue: test BOTH role configs on one device pair). Read the
    // prop on every decision so `adb shell setprop debug.lur.role central|peripheral`
    // takes effect on the next (re)launch/discovery without a reinstall; empty = auto.
    {
        char Prop[PROP_VALUE_MAX] = {};
        __system_property_get("debug.lur.role", Prop);
        if (std::strcmp(Prop, "central") == 0)         SetBleRoleOverride(EBleRole::Central);
        else if (std::strcmp(Prop, "peripheral") == 0) SetBleRoleOverride(EBleRole::Peripheral);
        else                                           ClearBleRoleOverride();
    }
#endif
    const jsize LocalLen = Env->GetArrayLength(LocalId);
    const jsize PeerLen  = Env->GetArrayLength(PeerId);
    std::string Local(static_cast<std::size_t>(LocalLen), '\0');
    std::string Peer(static_cast<std::size_t>(PeerLen), '\0');
    Env->GetByteArrayRegion(LocalId, 0, LocalLen, reinterpret_cast<jbyte*>(Local.data()));
    Env->GetByteArrayRegion(PeerId, 0, PeerLen, reinterpret_cast<jbyte*>(Peer.data()));
    // EBleRole::Peripheral == 0, Central == 1 (matches BleShim's constants).
    return static_cast<jint>(DecideBleRoleBreaking(Local, Peer, static_cast<int>(Defers)));
}

// --- JNI: the persistent device id (issue #17), sourced from the engine's shared
// Modules/Save so host / Android / iOS mint and read it identically. The Kotlin
// shim supplies its app-private files dir (Context.filesDir) and serves the
// returned GUID as this device's stable role identity. Idempotent: the same value
// comes back on every launch, which is exactly the stable-role fix. ---
static jbyteArray JNICALL Ble_nativeLoadOrCreateDeviceId(JNIEnv* Env, jobject /*Self*/,
                                                                jstring Dir) {
    const char* DirChars = Env->GetStringUTFChars(Dir, nullptr);
    const std::string DirPath = DirChars ? DirChars : ".";
    if (DirChars) Env->ReleaseStringUTFChars(Dir, DirChars);

    Lur::Save::Store DeviceStore(DirPath);
    const std::string Id = Lur::Save::LoadOrCreateDeviceId(DeviceStore);

    jbyteArray Arr = Env->NewByteArray(static_cast<jsize>(Id.size()));
    Env->SetByteArrayRegion(Arr, 0, static_cast<jsize>(Id.size()),
                            reinterpret_cast<const jbyte*>(Id.data()));
    return Arr;
}

// --- JNI: the last-linked peer's device id (issue #17 Step 3). Enables the cached-
// role reconnect shortcut. Empty array if none stored yet. ---
static jbyteArray JNICALL Ble_nativeLoadPeerId(JNIEnv* Env, jobject /*Self*/, jstring Dir) {
    const char* DirChars = Env->GetStringUTFChars(Dir, nullptr);
    const std::string DirPath = DirChars ? DirChars : ".";
    if (DirChars) Env->ReleaseStringUTFChars(Dir, DirChars);

    Lur::Save::Store DeviceStore(DirPath);
    const std::vector<uint8_t> Id = DeviceStore.Load(Lur::Save::PeerIdKey);

    jbyteArray Arr = Env->NewByteArray(static_cast<jsize>(Id.size()));
    if (!Id.empty())
        Env->SetByteArrayRegion(Arr, 0, static_cast<jsize>(Id.size()),
                                reinterpret_cast<const jbyte*>(Id.data()));
    return Arr;
}

static void JNICALL Ble_nativeSavePeerId(JNIEnv* Env, jobject /*Self*/,
                                                      jstring Dir, jbyteArray Data) {
    const char* DirChars = Env->GetStringUTFChars(Dir, nullptr);
    const std::string DirPath = DirChars ? DirChars : ".";
    if (DirChars) Env->ReleaseStringUTFChars(Dir, DirChars);

    const jsize Len = Env->GetArrayLength(Data);
    std::vector<uint8_t> Bytes(static_cast<std::size_t>(Len));
    if (Len > 0) Env->GetByteArrayRegion(Data, 0, Len, reinterpret_cast<jbyte*>(Bytes.data()));

    Lur::Save::Store DeviceStore(DirPath);
    DeviceStore.Save(Lur::Save::PeerIdKey, Bytes.data(), Bytes.size());
}

// --- #83 JNI: pairwise peer binding. While linked, this peripheral serves exactly ONE central; a
// third device in the room may link with someone else or wait, but must not disturb a live pair.
//
// The POLICY is Lur::Transport::PeerBinding (host-tested), not Kotlin, for the same reason the role
// tie-break is C++: the rule is tiny and was still wrong in all four transports at once, and none of
// the platform callbacks it guards can be reached by a host test. The Kotlin shim asks; C++ decides.
// Cleared by nativeOnDisconnected, so a lost link opens the binding up again — which is what keeps the
// deliberate opponent-switch (#38) possible.
//
// The id is the BluetoothDevice address, opaque here: compared, never parsed.
namespace {
Lur::Transport::PeerBinding g_PeerBinding;

std::string JStringToStd(JNIEnv* Env, jstring S) {
    if (S == nullptr) return {};
    const char* Chars = Env->GetStringUTFChars(S, nullptr);
    std::string Out = Chars != nullptr ? Chars : "";
    if (Chars != nullptr) Env->ReleaseStringUTFChars(S, Chars);
    return Out;
}
}  // namespace

// A central wrote the CCCD (enabled notifications). True = it is the peer we serve; false = a
// non-bound device whose subscription must be IGNORED rather than allowed to redirect our notifies.
static jboolean JNICALL Ble_nativeAcceptSubscriber(JNIEnv* Env, jobject /*Self*/,
                                                           jstring Addr) {
    const std::string A = JStringToStd(Env, Addr);
    const bool Ok = g_PeerBinding.AcceptSubscriber(A.c_str());
    if (!Ok) LOGI("BLE: IGNORING subscribe from %s — a live pair serves exactly one central (#83)",
                  A.c_str());
    return Ok ? JNI_TRUE : JNI_FALSE;
}

// May a datagram from this central reach the engine? Pre-link traffic passes (that IS the handshake);
// once bound, only the peer's bytes do — unfiltered, a third device injected straight into the move
// stream.
static jboolean JNICALL Ble_nativeAcceptData(JNIEnv* Env, jobject /*Self*/, jstring Addr) {
    const std::string A = JStringToStd(Env, Addr);
    return g_PeerBinding.AcceptData(A.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// Is this the bound peer? Only ITS disconnect may end the match — treating any device's departure as
// link loss is the hijack in reverse, letting an outsider kill a live pair by leaving.
static jboolean JNICALL Ble_nativeIsBoundPeer(JNIEnv* Env, jobject /*Self*/, jstring Addr) {
    const std::string A = JStringToStd(Env, Addr);
    return g_PeerBinding.IsPeer(A.c_str()) ? JNI_TRUE : JNI_FALSE;
}

static void JNICALL Ble_nativeOnConnected(JNIEnv* /*Env*/, jobject /*Self*/,
                                                       jboolean AsPeripheral) {
    // Binder thread: queue the event; the engine thread applies it in Pump().
    LOGI("BLE connected as %s", AsPeripheral ? "peripheral" : "central");
    // #83: linking as CENTRAL leaves our own GATT server with no legitimate peer, so shut it to
    // everyone. Binding only ever happened on a CCCD subscribe (the peripheral path), so a
    // central-role phone used to play the whole match with an OPEN binding and a published server —
    // and a third device that scanned before the link formed could bind itself, inject datagrams, and
    // end a healthy match by disconnecting. The peripheral path binds instead, in onDescriptorWrite.
    if (!AsPeripheral) g_PeerBinding.Close();
    g_Transport.Inbox.PushConnected();
    // The net Session sends the first Hello (central writes first) once it sees the
    // link up — no demo ping needed, and a bare 1-byte ping would now look like a move.
}

static void JNICALL Ble_nativeOnDisconnected(JNIEnv* /*Env*/, jobject /*Self*/) {
    LOGI("BLE disconnected");
    // #83: the link is genuinely gone, so release the peer binding — the next central to subscribe may
    // bind. Doing it HERE means one place covers every path that loses a link, and it is what keeps the
    // deliberate opponent-switch (#38) possible: that flow runs at session level after loss.
    g_PeerBinding.Clear();
    g_Transport.Inbox.PushDisconnected();  // Binder thread: engine applies it in Pump()
}

static void JNICALL Ble_nativeOnReceived(JNIEnv* Env, jobject /*Self*/, jbyteArray Data) {
    const jsize Len = Env->GetArrayLength(Data);
    std::vector<uint8_t> Bytes(static_cast<std::size_t>(Len));
    if (Len > 0) Env->GetByteArrayRegion(Data, 0, Len, reinterpret_cast<jbyte*>(Bytes.data()));
    // Binder thread: hand the datagram to the engine thread; Pump() calls the receiver.
    g_Transport.Inbox.PushDatagram(Bytes.data(), Bytes.size());
}

// ---- JNI binding by TABLE, not by exported symbol name ----
//
// The Java_<mangled-package>_* convention bakes the app's package into the engine's bridge, so the
// symbol `Java_com_lurmotorn_<app>_BleShim_nativeSetShim` is findable only from a class in that
// exact package — which is precisely why this file could not be shared between games and had to be
// copied instead. RegisterNatives binds an explicit table to a class we name ourselves, so the
// Kotlin shim lives in ONE fixed engine package no matter which app links the library.
//
// The engine class name is a contract with the Kotlin side. If they disagree, JNI_OnLoad fails
// LOUDLY below rather than leaving every native method unbound to fail one at a time later.
static const char* const kBleShimClass = "com/lurmotorn/engine/BleShim";

static const JNINativeMethod kBleShimMethods[] = {
    {"nativeSetShim", "()V", reinterpret_cast<void*>(Ble_nativeSetShim)},
    {"nativeIsValidDeviceId", "([B)Z", reinterpret_cast<void*>(Ble_nativeIsValidDeviceId)},
    {"nativeOnSendComplete", "()V", reinterpret_cast<void*>(Ble_nativeOnSendComplete)},
    {"nativeOnLinkLost",     "()V", reinterpret_cast<void*>(Ble_nativeOnLinkLost)},
    {"nativeOnAdvertiseStartFailed", "()Z", reinterpret_cast<void*>(Ble_nativeOnAdvertiseStartFailed)},
    {"nativeOnScanStartFailed",      "()Z", reinterpret_cast<void*>(Ble_nativeOnScanStartFailed)},
    {"nativeOnAdvertiseStarted", "()V", reinterpret_cast<void*>(Ble_nativeOnAdvertiseStarted)},
    {"nativeOnScanStarted",      "()V", reinterpret_cast<void*>(Ble_nativeOnScanStarted)},
    {"nativeOnConnectStarted",   "()V", reinterpret_cast<void*>(Ble_nativeOnConnectStarted)},
    {"nativeOnConnectResolved",  "()V", reinterpret_cast<void*>(Ble_nativeOnConnectResolved)},
    {"nativeScheduleRescan",     "()V", reinterpret_cast<void*>(Ble_nativeScheduleRescan)},
    {"nativeOnRadioLinked",      "()V", reinterpret_cast<void*>(Ble_nativeOnRadioLinked)},
    {"nativeOnRadioUnlinked",    "()V", reinterpret_cast<void*>(Ble_nativeOnRadioUnlinked)},
    {"nativeOnRadioStopped",     "()V", reinterpret_cast<void*>(Ble_nativeOnRadioStopped)},
    {"nativeOnServing",          "()V", reinterpret_cast<void*>(Ble_nativeOnServing)},
    {"nativeOnAdvertising",      "()V", reinterpret_cast<void*>(Ble_nativeOnAdvertising)},
    {"nativeOnScanning",         "()V", reinterpret_cast<void*>(Ble_nativeOnScanning)},
    {"nativeOnAdvertiseStopped", "()V", reinterpret_cast<void*>(Ble_nativeOnAdvertiseStopped)},
    {"nativeOnScanStopped",      "()V", reinterpret_cast<void*>(Ble_nativeOnScanStopped)},
    {"nativeOnAdapterOn",        "()V", reinterpret_cast<void*>(Ble_nativeOnAdapterOn)},
    {"nativeOnAdapterOff",       "()V", reinterpret_cast<void*>(Ble_nativeOnAdapterOff)},
    {"nativeShouldStartAdvertising", "()Z", reinterpret_cast<void*>(Ble_nativeShouldStartAdvertising)},
    {"nativeShouldStartScanning",    "()Z", reinterpret_cast<void*>(Ble_nativeShouldStartScanning)},
    {"nativeServiceUuid",  "()Ljava/lang/String;", reinterpret_cast<void*>(Ble_nativeServiceUuid)},
    {"nativeDatagramUuid", "()Ljava/lang/String;", reinterpret_cast<void*>(Ble_nativeDatagramUuid)},
    {"nativeDeviceIdUuid", "()Ljava/lang/String;", reinterpret_cast<void*>(Ble_nativeDeviceIdUuid)},
    {"nativeDecideRole", "([B[BI)I", reinterpret_cast<void*>(Ble_nativeDecideRole)},
    {"nativeLoadOrCreateDeviceId", "(Ljava/lang/String;)[B", reinterpret_cast<void*>(Ble_nativeLoadOrCreateDeviceId)},
    {"nativeLoadPeerId", "(Ljava/lang/String;)[B", reinterpret_cast<void*>(Ble_nativeLoadPeerId)},
    {"nativeSavePeerId", "(Ljava/lang/String;[B)V", reinterpret_cast<void*>(Ble_nativeSavePeerId)},
    {"nativeAcceptSubscriber", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(Ble_nativeAcceptSubscriber)},
    {"nativeAcceptData", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(Ble_nativeAcceptData)},
    {"nativeIsBoundPeer", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(Ble_nativeIsBoundPeer)},
    {"nativeOnConnected", "(Z)V", reinterpret_cast<void*>(Ble_nativeOnConnected)},
    {"nativeOnDisconnected", "()V", reinterpret_cast<void*>(Ble_nativeOnDisconnected)},
    {"nativeOnReceived", "([B)V", reinterpret_cast<void*>(Ble_nativeOnReceived)},
};

// Bind them. Returns false (having logged) if the class or any signature disagrees.
static bool RegisterBleShimNatives(JNIEnv* Env) {
    jclass Cls = Env->FindClass(kBleShimClass);
    if (Cls == nullptr) {
        LOGE("JNI: cannot find %s — the Kotlin shim must be in that exact package", kBleShimClass);
        return false;
    }
    const int Count = static_cast<int>(sizeof(kBleShimMethods) / sizeof(kBleShimMethods[0]));
    if (Env->RegisterNatives(Cls, kBleShimMethods, Count) != JNI_OK) {
        LOGE("JNI: RegisterNatives failed for %s (a signature disagrees with the Kotlin)",
             kBleShimClass);
        return false;
    }
    return true;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* Vm, void* /*Reserved*/) {
    g_Vm = Vm;
    JNIEnv* Env = nullptr;
    if (Vm->GetEnv(reinterpret_cast<void**>(&Env), JNI_VERSION_1_6) != JNI_OK || Env == nullptr) {
        LOGE("JNI_OnLoad: no JNIEnv — cannot bind the BLE shim natives");
        return JNI_ERR;
    }
    // Fail the LOAD, not the first call. An unbound native throws UnsatisfiedLinkError at its own
    // call site, which surfaces as an unrelated-looking failure deep in the radio flow; refusing to
    // load names the real problem at the moment it is knowable.
    if (!RegisterBleShimNatives(Env)) return JNI_ERR;
    return JNI_VERSION_1_6;
}
