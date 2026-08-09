// Bridges Lur::Transport::ITransport to the Kotlin BleShim over JNI (issue #3,
// Android half). The Kotlin side owns the radio (advertise/scan/GATT); this side
// is the platform-neutral seam the engine speaks to: Send() pushes a datagram to
// BleShim.send over JNI, and the BleShim's radio callbacks land in the native
// methods below, which feed bytes/state back to the engine.
#include <jni.h>
#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#if LUR_AGENT
#include <sys/system_properties.h>   // role override via debug.lur.role (#196/#197)
#endif

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/BleProtocol.h"
#include "Lur/Transport/EventInbox.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyRps", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OnlyRps", __VA_ARGS__)

namespace Lur::Transport {
namespace {

// The BLE radio callbacks (below) fire on Binder threads; they Push into Inbox, and
// the engine thread drains it via Pump() (called from Session::Tick). So ReceiverFn
// and Connected are only ever touched on the engine thread — the documented contract
// (issue #40). Inbox is the one thread-crossing point.
class AndroidBleTransport : public ITransport, public EventInbox::Sink {
public:
    void Send(const uint8_t* Data, std::size_t Size) override;
    void SetReceiver(Receiver NewReceiver) override { ReceiverFn = std::move(NewReceiver); }
    bool IsConnected() const override { return Connected; }
    void ResetLink() override;
    void RestartRadio() override;
    void Pump() override { Inbox.Drain(*this); }  // engine thread: dispatch queued events

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
jmethodID g_SendMethod    = nullptr; // BleShim.send([B)V
jmethodID g_ResetMethod   = nullptr; // BleShim.resetLink()V
jmethodID g_RestartMethod = nullptr; // BleShim.restartRadio()V  (#182 hard reset)

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

void AndroidBleTransport::Send(const uint8_t* Data, std::size_t Size) {
    if (g_Shim == nullptr || g_SendMethod == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;

    jbyteArray Arr = Env->NewByteArray(static_cast<jsize>(Size));
    Env->SetByteArrayRegion(Arr, 0, static_cast<jsize>(Size),
                            reinterpret_cast<const jbyte*>(Data));
    Env->CallVoidMethod(g_Shim, g_SendMethod, Arr);
    Env->DeleteLocalRef(Arr);
}

// The net keepalive timed out — the peer is silently gone. Force the Kotlin radio to
// drop the (dead) link and resume discovery, rather than waiting out the BLE
// supervision timeout (10-20s) for a disconnect callback. This makes a killed-peer
// drop detected in ~5s on Android too, matching iOS.
void AndroidBleTransport::ResetLink() {
    if (g_Shim == nullptr || g_ResetMethod == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;
    Env->CallVoidMethod(g_Shim, g_ResetMethod);
}

// #182: the harder reset the net layer escalates to once a link is judged HALF-OPEN — a soft
// ResetLink (above) merely drops the link and rediscovers; on a WEDGED BLE stack that clears
// nothing (80 tried on hardware). This tears the whole BluetoothGatt down (close, not just
// disconnect) and refresh()es its stale service cache before rebuilding the radio — the Kotlin
// side does the real work. Bounded by the Session (MaxRadioRestarts) so it can't become churn.
void AndroidBleTransport::RestartRadio() {
    if (g_Shim == nullptr || g_RestartMethod == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;
    Env->CallVoidMethod(g_Shim, g_RestartMethod);
}

} // namespace

ITransport* CreateBleTransport(EBleRole /*Role*/) { return &g_Transport; }

} // namespace Lur::Transport

using namespace Lur::Transport;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* Vm, void* /*Reserved*/) {
    g_Vm = Vm;
    return JNI_VERSION_1_6;
}

// --- JNI: BleShim hands C++ a durable reference to itself + caches send(). ---
extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeSetShim(JNIEnv* Env, jobject Self) {
    if (g_Shim != nullptr) Env->DeleteGlobalRef(g_Shim);
    g_Shim = Env->NewGlobalRef(Self);
    jclass Cls = Env->GetObjectClass(Self);
    g_SendMethod    = Env->GetMethodID(Cls, "send", "([B)V");
    g_ResetMethod   = Env->GetMethodID(Cls, "resetLink", "()V");
    g_RestartMethod = Env->GetMethodID(Cls, "restartRadio", "()V");  // #182
}

namespace {
std::string JBytesToString(JNIEnv* Env, jbyteArray Arr) {
    const jsize Len = Env->GetArrayLength(Arr);
    std::string S(static_cast<std::size_t>(Len), '\0');
    Env->GetByteArrayRegion(Arr, 0, Len, reinterpret_cast<jbyte*>(S.data()));
    return S;
}
#if LUR_AGENT
// FORCED STATE OVER A SYSTEM PROPERTY, SO LUR_AGENT — settled in #197. This was LUR_INTERNAL,
// which meant it shipped in every Development build, i.e. every build someone actually plays.
// Re-read the role pin on EVERY decision, so `adb shell setprop debug.lur.role
// central|peripheral` takes effect on the next (re)launch/discovery without a reinstall;
// empty = auto. Returns whether a pin is now in force, so the caller can say so out loud —
// the prop survives until reboot, and a stale one from an earlier rig run is the first thing
// to suspect when the roles come out wrong (#146).
bool RefreshRolePin() {
    char Prop[PROP_VALUE_MAX] = {};
    __system_property_get("debug.lur.role", Prop);
    if (std::strcmp(Prop, "central") == 0)         SetBleRoleOverride(EBleRole::Central);
    else if (std::strcmp(Prop, "peripheral") == 0) SetBleRoleOverride(EBleRole::Peripheral);
    else                                           ClearBleRoleOverride();
    if (IsBleRolePinned())
        LOGI("BLE role PINNED by debug.lur.role=%s (dev override; `setprop debug.lur.role \"\"` "
             "to restore the auto tie-break)", Prop);
    return IsBleRolePinned();
}
#endif
}  // namespace

// --- JNI: the shared, cross-platform role tie-break (single source of truth). Defers is how
// many times we have already connected out, been told "you're the peripheral" and deferred
// with no peer ever claiming Central — at the threshold the shared breaker takes Central so a
// both-Peripheral state cannot deadlock (#146). ---
extern "C" JNIEXPORT jint JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeDecideRole(JNIEnv* Env, jobject /*Self*/,
                                                      jbyteArray LocalId, jbyteArray PeerId,
                                                      jint Defers) {
#if LUR_AGENT
    RefreshRolePin();
#endif
    const std::string Local = JBytesToString(Env, LocalId);
    const std::string Peer  = JBytesToString(Env, PeerId);
    // EBleRole::Peripheral == 0, Central == 1 (matches BleShim's constants).
    return static_cast<jint>(DecideBleRoleBreaking(Local, Peer, static_cast<int>(Defers)));
}

// --- #83 JNI: pairwise peer binding. While linked, this peripheral serves exactly ONE central; a
// third device in the room may link with someone else or wait, but must not disturb a live pair.
//
// The POLICY is Lur::Transport::PeerBinding (host-tested), not Kotlin, for the same reason the role
// tie-break above is C++: the rule is tiny and was still wrong in all four transports at once, and
// none of the platform callbacks it guards can be reached by a host test. The Kotlin shim asks; C++
// decides. Cleared by nativeOnDisconnected, so a lost link opens the binding up again — which is what
// keeps chess's deliberate opponent-switch (#38) possible.
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
extern "C" JNIEXPORT jboolean JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeAcceptSubscriber(JNIEnv* Env, jobject /*Self*/, jstring Addr) {
    const std::string A = JStringToStd(Env, Addr);
    const bool Ok = g_PeerBinding.AcceptSubscriber(A.c_str());
    if (!Ok) LOGI("BLE: IGNORING subscribe from %s — a live pair serves exactly one central (#83)",
                  A.c_str());
    return Ok ? JNI_TRUE : JNI_FALSE;
}

// May a datagram from this central reach the engine? Pre-link traffic passes (that IS the handshake);
// once bound, only the peer's bytes do — unfiltered, a third device injected straight into the
// lockstep/move stream.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeAcceptData(JNIEnv* Env, jobject /*Self*/, jstring Addr) {
    const std::string A = JStringToStd(Env, Addr);
    return g_PeerBinding.AcceptData(A.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// Is this the bound peer? Only ITS disconnect may end the match — treating any device's departure as
// link loss is the hijack in reverse, letting an outsider kill a live pair by leaving.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeIsBoundPeer(JNIEnv* Env, jobject /*Self*/, jstring Addr) {
    const std::string A = JStringToStd(Env, Addr);
    return g_PeerBinding.IsPeer(A.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// --- JNI: is a device id read off the peer well-formed (32 lowercase hex)? A failed/truncated
// GATT read yields bytes that are not an id, and deciding a role from those is what produced
// the both-Peripheral deadlock — so the shim retries the read instead of trusting it (#146). ---
extern "C" JNIEXPORT jboolean JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeIsValidDeviceId(JNIEnv* Env, jobject /*Self*/,
                                                           jbyteArray Id) {
    return Lur::Save::IsValidDeviceId(JBytesToString(Env, Id)) ? JNI_TRUE : JNI_FALSE;
}

// --- JNI: the persistent device id (issue #17), sourced from the engine's shared
// Modules/Save so host / Android / iOS mint and read it identically. The Kotlin
// shim supplies its app-private files dir (Context.filesDir) and serves the
// returned GUID as this device's stable role identity. Idempotent: the same value
// comes back on every launch, which is exactly the stable-role fix. ---
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeLoadOrCreateDeviceId(JNIEnv* Env, jobject /*Self*/,
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
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeLoadPeerId(JNIEnv* Env, jobject /*Self*/, jstring Dir) {
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

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeSavePeerId(JNIEnv* Env, jobject /*Self*/,
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

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeOnConnected(JNIEnv* /*Env*/, jobject /*Self*/,
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

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeOnDisconnected(JNIEnv* /*Env*/, jobject /*Self*/) {
    LOGI("BLE disconnected");
    // #83: the link is genuinely gone, so release the peer binding — the next central to subscribe may
    // bind. Doing it HERE means one place covers every path that loses a link, and it is what keeps
    // chess's deliberate opponent-switch (#38) possible: that flow runs at session level after loss.
    g_PeerBinding.Clear();
    g_Transport.Inbox.PushDisconnected();  // Binder thread: engine applies it in Pump()
}

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlyrps_BleShim_nativeOnReceived(JNIEnv* Env, jobject /*Self*/, jbyteArray Data) {
    const jsize Len = Env->GetArrayLength(Data);
    std::vector<uint8_t> Bytes(static_cast<std::size_t>(Len));
    if (Len > 0) Env->GetByteArrayRegion(Data, 0, Len, reinterpret_cast<jbyte*>(Bytes.data()));
    // Binder thread: hand the datagram to the engine thread; Pump() calls the receiver.
    g_Transport.Inbox.PushDatagram(Bytes.data(), Bytes.size());
}
