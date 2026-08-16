#include "Lur/Save/Store.h"

// NO <filesystem>, NO <fstream> (issue #43, Phase 3 section F). This was the last shipping use of
// either — everything else that touches disk in this tree (FlightRecorder, MatchRecord, ScoreBook)
// already went through std::fopen, so Store was the odd one out rather than the norm.
//
// Three reasons it was worth the ~70 lines below:
//   * <filesystem> is the single largest stdlib dependency the app pulls in, for four operations.
//   * Its error-reporting API is exceptions-by-default; the std::error_code overloads used here are
//     the opt-out, and every call site had to remember to take it. One that forgets throws on a
//     phone, from a save path, which is the worst place to learn about it. `-fno-exceptions` is the
//     next item and this is its precondition.
//   * On Apple platforms <filesystem> is what pins the iOS deployment floor at 13.
//
// The behaviour below is deliberately identical to what it replaces, including the quirks: Load
// returns empty on any failure (absent, empty, short read) rather than distinguishing them, and Save
// falls back to remove-then-rename for host CRTs that will not clobber.
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
// Guarded: MinGW's UCRT toolchain predefines NOMINMAX, and -Werror turns a plain redefinition into a
// build failure.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace Lur::Save {

namespace {

// Join a directory and a leaf with '/'. Windows accepts a forward slash everywhere we open files, so
// there is no need to care which separator the platform prefers — and the result is never persisted,
// only handed straight to fopen/rename.
std::string JoinPath(const std::string& Dir, const std::string& Leaf) {
    if (Dir.empty()) return Leaf;   // matches what fs::path("") / Leaf produced
    const char Last = Dir.back();
    if (Last == '/' || Last == '\\') return Dir + Leaf;
    return Dir + '/' + Leaf;
}

// One directory level. True if it exists afterwards, however it got there — an existing directory is
// success, which is what create_directories meant.
bool MakeOneDir(const std::string& Path) {
#if defined(_WIN32)
    if (_mkdir(Path.c_str()) == 0) return true;
#else
    if (::mkdir(Path.c_str(), 0777) == 0) return true;
#endif
    return errno == EEXIST;
}

// Recursive mkdir -p. Walks the separators and creates each prefix in turn.
//
// The two skips matter. An empty first component means the path was absolute ("/data/..."), and
// mkdir("") always fails; a component ending in ':' is a Windows drive ("C:"), which cannot be
// created and must not be treated as a failure. Getting either wrong turns "save to an absolute
// path" — i.e. every real device path — into a silent no-op.
bool MakeDirs(const std::string& Path) {
    if (Path.empty()) return false;
    for (std::size_t I = 1; I <= Path.size(); ++I) {
        const bool AtEnd = (I == Path.size());
        if (!AtEnd && Path[I] != '/' && Path[I] != '\\') continue;
        const std::string Prefix = Path.substr(0, I);
        if (Prefix.empty()) continue;                    // leading '/' — the root already exists
        if (Prefix.back() == ':') continue;              // "C:" — a drive, not a directory
        if (!MakeOneDir(Prefix) && AtEnd) return false;  // only the leaf's failure is fatal
    }
    return true;
}

// Every regular file directly in Dir (no recursion, no "." / ".."). Empty on an absent or unreadable
// directory, which is the "nothing saved yet" case and not an error.
std::vector<std::string> ListRegularFiles(const std::string& Dir) {
    std::vector<std::string> Names;
#if defined(_WIN32)
    WIN32_FIND_DATAA Fd;
    const HANDLE H = FindFirstFileA(JoinPath(Dir, "*").c_str(), &Fd);
    if (H == INVALID_HANDLE_VALUE) return Names;
    do {
        if (Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Names.emplace_back(Fd.cFileName);
    } while (FindNextFileA(H, &Fd));
    FindClose(H);
#else
    DIR* D = ::opendir(Dir.c_str());
    if (D == nullptr) return Names;
    while (const dirent* E = ::readdir(D)) {
        // d_type is NOT reliable — several filesystems report DT_UNKNOWN — so stat rather than
        // trust it. A directory silently listed as a key would come back as an empty Load.
        struct stat St;
        if (::stat(JoinPath(Dir, E->d_name).c_str(), &St) != 0) continue;
        if (!S_ISREG(St.st_mode)) continue;
        Names.emplace_back(E->d_name);
    }
    ::closedir(D);
#endif
    return Names;
}

// A hex nibble 0..15, or -1 if C is not a hex digit. PathFor emits uppercase, but
// accept both cases so the decode is robust.
int HexNibble(char C) {
    if (C >= '0' && C <= '9') return C - '0';
    if (C >= 'A' && C <= 'F') return C - 'A' + 10;
    if (C >= 'a' && C <= 'f') return C - 'a' + 10;
    return -1;
}

}  // namespace

Store::Store(std::string Directory) : Dir(std::move(Directory)) {}

std::string Store::PathFor(std::string_view Key) const {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string Name;
    Name.reserve(Key.size());
    for (unsigned char C : Key) {
        const bool Safe = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
                          (C >= '0' && C <= '9') || C == '.' || C == '-' || C == '_';
        if (Safe) {
            Name.push_back(static_cast<char>(C));
        } else {
            Name.push_back('%');
            Name.push_back(Hex[C >> 4]);
            Name.push_back(Hex[C & 0x0F]);
        }
    }
    return JoinPath(Dir, Name);
}

std::vector<uint8_t> Store::Load(std::string_view Key) const {
    std::FILE* In = std::fopen(PathFor(Key).c_str(), "rb");
    if (In == nullptr) return {};

    std::vector<uint8_t> Buffer;
    if (std::fseek(In, 0, SEEK_END) == 0) {
        const long Size = std::ftell(In);
        if (Size > 0 && std::fseek(In, 0, SEEK_SET) == 0) {
            Buffer.resize(static_cast<std::size_t>(Size));
            // A short read is a failure, not a truncated record: a half-read save merged as if
            // complete is how a corrupt file becomes the new state of record.
            if (std::fread(Buffer.data(), 1, Buffer.size(), In) != Buffer.size()) Buffer.clear();
        }
    }
    std::fclose(In);
    return Buffer;
}

std::vector<std::string> Store::ListKeys() const {
    std::vector<std::string> Keys;
    for (const std::string& Name : ListRegularFiles(Dir)) {
        // Skip the transient temp files an interrupted Save() may have left behind.
        if (Name.size() >= 4 && Name.compare(Name.size() - 4, 4, ".tmp") == 0) continue;

        // Reverse PathFor: literal bytes pass through; %XX decodes back to one byte.
        std::string Key;
        Key.reserve(Name.size());
        for (std::size_t i = 0; i < Name.size(); ++i) {
            const int Hi = (Name[i] == '%' && i + 2 < Name.size()) ? HexNibble(Name[i + 1]) : -1;
            const int Lo = (Hi >= 0) ? HexNibble(Name[i + 2]) : -1;
            if (Hi >= 0 && Lo >= 0) {
                Key.push_back(static_cast<char>((Hi << 4) | Lo));
                i += 2;
            } else {
                Key.push_back(Name[i]);
            }
        }
        Keys.push_back(std::move(Key));
    }
    return Keys;
}

bool Store::Save(std::string_view Key, const uint8_t* Data, std::size_t Size) {
    MakeDirs(Dir);  // ok if it already exists

    const std::string Final = PathFor(Key);
    const std::string Temp  = Final + ".tmp";

    // Write the whole blob to the temp file first, then close it — Windows will
    // not rename a file that is still open.
    {
        std::FILE* Out = std::fopen(Temp.c_str(), "wb");
        if (Out == nullptr) return false;
        const bool Wrote = (Size == 0) || (std::fwrite(Data, 1, Size, Out) == Size);
        // fclose can fail where fwrite did not — buffered bytes are only forced out here, so a full
        // disk surfaces at close. Ignoring it would report a save that never landed.
        const bool Closed = (std::fclose(Out) == 0);
        if (!Wrote || !Closed) {
            std::remove(Temp.c_str());
            return false;
        }
    }

    // POSIX rename (Android/iOS, the real targets) atomically replaces an existing
    // file — the crash-safe path. Some host CRTs (Windows/MinGW) refuse to
    // overwrite, so fall back to remove-then-rename there: not atomic, but the host
    // only runs the unit tests, where it just needs to succeed.
    if (std::rename(Temp.c_str(), Final.c_str()) != 0) {
        std::remove(Final.c_str());
        if (std::rename(Temp.c_str(), Final.c_str()) != 0) {
            std::remove(Temp.c_str());
            return false;
        }
    }
    return true;
}

} // namespace Lur::Save
