#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Lur::Transport {

// Thread-safe hand-off from a radio callback thread to the engine thread.
//
// The ITransport contract says the receiver fires ON THE ENGINE THREAD, but a BLE
// backend's callbacks arrive on whatever thread the OS radio stack uses (Android
// Binder threads; a CoreBluetooth dispatch queue). Delivering straight from there
// races the engine thread's Tick()/Render() over Session + game state (issue #40).
//
// A backend Push*()es events here from its callback thread; the engine Drain()s them
// once per frame (from Session::Tick via ITransport::Pump). Connect, disconnect, and
// datagram events share ONE queue, so their arrival ORDER is preserved — a disconnect
// can never overtake the last datagram before it, and IsConnected only changes on the
// engine thread. Fixed-capacity ring: zero steady-state allocation. This is also the
// event front door the future shared BleLinkController is built on.
//
// Overflow (engine stalled far longer than the radio) drops the OLDEST event and sets
// a sticky flag rather than blocking the radio thread — at chess/RTS datagram rates it
// should never fire.
class EventInbox {
public:
    enum class EKind : uint8_t { Connected, Disconnected, Datagram };

    // The engine-thread consumer. Drain() dispatches queued events into this in order.
    struct Sink {
        virtual ~Sink() = default;
        virtual void OnConnected() = 0;
        virtual void OnDisconnected() = 0;
        virtual void OnDatagram(const uint8_t* Data, std::size_t Size) = 0;
    };

    // --- Producer side (radio callback thread), all thread-safe. ---
    void PushConnected()    { Push(EKind::Connected, nullptr, 0); }
    void PushDisconnected() { Push(EKind::Disconnected, nullptr, 0); }
    void PushDatagram(const uint8_t* Data, std::size_t Size) { Push(EKind::Datagram, Data, Size); }

    // --- Consumer side (engine thread). Drain every queued event, in FIFO order. ---
    void Drain(Sink& Out) {
        for (;;) {
            Event E;
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                if (Count == 0) break;
                E = Buffer[Head];
                Head = (Head + 1) % Capacity;
                --Count;
            }
            // Dispatch OUTSIDE the lock: the sink runs game logic and may call back in.
            switch (E.Kind) {
                case EKind::Connected:    Out.OnConnected(); break;
                case EKind::Disconnected: Out.OnDisconnected(); break;
                case EKind::Datagram:     Out.OnDatagram(E.Data, E.Size); break;
            }
        }
    }

    bool Overflowed() const { std::lock_guard<std::mutex> Lock(Mutex); return DidOverflow; }

private:
    static constexpr std::size_t Capacity    = 32;
    static constexpr std::size_t MaxDatagram = 256;  // >= max framed datagram (1 type + 254 payload)

    struct Event {
        EKind       Kind = EKind::Datagram;
        std::size_t Size = 0;
        uint8_t     Data[MaxDatagram] = {};
    };

    void Push(EKind Kind, const uint8_t* Data, std::size_t Size) {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (Size > MaxDatagram) { DidOverflow = true; return; }  // never truncate a datagram
        if (Count == Capacity) {                                 // full: drop the oldest
            Head = (Head + 1) % Capacity;
            --Count;
            DidOverflow = true;
        }
        Event& E = Buffer[Tail];
        E.Kind = Kind;
        E.Size = Size;
        for (std::size_t i = 0; i < Size; ++i) E.Data[i] = Data[i];
        Tail = (Tail + 1) % Capacity;
        ++Count;
    }

    mutable std::mutex Mutex;
    Event       Buffer[Capacity];
    std::size_t Head = 0;
    std::size_t Tail = 0;
    std::size_t Count = 0;
    bool        DidOverflow = false;
};

} // namespace Lur::Transport
