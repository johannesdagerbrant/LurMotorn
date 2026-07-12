// Bridges Lur::Transport::ITransport to the Kotlin BleShim over JNI (issue #3,
// Android half). The Kotlin side owns the radio (advertise/scan/GATT); this side
// is the platform-neutral seam the engine speaks to: Send() pushes a datagram to
// BleShim.send over JNI, and the BleShim's radio callbacks land in the native
// methods below, which feed bytes/state back to the engine.
#include <jni.h>
#include <android/log.h>
#include <cstdint>
#include <string>
#include <vector>

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/BleProtocol.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyChess", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OnlyChess", __VA_ARGS__)

namespace Lur::Transport {
namespace {

class AndroidBleTransport : public ITransport {
public:
    void Send(const uint8_t* Data, std::size_t Size) override;
    void SetReceiver(Receiver NewReceiver) override { ReceiverFn = std::move(NewReceiver); }
    bool IsConnected() const override { return Connected; }

    Receiver ReceiverFn;
    bool     Connected = false;
};

// One link to one peer — local multiplayer is strictly 1:1.
AndroidBleTransport g_Transport;

// JNI plumbing cached at load / shim-registration time.
JavaVM*   g_Vm        = nullptr;
jobject   g_Shim      = nullptr;  // global ref to the Kotlin BleShim
jmethodID g_SendMethod = nullptr; // BleShim.send([B)V

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
    g_SendMethod = Env->GetMethodID(Cls, "send", "([B)V");
}

// --- JNI: the shared, cross-platform role tie-break (single source of truth). ---
extern "C" JNIEXPORT jint JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeDecideRole(JNIEnv* Env, jobject /*Self*/,
                                                      jbyteArray LocalId, jbyteArray PeerId) {
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

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnConnected(JNIEnv* /*Env*/, jobject /*Self*/,
                                                       jboolean AsPeripheral) {
    g_Transport.Connected = true;
    LOGI("BLE connected as %s", AsPeripheral ? "peripheral" : "central");
    // The central writes first (the peripheral can't notify until the central has
    // enabled notifications); the peer replies from its receiver. See AndroidMain.
    if (AsPeripheral == JNI_FALSE) {
        const uint8_t Ping[] = {0xC5};
        g_Transport.Send(Ping, sizeof(Ping));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnDisconnected(JNIEnv* /*Env*/, jobject /*Self*/) {
    g_Transport.Connected = false;
    LOGI("BLE disconnected");
}

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnReceived(JNIEnv* Env, jobject /*Self*/, jbyteArray Data) {
    const jsize Len = Env->GetArrayLength(Data);
    std::vector<uint8_t> Bytes(static_cast<std::size_t>(Len));
    if (Len > 0) Env->GetByteArrayRegion(Data, 0, Len, reinterpret_cast<jbyte*>(Bytes.data()));
    if (g_Transport.ReceiverFn) g_Transport.ReceiverFn(Bytes.data(), Bytes.size());
}
