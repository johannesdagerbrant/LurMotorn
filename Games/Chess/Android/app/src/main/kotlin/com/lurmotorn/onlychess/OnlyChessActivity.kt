package com.lurmotorn.onlychess

import com.lurmotorn.engine.BleShim

import android.app.NativeActivity
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log

/**
 * NativeActivity hands control to C++ (`android_main` in libonlychess.so). We
 * subclass it to (1) construct the BLE shim so native code can reach it via JNI,
 * and (2) request the runtime BLE permissions, starting the radio once granted.
 * All game logic lives in C++.
 */
class OnlyChessActivity : NativeActivity() {

    // Held so it isn't garbage-collected; the C++ BLE transport calls into it.
    private lateinit var ble: BleShim

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Load the native library BEFORE touching BleShim. Its natives are bound by
        // RegisterNatives inside JNI_OnLoad, which runs at library load — so a BleShim method
        // called first throws UnsatisfiedLinkError. The library name is per-app
        // (android.app.lib_name), which is exactly why the engine class cannot load it itself.
        // Verified the hard way: dropping this produced that crash on the first launch.
        BleShim.ensureNativeLibrary("onlychess")
        // The engine's radio driver, told the two things that are this app's: its log tag
        // (what `logcat -s <tag>` filters on) and, via CMake, its BLE service UUID.
        ble = BleShim(this, "OnlyChess")

        val perms = ble.requiredPermissions()
        if (perms.any { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }) {
            requestPermissions(perms, REQUEST_BLE)
        } else {
            ble.onPermissionsReady()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQUEST_BLE) return
        if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
            ble.onPermissionsReady()
        } else {
            Log.e("OnlyChess", "BLE permissions denied; cannot link")
        }
    }

    /**
     * Hand the radio back (#194). An advertiser/scanner registration we leave behind is what
     * the NEXT launch collides with (ALREADY_STARTED), and a discovery that never links
     * leaves the app looking alive but invisible and deaf. A force-stop cannot run this — but
     * a force-stop is not what we do to players, and a normal exit now leaves nothing.
     */
    override fun onDestroy() {
        if (::ble.isInitialized) ble.stop()
        super.onDestroy()
    }

    companion object {
        private const val REQUEST_BLE = 7
    }
}
