#pragma once

namespace Lur {

// The build's identity: git commit + dirty flag + LUR_CONFIG, e.g. "9bf59f4c1a2b-dirty+Development".
// Two peers exchange it at connect and refuse the match on a mismatch (#112) — the proactive form of
// the reactive anchor-hash desync alarm, and what makes the 1-byte gameplay-CVar ids safe to agree
// on (identical builds => identical CVar list).
//
// A FUNCTION, not a macro, and that is the whole point (#164). The value used to be a compile
// definition baked at CMake configure time, which cannot answer the question it is asked: Ninja and
// Gradle reconfigure only when a CMakeLists.txt changes, so `commit; installDebug` shipped a binary
// stamped with the PREVIOUS commit and two phones built from identical source reported a mismatch.
// The definition now lives in a one-line TU regenerated per build by cmake/BuildFingerprint.cmake,
// so it describes the binary it is actually inside — and a change relinks that object instead of
// recompiling every consumer.
//
// Never empty; degrades to "no-git+<config>" where git isn't available (CI tarball, exported source).
const char* BuildFingerprint();

}  // namespace Lur
