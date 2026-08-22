#pragma once
#include <vector>

#include "Lur/Sim/SnapshotRing.h"
#include "Rps/Sim.h"

namespace Rps {

// The rollback restore buffer. Since #201 the ring itself is Lur::Sim::SnapshotRing — there was zero
// RPS content in it, only `Sim` where a template parameter belonged.
using SnapshotRing = Lur::Sim::SnapshotRing<Sim>;

// The peer-input predictor. Rollback speculates the ticks for which the peer's real frame has not yet
// arrived, and the prediction is "the peer produced NO input events this tick". It is right for the
// large majority of ticks because human taps are only a few per second against a 10 Hz sim, so most
// speculated ticks match the real frame when it lands and roll back to a no-op. Trivial by design, and
// named so the emptiness of the prediction is a property a test can pin.
//
// NOT promoted: "predict nothing" is a game's judgement about its own input, not an engine law. A
// physics game with continuous steering will predict "repeat the last input" instead, and that is the
// right place for the two to differ.
inline void PredictPeerBatch(std::vector<InputEvent>& Out) { Out.clear(); }

}  // namespace Rps
