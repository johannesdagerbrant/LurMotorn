#pragma once

// The Android entry point's ceremony, written ONCE (issue #43, Phase 3 section B).
//
// A NativeActivity main is roughly two things: a pile of OS ritual that no game decides, and a
// frame loop whose exact shape is a latency decision the game must own. This class is the first
// half. Both chess and RPS had re-derived it: the same glue-callback wiring, the same
// internalDataPath lookup, the same renderer create/init on APP_CMD_INIT_WINDOW, the same
// shutdown on APP_CMD_TERM_WINDOW, the same six-line ALooper drain including the same non-obvious
// timeout rule — and the same ANativeWindow_getWidth/Height at every use site.
//
// WHAT IT DELIBERATELY DOES NOT OWN: the loop. The two games' loops differ in ways that matter and
// were arrived at by measurement — chess ticks the session at the top and drains the inbox again
// immediately before drawing (#188/#189), RPS spends the GPU fence-wait up front so the touch it
// renders is the freshest possible (wait-early/sample-late), and RPS's sim runs on its own thread
// besides. An entry point that owned `while (...)` would have to pick one cadence, and the cadence
// is exactly where the input-latency work lives. So the game keeps its `while`, and calls
// IsRunning() / PumpEvents() inside it — which is what the two loops already said, in their own
// words.
//
// This is also where the remaining section-B absorptions want to live: pause/persist, swapchain
// resize, safe-area insets and save-dir discovery all need the `android_app*` handle, which is why
// they wait on the entry point rather than each growing a handle parameter.

#include <functional>

#include "Lur/Render/Renderer.h"

// From the NDK's native_app_glue / <android/input.h>. Forward-declared so a game that includes
// this header does not inherit the glue's include path just to hold a Lur::App::AndroidApp.
struct android_app;
struct AInputEvent;

namespace Lur::App {

class AndroidApp {
public:
    struct Callbacks {
        // The window exists and the Vulkan renderer is up on it: create GPU resources, size the
        // HUD, start audio. Called from inside PumpEvents (APP_CMD_INIT_WINDOW), which is why the
        // renderer cannot simply be created before the loop.
        std::function<void()> OnSurfaceReady;

        // The window is going away. The renderer is still valid inside this callback and is shut
        // down immediately after it returns — chess stops its audio device here, which must
        // happen before the GPU teardown, not after.
        std::function<void()> OnSurfaceLost;

        // Backgrounded (APP_CMD_PAUSE). OPTIONAL, and leaving it unset is a decision rather than
        // an oversight: RPS handles no PAUSE today, and GameHost::OnBackground is a no-op without
        // record sync, so wiring one in would be a behaviour change (what should a match in
        // flight do?) dressed as a cleanup. Chess persists its in-progress record here.
        std::function<void()> OnPause;

        // A raw input event, exactly as the glue delivers it. Return true if consumed. The event
        // pipeline itself (multi-touch, recognizers) is a later section-B item; this seam only
        // stops every main from re-wiring onInputEvent by hand.
        std::function<bool(AInputEvent*)> OnInput;
    };

    // Install the engine log sink, take over the glue's callbacks, and stop there. Nothing else can
    // be brought up yet: on Android the window arrives asynchronously, so the renderer is created
    // later, from inside PumpEvents, and announced through OnSurfaceReady.
    //
    // The Vulkan application name comes from Lur::Core::LogTag (LUR_LOG_TAG) rather than a
    // parameter — same reasoning as InstallLogSink: the two mains passed string literals that
    // happened to match their tags, and a build that forgets the tag now fails to compile instead
    // of naming itself after another app.
    void Start(android_app* App, Callbacks Cb);

    // The game loop's `while` condition. Separate from PumpEvents on purpose: both mains test it
    // at the TOP of the loop and drain events in the middle, with game work either side.
    bool IsRunning() const;

    // Drain the looper: dispatches window/lifecycle commands and input, so the callbacks above
    // fire from here. Blocks until something happens while there is nothing to draw, and returns
    // immediately once there is — see the timeout note in the implementation, which is a real bug
    // this consolidates rather than a style choice.
    void PumpEvents();

    // Final teardown, after the loop. Idempotent: TERM_WINDOW may already have shut the renderer
    // down, and a destroy without one is equally normal.
    void Shutdown();

    bool IsReady() const { return Ready; }
    Lur::Render::IRenderer* Renderer() const { return Rend; }
    android_app* Handle() const { return App; }

    // Where this app may write. The internal data dir — the same path the Kotlin BLE shim reads
    // the device GUID from, which is why both agree on identity. Falls back to "." if the activity
    // is gone (it is not, in practice, at the point a main asks).
    const char* SaveDir() const;

    // Backing-store size of the current window, in pixels. Zero when there is no window.
    float Width() const;
    float Height() const;

    // Screen density as a dp multiplier (160 dpi == 1.0). RPS scales its safe-area insets by this;
    // the proper WindowInsets seam is still a later item.
    float DisplayDensity() const;

private:
    static void OnCmd(android_app* App, int32_t Cmd);
    static int32_t OnInputEvent(android_app* App, AInputEvent* Event);

    android_app* App = nullptr;
    Lur::Render::IRenderer* Rend = nullptr;
    Callbacks Cb;
    bool Ready = false;
};

}  // namespace Lur::App
