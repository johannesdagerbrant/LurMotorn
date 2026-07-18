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
#if LUR_INTERNAL
#include <sys/system_properties.h>   // dev role override via debug.lur.role
#endif

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/BleProtocol.h"
#include "Lur/Transport/EventInbox.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OnlyChess", __VA_ARGS__)

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
Java_com_lurmotorn_onlychess_BleShim_nativeSetShim(JNIEnv* Env, jobject Self) {
    if (g_Shim != nullptr) Env->DeleteGlobalRef(g_Shim);
    g_Shim = Env->NewGlobalRef(Self);
    jclass Cls = Env->GetObjectClass(Self);
    g_SendMethod  = Env->GetMethodID(Cls, "send", "([B)V");
    g_ResetMethod = Env->GetMethodID(Cls, "resetLink", "()V");
}

// --- JNI: the shared, cross-platform role tie-break (single source of truth). ---
extern "C" JNIEXPORT jint JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeDecideRole(JNIEnv* Env, jobject /*Self*/,
                                                      jbyteArray LocalId, jbyteArray PeerId) {
#if LUR_INTERNAL
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
    return static_cast<jint>(DecideBleRole(Local, Peer));
}

// --- JNI: the persistent device id (issue #17), sourced from the engine's shared
// Modules/Save so host / Android / iOS mint and read it identically. The Kotlin
// shim supplies its app-private files dir (Context.filesDir) and serves the
// returned GUID as this device's stable role identity. Idempotent: the same value
// comes back on every launch, which is exactly the stable-role fix. ---
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeLoadOrCreateDeviceId(JNIEnv* Env, jobject /*Self*/,
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
Java_com_lurmotorn_onlychess_BleShim_nativeLoadPeerId(JNIEnv* Env, jobject /*Self*/, jstring Dir) {
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
Java_com_lurmotorn_onlychess_BleShim_nativeSavePeerId(JNIEnv* Env, jobject /*Self*/,
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
Java_com_lurmotorn_onlychess_BleShim_nativeOnConnected(JNIEnv* /*Env*/, jobject /*Self*/,
                                                       jboolean AsPeripheral) {
    // Binder thread: queue the event; the engine thread applies it in Pump().
    LOGI("BLE connected as %s", AsPeripheral ? "peripheral" : "central");
    g_Transport.Inbox.PushConnected();
    // The net Session sends the first Hello (central writes first) once it sees the
    // link up — no demo ping needed, and a bare 1-byte ping would now look like a move.
}

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnDisconnected(JNIEnv* /*Env*/, jobject /*Self*/) {
    LOGI("BLE disconnected");
    g_Transport.Inbox.PushDisconnected();  // Binder thread: engine applies it in Pump()
}

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnReceived(JNIEnv* Env, jobject /*Self*/, jbyteArray Data) {
    const jsize Len = Env->GetArrayLength(Data);
    std::vector<uint8_t> Bytes(static_cast<std::size_t>(Len));
    if (Len > 0) Env->GetByteArrayRegion(Data, 0, Len, reinterpret_cast<jbyte*>(Bytes.data()));
    // Binder thread: hand the datagram to the engine thread; Pump() calls the receiver.
    g_Transport.Inbox.PushDatagram(Bytes.data(), Bytes.size());
}
