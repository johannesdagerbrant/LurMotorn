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

#include "Lur/Core/LogTag.h"
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
jmethodID g_SendMethod  = nullptr; // BleShim.send([B)V
jmethodID g_RestartMethod = nullptr;  // BleShim.restartRadio()V (#182 hard reset)
jmethodID g_ResetMethod = nullptr; // BleShim.resetLink()V

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
    if (g_Shim == nullptr || g_SendMethod == nullptr) return;
    JNIEnv* Env = EnvForThisThread();
    if (Env == nullptr) return;

    jbyteArray Arr = Env->NewByteArray(static_cast<jsize>(Size));
    Env->SetByteArrayRegion(Arr, 0, static_cast<jsize>(Size),
                            reinterpret_cast<const jbyte*>(Data));
    // Urgency is passed, never inferred. The Kotlin used to guess it from the array's LENGTH
    // ("1 byte means a live move"), which put one game's wire format inside the radio shim and
    // broke silently when that format changed — the fast path just stopped happening.
    Env->CallVoidMethod(g_Shim, g_SendMethod, Arr, static_cast<jboolean>(Expedited));
    Env->DeleteLocalRef(Arr);
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

ITransport* CreateBleTransport(EBleRole /*Role*/) { return &g_Transport; }

} // namespace Lur::Transport

using namespace Lur::Transport;


// The BLE wire identity, served to Kotlin from its single source of truth (BleProtocol.h).
//
// The Kotlin used to declare all three UUIDs itself, under a comment reading "MUST match
// Lur::Transport::BleProtocol". That is a duplication maintained by hope: nothing checks it, and a
// mismatch does not fail loudly — two phones simply never see each other, which is the hardest BLE
// symptom to attribute. Reading them across the seam makes drift impossible rather than discouraged.
// #146: is a device id read off the peer well-formed (32 lowercase hex)? A failed or truncated GATT
// read yields bytes that are not an id, and a role decided from those is one the peer cannot mirror
// — which IS the deadlock's mechanism. Guarding the read is the other half of the breaker.
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
    g_SendMethod  = Env->GetMethodID(Cls, "send", "([BZ)V");   // (bytes, expedited)
    g_RestartMethod = Env->GetMethodID(Cls, "restartRadio", "()V");  // #182
    g_ResetMethod = Env->GetMethodID(Cls, "resetLink", "()V");
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
