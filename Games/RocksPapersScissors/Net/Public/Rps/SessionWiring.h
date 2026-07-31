#pragma once
#include <cstddef>
#include <cstdint>

#include "Lur/Net/Session.h"
#include "Rps/LockstepPeer.h"

namespace Rps {

// ONE definition of "which datagram types the RTS listens to", because it was five (the Android,
// iOS and two desktop mains, plus the test harness) and a message set copied five times is a message
// set that goes out of sync. Every copy did the identical thing — forward the type to
// LockstepPeer::OnMessage — so the duplication bought nothing and cost coverage:
//
//   * #147/#112: Session's handler table was bounded one slot too low, so MsgCvar/MsgCvarSync/
//     MsgFingerprint were dropped at registration AND dispatch in silence. Every direct-OnMessage
//     test passed while the feature was completely dead on the wire.
//   * #160 adds MsgCamp. Registering it in three mains out of four would present as "the match never
//     starts on the iPhone" — a symptom nowhere near its cause.
//
// An unregistered slot fails SILENTLY at both ends, which is exactly the failure mode that deserves
// a single definition. Call this instead of hand-registering; the test harness calls it too, so the
// composition under test is the composition that ships (the lesson of #147).
//
// Not a Session method and not in Modules/Net: the message SET is a game concept, and Modules must
// never depend on Games (the CMake-enforced wall). The lifetime rule is the obvious one — Lp must
// outlive S, which is already true wherever a session drives a peer.
inline void RouteSessionToPeer(Lur::Net::Session& S, LockstepPeer& Lp) {
    const auto Route = [&S, &Lp](Lur::Net::EMsgType T) {
        S.SetHandler(T, [&Lp, T](const uint8_t* D, std::size_t N) { Lp.OnMessage(T, D, N); });
    };
    Route(MsgInput);
    Route(MsgAnchor);
    Route(MsgResyncChunk);
    Route(MsgCamp);  // #160: the pre-match opening camp, on its own slot rather than inside MsgInput
#if LUR_INTERNAL
    // Dev-only slots. They are part of the composition a dev build runs, so they belong here rather
    // than in each main — a phone playtest exercises the cvar sync and the build-fingerprint gate.
    Route(MsgCvar);
    Route(MsgCvarSync);
    Route(MsgFingerprint);
#endif
}

}  // namespace Rps
