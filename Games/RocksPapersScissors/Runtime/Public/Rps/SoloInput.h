#pragma once
#include "Lur/Sim/InputInbox.h"
#include "Rps/Sim.h"  // InputEvent, MaxEventsPerTick

namespace Rps {

// Thread-safe hand-off of the solo human's place/queue EVENTS from the input/render thread to the
// SimRunner's sim thread. Since #201 the mechanism is Lur::Sim::InputInbox — the SAME inbox
// LockstepPeer now uses for its local events, so the solo and linked input paths can no longer
// disagree about ordering or about what a full inbox does.
//
// Capacity: four ticks' worth of events. A human issues at most a few taps per 100 ms tick, so this is
// far above the real rate; what it buys is a bounded, allocation-free input path with an observable
// drop instead of an unbounded queue that fails somewhere else.
using SoloInputInbox = Lur::Sim::InputInbox<InputEvent, MaxEventsPerTick * 4>;

}  // namespace Rps
