#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Lur::Save {

// A tiny, dependency-free key -> blob store on the local filesystem. This is the
// engine's persistence primitive (issue #17, Phase A): the device-identity GUID,
// and later the per-opponent save records, are just blobs it reads and writes.
//
// Pure C++ std::filesystem so the SAME code compiles on host, Android (NDK), and
// iOS. The app supplies the platform save directory (Android filesDir, iOS
// Application Support); the store is otherwise platform-free.
//
// Not thread-safe: drive it from one thread (the engine thread), like the rest of
// the core. Keys are arbitrary UTF-8; the store maps each to one file, sanitising
// unsafe bytes so a GUID or an opponent id can never escape the directory.
class Store {
public:
    // Directory is created lazily on the first Save; it need not exist yet.
    explicit Store(std::string Directory);

    // The blob previously stored under Key, or an EMPTY vector if the key is
    // absent (or was stored empty, or could not be read — all indistinguishable
    // and all "nothing here yet", which is exactly what callers want).
    std::vector<uint8_t> Load(std::string_view Key) const;

    // Write Size bytes under Key, replacing any prior value. ATOMIC: the bytes go
    // to a temp file that is then renamed over the target, so a crash mid-write
    // can never leave a half-written record — a reader sees either the old blob or
    // the new one, never a torn one. Returns false on I/O failure.
    bool Save(std::string_view Key, const uint8_t* Data, std::size_t Size);

private:
    // Map a key to its on-disk filename, escaping any byte outside [A-Za-z0-9._-]
    // as %XX. Reversible and collision-free, so distinct keys never share a file
    // and no key can contain a path separator that escapes the directory.
    std::string PathFor(std::string_view Key) const;

    std::string Dir;
};

} // namespace Lur::Save
