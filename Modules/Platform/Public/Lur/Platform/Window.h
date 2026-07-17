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

    bool Create(const char* Title, int Width, int Height);
    void Destroy();

    void* NativeHandle() const { return Hwnd; }   // HWND, for the Vulkan surface
    bool  IsOpen() const { return Hwnd != nullptr && !ShouldClose; }
    void  GetSize(int* Width, int* Height) const;

    // Process pending OS messages (fills the touch queue). Returns false once the
    // window has been closed.
    bool PumpEvents();

    // Move out the touch events collected since the last call (mouse taps/drags).
    std::vector<Lur::Input::TouchEvent> TakeTouches();

    // --- Internal: driven by the Win32 WndProc. Not for game code. ---
    void PushTouch(Lur::Input::ETouchPhase Phase, float XPx, float YPx);
    void RequestClose() { ShouldClose = true; }

private:
    void* Hwnd = nullptr;   // HWND
    bool  ShouldClose = false;
    std::vector<Lur::Input::TouchEvent> Touches;
};

} // namespace Lur::Platform
