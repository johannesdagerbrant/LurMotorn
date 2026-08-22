package com.lurmotorn.shell

import com.lurmotorn.engine.BleShim

import android.app.NativeActivity
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log

/**
 * The Android app shell, shared by every game (#198).
 *
 * NativeActivity hands control to C++ (`android_main` in the app's own `.so`). This subclass exists
 * for exactly two reasons: to construct the BLE shim so native code can reach it over JNI, and to
 * request the runtime BLE permissions and start the radio once they are granted. All game logic —
 * and all rendering — lives in C++.
 *
 * ── Why this is ONE file now ─────────────────────────────────────────────────
 * There was one of these per game, 30 of 46 lines byte-identical, and the differences were only the
 * package, the class name, the native library name and the log tag. The other 16 lines were the same
 * comments RE-WRAPPED — cosmetic drift, which is the shape that precedes real drift. A third game
 * would have made a third copy.
 *
 * The two per-app values are read from the activity's OWN manifest meta-data rather than baked in,
 * so this class knows nothing about any game:
 *
 *   android.app.lib_name      the native library to load (NativeActivity already reads this itself)
 *   com.lurmotorn.log_tag     the tag `logcat -s <tag>` filters on
 *
 * Both are supplied by the app's `manifestPlaceholders` in build.gradle.kts, so each game states
 * them once, in one place, next to its application id.
 */
class LurActivity : NativeActivity() {

    // Held so it isn't garbage-collected; the C++ BLE transport calls into it.
    private lateinit var ble: BleShim
    private var logTag: String = "Lur"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val meta = packageManager
            .getActivityInfo(componentName, PackageManager.GET_META_DATA)
            .metaData
        // No fallbacks. A missing lib_name cannot be guessed, and a wrong guess would load another
        // app's library or none; a missing log tag would file this app's lines under a name nobody
        // greps. Same discipline as LUR_LOG_TAG on the C++ side, which #errors rather than defaulting.
        val libName = requireNotNull(meta?.getString("android.app.lib_name")) {
            "AndroidManifest is missing meta-data android.app.lib_name"
        }
        logTag = requireNotNull(meta?.getString("com.lurmotorn.log_tag")) {
            "AndroidManifest is missing meta-data com.lurmotorn.log_tag"
        }

        // Load the native library BEFORE touching BleShim. Its natives are bound by RegisterNatives
        // inside JNI_OnLoad, which runs at library load — so a BleShim method called first throws
        // UnsatisfiedLinkError. The library name is per-app, which is exactly why the engine class
        // cannot load it itself. Verified the hard way: dropping this crashed on first launch.
        BleShim.ensureNativeLibrary(libName)
        // The engine's radio driver, told the one thing that is this app's: its log tag. The BLE
        // service UUID reaches it from CMake, through C++.
        ble = BleShim(this, logTag)

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
            Log.e(logTag, "BLE permissions denied; cannot link")
        }
    }

    /**
     * Hand the radio back (#194). An advertiser/scanner registration we leave behind is what the
     * NEXT launch collides with (ALREADY_STARTED), and a discovery that never links leaves the app
     * looking alive but invisible and deaf. A force-stop cannot run this — but a force-stop is not
     * what we do to players, and a normal exit now leaves nothing.
     */
    override fun onDestroy() {
        if (::ble.isInitialized) ble.stop()
        super.onDestroy()
    }

    companion object {
        private const val REQUEST_BLE = 7
    }
}
