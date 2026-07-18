#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Lur/Transport/EventInbox.h"
#include "Lur/Transport/Transport.h"

namespace Lur::DevRig {

// A THIRD radio for the Workbench: a real BLE central, on Windows, under a debugger.
//
// Loopback has no radio and two phones have no debugger; this transport is both. It
// speaks the engine's existing wire protocol over real Bluetooth to the UNMODIFIED
// Android app, so a desktop chess instance becomes a live BLE peer — a Hello
// handshake and moves over the air, with Lur::Log + the flight recorder watching.
//
// Windows' WinRT BLE API can't be consumed from the MinGW C++ toolchain, so the
// radio itself is a small C# subprocess (Tools/BleDevRig/BleRadio.exe) and this
// class is the bridge: it spawns the radio, pipes datagrams to/from it, and lifts
// the radio's callback-thread events onto the engine thread via EventInbox (issue
// #40) exactly like the Android/iOS backends. Central-ONLY on purpose — Windows'
// peripheral mode is flaky and the phone already plays peripheral.
//
// It is a dev instrument, NOT shipped engine, which is why it lives in the Desktop
// app build (like AndroidBleTransport lives in the Android app) rather than in the
// cross-platform Modules/Transport. When BLE unification lands (Phase 5) it becomes
// the third IBleRadio driver; built behind ITransport so that migration is cheap.
class WindowsBleTransport : public Lur::Transport::ITransport,
                            private Lur::Transport::EventInbox::Sink {
public:
    // RadioExePath is the C# radio subprocess (Tools/BleDevRig/BleRadio.exe).
    explicit WindowsBleTransport(std::string RadioExePath);
    ~WindowsBleTransport() override;

    // Spawn the radio subprocess + start the reader thread. Returns false if the
    // subprocess couldn't be launched (e.g. the exe path is wrong).
    bool Start();

    void SetLogger(std::function<void(const char*)> L) { Logger = std::move(L); }

    // The peer's persistent device id, learned when the radio announces the link.
    const std::string& GetPeerId() const { return PeerId; }

    // --- ITransport ---
    void Send(const uint8_t* Data, std::size_t Size) override;
    void SetReceiver(Lur::Transport::ITransport::Receiver NewReceiver) override {
        ReceiverFn = std::move(NewReceiver);
    }
    bool IsConnected() const override { return Connected; }
    void Pump() override { Inbox.Drain(*this); }  // engine thread: fire events in order

private:
    // --- EventInbox::Sink (engine thread only, via Pump/Drain) ---
    void OnConnected() override    { Connected = true; }
    void OnDisconnected() override { Connected = false; }
    void OnDatagram(const uint8_t* Data, std::size_t Size) override {
        if (ReceiverFn) ReceiverFn(Data, Size);
    }

    void ReaderLoop();                 // radio-stdout thread: frames -> Inbox
    void Log(const char* Fmt, ...);
    bool WriteFrame(char Tag, const uint8_t* Data, std::size_t Size);
    bool ReadExact(void* Buf, std::size_t N);

    std::string RadioExe;
    std::function<void(const char*)> Logger;
    Lur::Transport::ITransport::Receiver ReceiverFn;

    Lur::Transport::EventInbox Inbox;
    std::atomic<bool>          Connected{false};  // only mutated on engine thread
    std::string                PeerId;            // set on the reader thread pre-Connected

    std::thread       Reader;
    std::atomic<bool> Running{false};
    std::mutex        WriteMutex;                 // serialise stdin writes to the radio

    // Win32 handles kept as void* so this header stays windows.h-free for includers.
    void* ChildProcess = nullptr;  // HANDLE
    void* ToRadio      = nullptr;  // HANDLE: our write end of the radio's stdin
    void* FromRadio    = nullptr;  // HANDLE: our read end of the radio's stdout
};

} // namespace Lur::DevRig
