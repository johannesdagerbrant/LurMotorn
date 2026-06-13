// Bridges Lur::Transport::ITransport to the Kotlin BleShim over JNI. STUB seam:
// the contract and the JNI entry points are here; full GATT advertise/scan/
// connect + datagram plumbing is task #8.
#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <functional>
#include <vector>

#include "Lur/Transport/Ble.h"

namespace Lur::Transport {
namespace {

class AndroidBleTransport : public ITransport {
public:
    void Send(const uint8_t* /*Data*/, std::size_t /*Size*/) override {
        // TODO(#8): JNI-call BleShim.send(byte[]) on the connected link.
    }
    void SetReceiver(Receiver NewReceiver) override { ReceiverFn = std::move(NewReceiver); }
    bool IsConnected() const override { return Connected; }

    Receiver ReceiverFn;
    bool Connected = false;
};

// One link to one peer — local multiplayer is strictly 1:1.
AndroidBleTransport g_Transport;

} // namespace

ITransport* CreateBleTransport(EBleRole /*Role*/) { return &g_Transport; }

} // namespace Lur::Transport

// --- JNI: called from Kotlin BleShim when the radio reports events ---

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnConnected(JNIEnv* /*Env*/, jobject /*Self*/) {
    __android_log_print(ANDROID_LOG_INFO, "OnlyChess", "BLE connected (TODO #8)");
    // TODO(#8): mark g_Transport connected; kick off the net Hello handshake.
}

extern "C" JNIEXPORT void JNICALL
Java_com_lurmotorn_onlychess_BleShim_nativeOnReceived(JNIEnv* /*Env*/, jobject /*Self*/,
                                                      jbyteArray /*Data*/) {
    // TODO(#8): copy bytes out of the jbyteArray and dispatch to
    // g_Transport.ReceiverFn so the net/game layers see the datagram.
}
