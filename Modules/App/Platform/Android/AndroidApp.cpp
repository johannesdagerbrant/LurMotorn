// The Android entry point's ceremony (issue #43, Phase 3 section B). See AndroidApp.h for what
// this deliberately does NOT own — the frame loop.
//
// Compiled into the app TARGET rather than lur_app, for the same reason as AppPlatform.cpp and the
// BLE bridge: it needs the NDK's native_app_glue and the app's LUR_LOG_TAG, and only an app build
// has an Android toolchain at all.
#include "Lur/App/AndroidApp.h"

#include <android/configuration.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include "Lur/App/Platform.h"
#include "Lur/Core/Log.h"
#include "Lur/Core/LogTag.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"

namespace Lur::App {

void AndroidApp::Start(android_app* A, Callbacks C) {
    App = A;
    Cb = std::move(C);

    // FIRST, before anything can try to report a problem: give the engine logger a home. Chess
    // never installed a sink on either phone, so every Lur::Log::* line from inside the engine —
    // including the build-fingerprint mismatch that exists to be seen — went to a stdout nobody
    // reads (#43 section B).
    Platform::InstallLogSink();

    App->userData = this;
    App->onAppCmd = &AndroidApp::OnCmd;
    App->onInputEvent = &AndroidApp::OnInputEvent;
}

void AndroidApp::OnCmd(android_app* A, int32_t Cmd) {
    auto* Self = static_cast<AndroidApp*>(A->userData);
    if (Self == nullptr) return;
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (A->window == nullptr) break;
            Self->Rend = Lur::Render::VulkanRenderer::Create(Lur::Core::LogTag);
            Self->Ready = Self->Rend != nullptr && Self->Rend->Init(A->window);
            Lur::Log::Info("Renderer init: %s", Self->Ready ? "ok" : "failed");
            if (Self->Ready && Self->Cb.OnSurfaceReady) Self->Cb.OnSurfaceReady();
            break;
        case APP_CMD_TERM_WINDOW:
            // The game first, then the GPU: chess stops its audio device in OnSurfaceLost, and
            // anything holding renderer handles must let go while they are still valid.
            if (Self->Cb.OnSurfaceLost) Self->Cb.OnSurfaceLost();
            if (Self->Rend != nullptr) Self->Rend->Shutdown();
            Self->Ready = false;
            break;
        case APP_CMD_PAUSE:
            if (Self->Cb.OnPause) Self->Cb.OnPause();
            break;
        default:
            break;
    }
}

int32_t AndroidApp::OnInputEvent(android_app* A, AInputEvent* Event) {
    auto* Self = static_cast<AndroidApp*>(A->userData);
    // Drop events with no window to interpret them against: the coordinates a game needs are
    // relative to a surface size, and there isn't one.
    if (Self == nullptr || !Self->Ready || A->window == nullptr) return 0;
    if (!Self->Cb.OnInput) return 0;
    return Self->Cb.OnInput(Event) ? 1 : 0;
}

bool AndroidApp::IsRunning() const { return App != nullptr && !App->destroyRequested; }

void AndroidApp::PumpEvents() {
    int Events = 0;
    android_poll_source* Source = nullptr;
    // Re-evaluate the timeout on EVERY poll rather than hoisting it: INIT_WINDOW flips Ready from
    // inside this very loop, and a hoisted -1 would then block forever waiting for an event that
    // isn't coming, having just been handed the window it needed to start drawing.
    while (ALooper_pollOnce(Ready ? 0 : -1, nullptr, &Events,
                           reinterpret_cast<void**>(&Source)) >= 0) {
        if (Source != nullptr) Source->process(App, Source);
        if (App->destroyRequested) break;
    }
}

void AndroidApp::Shutdown() {
    if (Rend != nullptr) {
        Rend->Shutdown();
        Rend = nullptr;
    }
    Ready = false;
}

const char* AndroidApp::SaveDir() const {
    if (App != nullptr && App->activity != nullptr && App->activity->internalDataPath != nullptr)
        return App->activity->internalDataPath;
    return ".";
}

float AndroidApp::Width() const {
    if (App == nullptr || App->window == nullptr) return 0.0f;
    return static_cast<float>(ANativeWindow_getWidth(App->window));
}

float AndroidApp::Height() const {
    if (App == nullptr || App->window == nullptr) return 0.0f;
    return static_cast<float>(ANativeWindow_getHeight(App->window));
}

float AndroidApp::DisplayDensity() const {
    if (App == nullptr || App->config == nullptr) return 1.0f;
    return static_cast<float>(AConfiguration_getDensity(App->config)) / 160.0f;
}

}  // namespace Lur::App
