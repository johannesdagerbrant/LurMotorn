#pragma once
#include <vector>

#include "Lur/Input/Input.h"

namespace Lur::Platform {

// A minimal desktop window + input seam for the Workbench build (roadmap Phase 0.5).
// Win32 today; the shape (create, pump, native handle, drained TouchEvents) is what
// any desktop backend provides. Mouse events normalize into Lur::Input::TouchEvent —
// the struct's first real instantiation — so game input is written once across the
// phones and the desktop, per the shared-first doctrine.
//
// Kept free of <windows.h> so game/engine code that includes this stays clean; the
// HWND lives behind a void* and all Win32 code is in the .cpp.
class Window {
public:
    ~Window();

    // X/Y default to -1 (let the OS place the window); pass explicit coords to lay
    // out several windows side by side (the two-window desktop game, issue #53).
    bool Create(const char* Title, int Width, int Height, int X = -1, int Y = -1);
    void Destroy();

    void* NativeHandle() const { return Hwnd; }   // HWND, for the Vulkan surface
    bool  IsOpen() const { return Hwnd != nullptr && !ShouldClose; }
    void  GetSize(int* Width, int* Height) const;

    // Process pending OS messages (fills the touch queue). Returns false once the
    // window has been closed.
    bool PumpEvents();

    // Move out the touch events collected since the last call (mouse taps/drags).
    std::vector<Lur::Input::TouchEvent> TakeTouches();

    // True once per F1 press — a debug toggle the game consumes (issue #54 overlay).
    bool TakeOverlayToggle() { bool T = OverlayToggle; OverlayToggle = false; return T; }

    // Move out the key-DOWN edges (virtual-key codes) collected since the last call.
    // Auto-repeat is filtered at the source, so a held key is a single press — what a
    // discrete game action (e.g. RPS's 1-4 production buttons) wants. Casing-agnostic
    // raw VKs; the game maps the codes it cares about.
    std::vector<uint32_t> TakeKeys() { std::vector<uint32_t> K; K.swap(Keys); return K; }

    // --- Internal: driven by the Win32 WndProc. Not for game code. ---
    void PushTouch(Lur::Input::ETouchPhase Phase, float XPx, float YPx);
    void PushKey(uint32_t Vk) { Keys.push_back(Vk); }
    void RequestClose() { ShouldClose = true; }
    void RequestOverlayToggle() { OverlayToggle = true; }

private:
    void* Hwnd = nullptr;   // HWND
    bool  ShouldClose = false;
    bool  OverlayToggle = false;
    std::vector<Lur::Input::TouchEvent> Touches;
    std::vector<uint32_t> Keys;
};

} // namespace Lur::Platform
