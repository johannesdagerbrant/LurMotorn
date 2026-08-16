#pragma once
#include <cstdint>

namespace Lur::Input {

enum class ETouchPhase : uint8_t { Began, Moved, Ended, Cancelled };

// One touch sample in pixels. Each platform's app glue (Android NativeActivity input queue, iOS
// UITouch/Metal view, the desktop window) normalizes its native events into this struct and feeds
// the game a single stream — so the game's input handling is written once.
//
// HONEST STATUS (#43 section D): that "written once" is still only true on DESKTOP. Both phones
// bypass this type today and read their native events directly, which is what this section exists
// to fix. Do not read the comment above as a description of the current wiring.
//
// `TimeNs` is the device timestamp taken at the touch, NOT when the event was dequeued. The
// distinction is load-bearing: RPS packages touches on the UIKit thread and replays them on the
// render thread up to a frame later, so a timestamp taken at replay would stretch the console
// gesture's hold/chain windows by a frame each.
struct TouchEvent {
    ETouchPhase Phase;
    float    XPx;
    float    YPx;
    uint64_t TimeNs;
    // How many pointers are down at this sample — NOT which finger this is.
    //
    // It replaced a `PointerId` field that had no reader anywhere and was hard-coded to 0 by its one
    // writer, so "distinguishes fingers for multi-touch" described an intention rather than a
    // behaviour. What the games actually need is the COUNT: Lur::Input::ConsoleGesture opens the dev
    // console on a two-finger triple-tap, and both RPS mains were carrying this fact alongside the
    // event because the engine type could not express it. Identity can come back when something
    // needs it — git remembers the field.
    int PointerCount;
};

} // namespace Lur::Input
