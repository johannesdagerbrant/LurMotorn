package com.lurmotorn.onlychess

import android.app.NativeActivity
import android.os.Bundle

/**
 * NativeActivity hands control to C++ (`android_main` in libonlychess.so). We
 * subclass it only so a Kotlin object — the BLE shim — is constructed and
 * reachable from native code via JNI. All game logic lives in C++.
 */
class OnlyChessActivity : NativeActivity() {

    // Held so it isn't garbage-collected; the C++ BLE transport calls into it.
    private lateinit var ble: BleShim

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ble = BleShim(this)
    }
}
