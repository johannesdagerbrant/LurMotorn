#pragma once
#include <cstdint>

namespace Lur::Input {

enum class ETouchPhase : uint8_t { Began, Moved, Ended, Cancelled };

// One touch sample in pixels. Each platform's app glue (Android NativeActivity
// input queue, iOS UITouch/Metal view) normalizes its native events into this
// struct and feeds the game a single stream — so the game's input handling is
// written once. `TimeNs` is the device timestamp, used to keep input latency
// low and (later) to align inputs to sim ticks for rollback.
struct TouchEvent {
    ETouchPhase Phase;
    float    XPx;
    float    YPx;
    uint64_t TimeNs;
    uint32_t PointerId;  // distinguishes fingers for multi-touch
};

} // namespace Lur::Input
