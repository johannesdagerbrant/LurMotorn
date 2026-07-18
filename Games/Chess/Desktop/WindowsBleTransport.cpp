#include "WindowsBleTransport.h"

#include <cstdarg>
#include <cstdio>

#include <windows.h>

namespace Lur::DevRig {

// IPC framing (mirrors Tools/BleDevRig/BleRadio.cs):
//   Each frame: [1 byte tag][4 byte LE length][length bytes payload]
//   radio -> us: 'C' connected (payload=peer id), 'D' datagram in, 'X' disconnected
//   us -> radio: 'D' datagram out
static constexpr std::size_t kMaxFrame = 4096;  // BLE datagrams are tiny; a guard rail

WindowsBleTransport::WindowsBleTransport(std::string RadioExePath)
    : RadioExe(std::move(RadioExePath)) {}

WindowsBleTransport::~WindowsBleTransport() {
    Running = false;
    // Closing our end of the radio's stdin unblocks its stdin pump so it exits.
    if (ToRadio) { CloseHandle(static_cast<HANDLE>(ToRadio)); ToRadio = nullptr; }
    // Killing the child closes its pipe write end, so the reader's blocked ReadFile
    // returns EOF and the thread exits. Join BEFORE closing FromRadio, so the reader
    // never touches a handle we've already freed.
    if (ChildProcess) TerminateProcess(static_cast<HANDLE>(ChildProcess), 0);
    if (Reader.joinable()) Reader.join();
    if (FromRadio) { CloseHandle(static_cast<HANDLE>(FromRadio)); FromRadio = nullptr; }
    if (ChildProcess) { CloseHandle(static_cast<HANDLE>(ChildProcess)); ChildProcess = nullptr; }
}

void WindowsBleTransport::Log(const char* Fmt, ...) {
    if (!Logger) return;
    char Buf[512];
    va_list Args;
    va_start(Args, Fmt);
    std::vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);
    Logger(Buf);
}

bool WindowsBleTransport::Start() {
    SECURITY_ATTRIBUTES Sa{};
    Sa.nLength = sizeof(Sa);
    Sa.bInheritHandle = TRUE;  // pipe ends must be inheritable for the child

    HANDLE OutRead = nullptr, OutWrite = nullptr;  // radio stdout -> us
    HANDLE InRead = nullptr, InWrite = nullptr;    // us -> radio stdin
    if (!CreatePipe(&OutRead, &OutWrite, &Sa, 0) || !CreatePipe(&InRead, &InWrite, &Sa, 0)) {
        Log("BLE dev-rig: CreatePipe failed");
        return false;
    }
    // Our own ends must NOT be inherited by the child (else they never close).
    SetHandleInformation(OutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(InWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA Si{};
    Si.cb = sizeof(Si);
    Si.dwFlags = STARTF_USESTDHANDLES;
    Si.hStdInput  = InRead;
    Si.hStdOutput = OutWrite;
    Si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);  // radio logs pass through to our stderr

    // CreateProcess may write into the command line buffer, so it can't be const.
    std::string Cmd = "\"" + RadioExe + "\"";
    std::string CmdBuf = Cmd;

    PROCESS_INFORMATION Pi{};
    BOOL Ok = CreateProcessA(nullptr, CmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &Si, &Pi);
    // The child owns its ends now; close ours regardless of success.
    CloseHandle(OutWrite);
    CloseHandle(InRead);
    if (!Ok) {
        Log("BLE dev-rig: CreateProcess failed for '%s' (err=%lu)", RadioExe.c_str(), GetLastError());
        CloseHandle(OutRead);
        CloseHandle(InWrite);
        return false;
    }
    CloseHandle(Pi.hThread);
    ChildProcess = Pi.hProcess;
    FromRadio = OutRead;
    ToRadio = InWrite;

    Running = true;
    Reader = std::thread(&WindowsBleTransport::ReaderLoop, this);
    Log("BLE dev-rig: radio launched (%s) — scanning for the phone", RadioExe.c_str());
    return true;
}

bool WindowsBleTransport::ReadExact(void* Buf, std::size_t N) {
    auto* P = static_cast<uint8_t*>(Buf);
    std::size_t Off = 0;
    while (Off < N) {
        DWORD Got = 0;
        if (!ReadFile(static_cast<HANDLE>(FromRadio), P + Off, static_cast<DWORD>(N - Off), &Got, nullptr))
            return false;
        if (Got == 0) return false;  // pipe closed (radio exited)
        Off += Got;
    }
    return true;
}

void WindowsBleTransport::ReaderLoop() {
    uint8_t Hdr[5];
    uint8_t Payload[kMaxFrame];
    while (Running) {
        if (!ReadExact(Hdr, sizeof(Hdr))) break;
        const char Tag = static_cast<char>(Hdr[0]);
        const std::size_t Len = static_cast<std::size_t>(Hdr[1]) | (static_cast<std::size_t>(Hdr[2]) << 8) |
                                (static_cast<std::size_t>(Hdr[3]) << 16) | (static_cast<std::size_t>(Hdr[4]) << 24);
        if (Len > kMaxFrame) { Log("BLE dev-rig: oversize frame (%zu) — desync, stopping", Len); break; }
        if (Len > 0 && !ReadExact(Payload, Len)) break;

        switch (Tag) {
            case 'C':  // link live: payload is the peer's persistent device id
                PeerId.assign(reinterpret_cast<char*>(Payload), Len);
                Log("BLE dev-rig: linked to peer %s", PeerId.c_str());
                Inbox.PushConnected();
                break;
            case 'D':  // datagram from the peer
                Inbox.PushDatagram(Payload, Len);
                break;
            case 'X':  // link dropped
                Log("BLE dev-rig: link lost");
                Inbox.PushDisconnected();
                break;
            default:
                Log("BLE dev-rig: unknown frame tag 0x%02X", static_cast<unsigned>(Hdr[0]));
                break;
        }
    }
    // Radio pipe closed: reflect the link as down for the engine.
    if (Running) Inbox.PushDisconnected();
}

bool WindowsBleTransport::WriteFrame(char Tag, const uint8_t* Data, std::size_t Size) {
    if (!ToRadio) return false;
    uint8_t Hdr[5];
    Hdr[0] = static_cast<uint8_t>(Tag);
    Hdr[1] = static_cast<uint8_t>(Size & 0xFF);
    Hdr[2] = static_cast<uint8_t>((Size >> 8) & 0xFF);
    Hdr[3] = static_cast<uint8_t>((Size >> 16) & 0xFF);
    Hdr[4] = static_cast<uint8_t>((Size >> 24) & 0xFF);

    std::lock_guard<std::mutex> Lock(WriteMutex);
    DWORD Wrote = 0;
    if (!WriteFile(static_cast<HANDLE>(ToRadio), Hdr, sizeof(Hdr), &Wrote, nullptr) || Wrote != sizeof(Hdr))
        return false;
    if (Size > 0) {
        if (!WriteFile(static_cast<HANDLE>(ToRadio), Data, static_cast<DWORD>(Size), &Wrote, nullptr) ||
            Wrote != Size)
            return false;
    }
    return true;
}

void WindowsBleTransport::Send(const uint8_t* Data, std::size_t Size) {
    if (!Connected) return;  // no link: drop, matching the BLE backends
    if (Size > kMaxFrame) { Log("BLE dev-rig: datagram too large (%zu) — dropped", Size); return; }
    if (!WriteFrame('D', Data, Size)) Log("BLE dev-rig: failed to write datagram to radio");
}

} // namespace Lur::DevRig
