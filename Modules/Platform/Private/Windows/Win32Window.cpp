// Win32 implementation of Lur::Platform::Window — the desktop Workbench window +
// input seam (roadmap Phase 0.5). The only Windows-specific windowing code; the
// Vulkan surface for the returned HWND is created by the render seam
// (Modules/Render/Platform/Windows/VulkanSurface.cpp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <chrono>
#include <cstdint>

#include "Lur/Platform/Window.h"

namespace Lur::Platform {
namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Window* WindowFrom(HWND Hwnd) {
    return reinterpret_cast<Window*>(GetWindowLongPtrW(Hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK WndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam) {
    if (Msg == WM_NCCREATE) {
        auto* Cs = reinterpret_cast<CREATESTRUCTW*>(LParam);
        SetWindowLongPtrW(Hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(Cs->lpCreateParams));
        return DefWindowProcW(Hwnd, Msg, WParam, LParam);
    }
    Window* Self = WindowFrom(Hwnd);
    if (Self == nullptr) return DefWindowProcW(Hwnd, Msg, WParam, LParam);

    switch (Msg) {
        case WM_LBUTTONDOWN:
            SetCapture(Hwnd);
            Self->PushTouch(Lur::Input::ETouchPhase::Began,
                            static_cast<float>(GET_X_LPARAM(LParam)),
                            static_cast<float>(GET_Y_LPARAM(LParam)));
            return 0;
        case WM_MOUSEMOVE:
            if ((WParam & MK_LBUTTON) != 0)
                Self->PushTouch(Lur::Input::ETouchPhase::Moved,
                                static_cast<float>(GET_X_LPARAM(LParam)),
                                static_cast<float>(GET_Y_LPARAM(LParam)));
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            Self->PushTouch(Lur::Input::ETouchPhase::Ended,
                            static_cast<float>(GET_X_LPARAM(LParam)),
                            static_cast<float>(GET_Y_LPARAM(LParam)));
            return 0;
        case WM_KEYDOWN:
            if (WParam == VK_F1) Self->RequestOverlayToggle();
            return 0;
        case WM_CLOSE:
            Self->RequestClose();
            return 0;
        default:
            return DefWindowProcW(Hwnd, Msg, WParam, LParam);
    }
}

const wchar_t* kClassName = L"LurMotornWindow";

} // namespace

Window::~Window() { Destroy(); }

bool Window::Create(const char* Title, int Width, int Height, int X, int Y) {
    HINSTANCE Instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW Wc{};
    Wc.cbSize        = sizeof(Wc);
    Wc.style         = CS_HREDRAW | CS_VREDRAW;
    Wc.lpfnWndProc   = WndProc;
    Wc.hInstance     = Instance;
    Wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    Wc.lpszClassName = kClassName;
    RegisterClassExW(&Wc);  // idempotent enough for our use; ignore "already registered"

    // Size the window so the CLIENT area is Width x Height.
    RECT R{0, 0, Width, Height};
    AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, FALSE);

    wchar_t WideTitle[256];
    MultiByteToWideChar(CP_UTF8, 0, Title, -1, WideTitle, 256);

    HWND Hwnd = CreateWindowExW(0, kClassName, WideTitle, WS_OVERLAPPEDWINDOW,
                                X < 0 ? CW_USEDEFAULT : X, Y < 0 ? CW_USEDEFAULT : Y,
                                R.right - R.left, R.bottom - R.top,
                                nullptr, nullptr, Instance, this);
    if (Hwnd == nullptr) return false;
    this->Hwnd = Hwnd;
    ShowWindow(Hwnd, SW_SHOW);
    UpdateWindow(Hwnd);
    return true;
}

void Window::Destroy() {
    if (Hwnd != nullptr) {
        DestroyWindow(static_cast<HWND>(Hwnd));
        Hwnd = nullptr;
    }
}

void Window::GetSize(int* Width, int* Height) const {
    RECT R{};
    if (Hwnd != nullptr) GetClientRect(static_cast<HWND>(Hwnd), &R);
    if (Width != nullptr)  *Width  = R.right - R.left;
    if (Height != nullptr) *Height = R.bottom - R.top;
}

bool Window::PumpEvents() {
    MSG Msg;
    while (PeekMessageW(&Msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return IsOpen();
}

std::vector<Lur::Input::TouchEvent> Window::TakeTouches() {
    std::vector<Lur::Input::TouchEvent> Out;
    Out.swap(Touches);
    return Out;
}

void Window::PushTouch(Lur::Input::ETouchPhase Phase, float XPx, float YPx) {
    Touches.push_back(Lur::Input::TouchEvent{Phase, XPx, YPx, NowNs(), /*PointerId*/ 0});
}

} // namespace Lur::Platform
