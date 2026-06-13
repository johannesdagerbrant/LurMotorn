package com.lurmotorn.onlychess

import android.content.Context

/**
 * Thin Bluetooth Low Energy shim — the ONLY job is to move opaque `ByteArray`
 * datagrams between this device and the peer, and report connection state. All
 * framing, encoding, and game logic live in C++ (`Lur::Transport::ITransport`
 * consumes this over JNI). Full GATT advertise / scan / connect is task #8.
 */
class BleShim(private val context: Context) {

    // --- Called FROM C++ (the Lur::Transport BLE backend) ---

    /** Send one datagram to the peer. The bytes are already minimal — send as-is. */
    @Suppress("unused")
    fun send(bytes: ByteArray) {
        // TODO(#8): write to the connected GATT characteristic (notify/write).
    }

    // --- Called INTO C++ when the radio reports events ---

    private external fun nativeOnConnected()
    private external fun nativeOnReceived(bytes: ByteArray)

    companion object {
        init {
            // Same .so NativeActivity loads (android.app.lib_name = "onlychess").
            System.loadLibrary("onlychess")
        }
    }
}
