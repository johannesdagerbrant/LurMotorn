#pragma once
// Lur::DevGui::Numpad — a self-contained numeric entry pad for the dev-GUI layer (#113/
// #115). It exists because raw NativeActivity can't raise the OS soft keyboard (#118
// no-go), and numeric CVars only need digits/./sign anyway: a tap-driven pad is the
// portable text-entry answer on phones AND desktop (mouse) with zero platform input glue.
//
// Layout (as specified):   1 2 3
//                          4 5 6
//                          7 8 9
//                          . 0 Enter
//
// PURE logic + shared geometry: KeyRect is the single source for BOTH the renderer (draws
// each key there) and Tap (hit-tests there), so the two can never drift. It accumulates a
// value string as keys are pressed; Enter is a one-shot signal the caller drains to commit
// the buffer (e.g. CVar SetFromString). Reusable for any numeric dev field. Dev-only.
#include <string>

namespace Lur::DevGui {

class Numpad {
public:
    static constexpr int Rows = 4;
    static constexpr int Cols = 3;

    static const char* Label(int R, int C) {
        static const char* L[Rows][Cols] = {
            {"1", "2", "3"},
            {"4", "5", "6"},
            {"7", "8", "9"},
            {".", "0", "Enter"},
        };
        return (R >= 0 && R < Rows && C >= 0 && C < Cols) ? L[R][C] : "";
    }
    static bool IsEnter(int R, int C) { return R == 3 && C == 2; }

    // Pixel rect of key (R,C) inside the pad box (X,Y,W,H). A uniform gap of Gap px sits
    // between keys; the same call feeds the renderer and the hit-test.
    static void KeyRect(float X, float Y, float W, float H, float Gap, int R, int C,
                        float& Kx, float& Ky, float& Kw, float& Kh) {
        Kw = (W - Gap * (Cols - 1)) / Cols;
        Kh = (H - Gap * (Rows - 1)) / Rows;
        Kx = X + static_cast<float>(C) * (Kw + Gap);
        Ky = Y + static_cast<float>(R) * (Kh + Gap);
    }

    // A tap at (Px,Py). If it lands on a key, apply it (append a digit/'.', or arm Enter)
    // and return true; otherwise return false (the caller then tries other hit targets).
    bool Tap(float X, float Y, float W, float H, float Gap, float Px, float Py) {
        for (int R = 0; R < Rows; ++R)
            for (int C = 0; C < Cols; ++C) {
                float Kx, Ky, Kw, Kh;
                KeyRect(X, Y, W, H, Gap, R, C, Kx, Ky, Kw, Kh);
                if (Px >= Kx && Px <= Kx + Kw && Py >= Ky && Py <= Ky + Kh) {
                    Press(R, C);
                    return true;
                }
            }
        return false;
    }

    // Apply key (R,C) to the buffer (Enter arms the one-shot flag). Exposed for keyboard
    // callers / tests that address keys directly.
    void Press(int R, int C) {
        if (IsEnter(R, C)) { EnterPending_ = true; return; }
        const char* Lbl = Label(R, C);
        if (Lbl[0] == '.' && Buffer_.find('.') != std::string::npos) return;  // one dot only
        Buffer_ += Lbl;
    }

    void Backspace() { if (!Buffer_.empty()) Buffer_.pop_back(); }
    const std::string& Buffer() const { return Buffer_; }
    void SetBuffer(const std::string& S) { Buffer_ = S; }
    void Clear() { Buffer_.clear(); EnterPending_ = false; }
    bool TakeEnter() {  // one-shot: true once after Enter, then consumed
        const bool E = EnterPending_;
        EnterPending_ = false;
        return E;
    }

private:
    std::string Buffer_;
    bool        EnterPending_ = false;
};

}  // namespace Lur::DevGui
