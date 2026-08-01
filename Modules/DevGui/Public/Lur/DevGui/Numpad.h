#pragma once
// Lur::DevGui::Numpad — a self-contained numeric entry pad for the dev-GUI layer (#113),
// the console's only editor. It exists because raw NativeActivity can't raise the OS soft keyboard (#118
// no-go), and numeric CVars only need digits/./sign anyway: a tap-driven pad is the
// portable text-entry answer on phones AND desktop (mouse) with zero platform input glue.
//
// Layout:    1    2    3
//            4    5    6
//            7    8    9
//           +/-   0    .
//         [    E n t e r    ]
//
// `+/-` toggles the sign, which is what lets a negative value be entered AT ALL — before it,
// a tuner facing a knob that wanted -1 simply could not say so, on either platform.
//
// Enter spans the WHOLE bottom row: it is the key you hit most and the only one whose misfire
// costs you a retype, so it gets the biggest target on the pad. It occupies every column of
// its row — Label carries the text on the first cell only (so the renderer draws it once,
// across RowRect) while IsEnter is true for the entire row (so a tap anywhere along it
// commits). Those two must stay in agreement; that is why both key off EnterRow.
//
// Labels are ASCII on purpose (no ±): the MSDF atlas is cooked from the glyphs we ship, so a
// fancier codepoint is not guaranteed to be in it and would render as a hole.
//
// PURE logic + shared geometry: KeyRect is the single source for BOTH the renderer (draws
// each key there) and Tap (hit-tests there), so the two can never drift. It accumulates a
// value string as keys are pressed; Enter is a one-shot signal the caller drains to commit
// the buffer (e.g. CVar SetFromString). Reusable for any numeric dev field. Dev-only.
#include <cstring>
#include <string>

namespace Lur::DevGui {

class Numpad {
public:
    static constexpr int Rows = 5;
    static constexpr int Cols = 3;
    static constexpr int EnterRow = 4;  // the full-width bottom row

    static const char* Label(int R, int C) {
        // Enter's text sits on its first cell only; the rest of the row is empty so the
        // renderer doesn't stamp "Enter" three times across it.
        static const char* L[Rows][Cols] = {
            {"1", "2", "3"},
            {"4", "5", "6"},
            {"7", "8", "9"},
            {"+/-", "0", "."},
            {"Enter", "", ""},
        };
        return (R >= 0 && R < Rows && C >= 0 && C < Cols) ? L[R][C] : "";
    }
    // True for EVERY column of the bottom row — the whole strip is one key.
    static bool IsEnter(int R, int C) { return R == EnterRow && C >= 0 && C < Cols; }

    // Pixel rect of key (R,C) inside the pad box (X,Y,W,H). A uniform gap of Gap px sits
    // between keys; the same call feeds the renderer and the hit-test.
    static void KeyRect(float X, float Y, float W, float H, float Gap, int R, int C,
                        float& Kx, float& Ky, float& Kw, float& Kh) {
        Kw = (W - Gap * (Cols - 1)) / Cols;
        Kh = (H - Gap * (Rows - 1)) / Rows;
        Kx = X + static_cast<float>(C) * (Kw + Gap);
        Ky = Y + static_cast<float>(R) * (Kh + Gap);
    }

    // Full-width rect of row R — the renderer's answer for a key that spans its whole row
    // (Enter). Derived from KeyRect so a spanning key can never drift from the grid it sits in.
    static void RowRect(float X, float Y, float W, float H, float Gap, int R,
                        float& Kx, float& Ky, float& Kw, float& Kh) {
        KeyRect(X, Y, W, H, Gap, R, 0, Kx, Ky, Kw, Kh);
        Kw = W;  // span every column, gaps included
    }

    // A tap at (Px,Py). If it lands on a key, apply it (append a digit/'.', toggle the sign,
    // or arm Enter) and return true; otherwise return false (the caller then tries other hit
    // targets). Cell-by-cell even for Enter: its row's three cells all map to the same action,
    // so the spanning key needs no special case here.
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
    // callers / tests that address keys directly. Enter is tested FIRST, before the label is
    // even read: two of its three cells carry an empty label, and an empty label otherwise
    // means "inert".
    void Press(int R, int C) {
        if (IsEnter(R, C)) { EnterPending_ = true; return; }
        const char* Lbl = Label(R, C);
        if (Lbl[0] == '\0') return;
        if (std::strcmp(Lbl, "+/-") == 0) { ToggleSign(); return; }
        if (Lbl[0] == '.' && Buffer_.find('.') != std::string::npos) return;  // one dot only
        Buffer_ += Lbl;
    }

    // Flip the sign of the buffer. On an EMPTY buffer this leaves a lone "-", so the natural
    // "press +/-, then type the digits" order works; a lone "-" fails to parse, so a caller
    // committing it writes nothing rather than something surprising.
    void ToggleSign() {
        if (!Buffer_.empty() && Buffer_[0] == '-') Buffer_.erase(Buffer_.begin());
        else Buffer_.insert(Buffer_.begin(), '-');
    }

    // Type a character straight into the buffer (physical keyboard, #119) — the same effect
    // as tapping the matching on-screen key, so the two input paths can't produce different
    // buffers. Accepts '0'-'9', '.', and '-' (which toggles the sign, exactly as the +/- key
    // does — NOT a blind append, or "5-" would be reachable by keyboard and not by pad).
    // Returns false for anything else (including a second dot) so the caller can let the key
    // fall through.
    bool Press(char Ch) {
        if (Ch >= '0' && Ch <= '9') { Buffer_ += Ch; return true; }
        if (Ch == '-') { ToggleSign(); return true; }
        if (Ch == '.') {
            if (Buffer_.find('.') != std::string::npos) return false;  // one dot only
            Buffer_ += Ch;
            return true;
        }
        return false;
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
