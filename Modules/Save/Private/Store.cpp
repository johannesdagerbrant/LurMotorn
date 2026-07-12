#include "Lur/Save/Store.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace Lur::Save {

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
    return (fs::path(Dir) / Name).string();
}

std::vector<uint8_t> Store::Load(std::string_view Key) const {
    std::ifstream In(PathFor(Key), std::ios::binary | std::ios::ate);
    if (!In) return {};
    const std::streamoff Size = In.tellg();
    if (Size <= 0) return {};

    std::vector<uint8_t> Buffer(static_cast<std::size_t>(Size));
    In.seekg(0);
    In.read(reinterpret_cast<char*>(Buffer.data()), Size);
    if (!In) return {};
    return Buffer;
}

bool Store::Save(std::string_view Key, const uint8_t* Data, std::size_t Size) {
    std::error_code Ec;
    fs::create_directories(Dir, Ec);  // ok if it already exists

    const std::string Final = PathFor(Key);
    const std::string Temp  = Final + ".tmp";

    // Write the whole blob to the temp file first, then close it — Windows will
    // not rename a file that is still open.
    {
        std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
        if (!Out) return false;
        if (Size > 0) Out.write(reinterpret_cast<const char*>(Data), static_cast<std::streamsize>(Size));
        Out.flush();
        if (!Out) return false;
    }

    // POSIX rename (Android/iOS, the real targets) atomically replaces an existing
    // file — the crash-safe path. Some host CRTs (Windows/MinGW) refuse to
    // overwrite, so fall back to remove-then-rename there: not atomic, but the host
    // only runs the unit tests, where it just needs to succeed.
    fs::rename(Temp, Final, Ec);
    if (Ec) {
        std::error_code Ec2;
        fs::remove(Final, Ec2);
        fs::rename(Temp, Final, Ec2);
        if (Ec2) {
            fs::remove(Temp, Ec2);
            return false;
        }
    }
    return true;
}

} // namespace Lur::Save
