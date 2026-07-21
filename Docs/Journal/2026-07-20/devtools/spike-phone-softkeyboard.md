# Spike Plan — Phone Soft-Keyboard Text Entry (dev console)

*2026-07-20. A timeboxed investigation, not a feature. Deliverable = **knowledge + a go/no-go**, not shippable code. This is the one open item in the dev-console/CVar specs (`dev-console-cvar-tech-spec.md` §4.2) whose *approach* is uncertain — raising the OS soft keyboard in a native-code app and getting typed characters back into C++ — so it must be proven on hardware before the real feature is spec'd. All of it is `#if !LUR_SHIPPING`; nothing here ships.*

## 1. The question (and only this)

**Can the on-screen soft keyboard be raised on demand in our native app, and can the characters it produces reach a C++ buffer — on Android (`NativeActivity`) and iOS (Metal/native, no UIKit text UI)?**

Not in scope for the spike: console integration, completion, editing niceties, the three-finger open gesture, layout. The spike answers *feasibility and the API path*; the real implementation (a later task) does the rest.

## 2. Why it's uncertain (the risk being burned down)

- **Android:** the app is a `NativeActivity` — native code with no ordinary Android view tree. `InputMethodManager.showSoftInput` frequently **silently no-ops** without a focused, IME-eligible `View`, and even when the keyboard shows, delivering the typed text *back* into native code isn't automatic — it typically needs a Kotlin/JNI shim (a hidden `EditText` or an `InputConnection`), mirroring the BLE shim pattern already in the repo.
- **iOS:** a Metal/native app has no `UITextField` on screen. Text entry usually means a **hidden first-responder** (`UITextField` or a `UIKeyInput`-conforming view) added to the view, made first responder to raise the keyboard, with its callbacks forwarded into C++.
- Both are well-trodden by other native-app/game developers, but "known to be possible" ≠ "known to work in *our* setup" until tried. That gap is the whole reason for a spike.

## 3. Timebox

**Up to 2 days total** (roughly 1 per platform). Hard stop. If a platform isn't working at its budget, that platform's result is "no-go for now" → fallback (§6). Do **not** extend the box; extending is how a spike becomes a swamp.

## 4. What to try (per platform)

### Android (`NativeActivity`)
Attempt, simplest first, stop at the first that works:
1. A minimal Kotlin shim on the activity: add an **invisible `EditText`** (0-size or off-screen) to the window's content, `requestFocus()` + `InputMethodManager.showSoftInput`; forward its `TextWatcher`/`onKey` output to C++ via a JNI callback (the `nativeOnReceived`-style pattern the BLE shim already uses). Hide/`hideSoftInputFromWindow` to dismiss.
2. If the invisible-EditText path is flaky: a custom `View` returning a real `InputConnection` from `onCreateInputConnection` (commit-text → JNI). More control, more code.
3. Note the `android:windowSoftInputMode` and any focus/window-flag requirements discovered.

### iOS (Metal/native)
1. Add a **hidden `UITextField`** to the root view; `becomeFirstResponder()` to raise the keyboard; forward `editingChanged`/delegate text into C++; `resignFirstResponder()` to dismiss.
2. If a full `UITextField` is heavier than wanted: a minimal **`UIKeyInput`-conforming `UIView`** (implement `insertText:`/`deleteBackward:`, `hasText`) made first responder — lighter, gives raw key events.
3. Note keyboard-frame/safe-area interactions (the keyboard covers part of the screen — the real console must reposition above it, but the spike only *notes* the frame notification exists).

## 5. Success criterion (binary, per platform)

> Tapping a key on the OS soft keyboard appends the corresponding character to a `std::string` in C++, visible via a log line (or drawn through the existing dev text renderer). Backspace deletes one. The keyboard can be raised and dismissed on command.

That's it. One char in → one char in the buffer → logged. No console, no editing model.

## 6. Decision gate & fallbacks

At the timebox, each platform is **go** or **no-go**:

- **Both go** → write the real soft-keyboard implementation as a normal task, using the proven API path; the console gets full text entry on phones. Fold the findings (exact APIs, shim shape, gotchas) into `dev-console-cvar-tech-spec.md` §4.2, replacing the "spike" note.
- **One/both no-go (or over budget)** → ship the console on phones with the **fallback already allowed by the spec**: **tap-to-select from the completion list only**, no free text entry. Combined with CVar sliders, the color picker, and command buttons in the desktop/panel surfaces, this covers most on-device dev needs without typing. Free text stays a desktop-first capability (where the hardware keyboard is trivial, §4.1). Record the no-go and revisit if a real need forces it.
- **Partial (e.g. shows but no text back)** → treat as no-go for text entry; the keyboard-raise alone is useless without delivery.

## 7. Deliverable

A short findings note (a paragraph per platform) appended to `dev-console-cvar-tech-spec.md` §4.2: what worked, the exact API path, the shim/first-responder shape, gotchas (focus, window flags, keyboard frame), and the go/no-go. Plus the throwaway spike branch, tagged and abandoned (not merged — the real implementation is written clean from the learnings).

## 8. Guardrails

- Throwaway code — resist polishing it into the real thing; the real thing is written fresh with the risk gone.
- Desktop text entry (§4.1) is **not** part of this and is not blocked by it — the console is fully usable on desktop (hardware keyboard) regardless of how this spike lands. So this never gates the primary tuning workflow.
- Purely dev-only; no shipping surface touched.
