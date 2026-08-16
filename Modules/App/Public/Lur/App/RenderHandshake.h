#pragma once
#include <atomic>

// Who owns the renderer, and how the platform thread talks to it (issue #43, Phase 3 section C).
//
// Every app in this tree has a platform thread it does not control: UIKit's main thread, Android's
// looper. That thread learns things the renderer must act on — the drawable resized, the window was
// torn off and rebuilt (#73), the app went to background — but it is NOT allowed to touch the
// renderer when something else owns it. RPS's render loop runs on its own pthread (#183), so every
// one of those events crosses a thread boundary; chess renders on the platform thread, so none of
// them do.
//
// That difference is why the same event is written two ways today:
//
//     chess iOS   _Renderer->Resize(W, H);                        // inline, main thread
//     RPS iOS     _ResizeReq.store(true, std::memory_order_release);   // deferred; render thread acts
//
// Both are correct FOR THEIR TOPOLOGY and neither can be pasted into the other, which is what makes
// the resize/pause/reattach handling unabsorbable while the topology is implicit. This class makes it
// explicit: one call, `RequestResize`, whose meaning is configured once at startup. That is the whole
// point of section C — not to give chess a render thread, but to stop the question "is there one?"
// from being re-answered at every call site.
//
// THE DECISION THIS EXISTS TO OWN. Parking the renderer differs between the two topologies in a way
// that is not a detail:
//
//     Dedicated   main sets the request, then WAITS for the render thread's ack. Skipping the wait
//                 means main frees a CAMetalLayer the render thread is still drawing into.
//     Inline      main IS the render thread. The same wait is a DEADLOCK — waiting on an ack that
//                 only this thread can give.
//
// So the naive unification ("just always wait") hangs chess on launch, and the other naive one
// ("never wait") is a use-after-free on RPS. `IsParked()` below is that decision, made once, with a
// test for each topology. Everything else here is bookkeeping.
//
// Pure C++ with no platform types on purpose: the surface is a `void*` the platform owns and this
// only ferries. That keeps the whole handshake host-testable — which it has never been, and the RPS
// atomics it replaces have already drifted between RPS's own two mains.

namespace Lur::App {

// Which thread runs the frame loop. A config value, set once at startup, never inferred.
enum class ERenderTopology {
    Inline,      // the frame loop runs ON the platform thread (chess: CADisplayLink / looper)
    Dedicated,   // the frame loop runs on its own thread and solely owns the renderer (RPS, #183)
};

// Work the frame loop must do at a safe point, consumed once. Fields are independent: a reattach
// arrives as Park -> (ack, release) -> Reinit, while a plain rotation is Resize alone.
struct RenderWork {
    bool  Park    = false;   // park at a safe point and AckParked(true) — Dedicated only
    bool  Reinit  = false;   // Shutdown + Init against Surface (#73 reattach), then SignalReinitDone
    bool  Resize  = false;   // Resize(W, H) — the swapchain, not a full rebuild
    void* Surface = nullptr; // the platform surface to init against (CAMetalLayer, ANativeWindow)
    int   W       = 0;
    int   H       = 0;
};

class RenderHandshake {
public:
    // Topology is fixed for the life of the app; there is no case for changing it at runtime, and
    // allowing it would mean every rule below had to be re-derived mid-flight.
    void Configure(ERenderTopology Topology) { Topology_ = Topology; }
    ERenderTopology Topology() const { return Topology_; }

    // ---------------- Platform (main) thread ----------------

    // The first surface is available and sized. Until this lands, the frame loop has nothing to
    // init against and must idle rather than spin — see HasSurface().
    void PublishSurface(void* Surface, int W, int H) {
        W_.store(W, std::memory_order_relaxed);
        H_.store(H, std::memory_order_relaxed);
        Surface_.store(Surface, std::memory_order_release);
        SurfacePublished_.store(true, std::memory_order_release);
    }

    // The drawable changed size (rotation, safe-area change, #103 render-scale). Cheap: recreates the
    // swapchain, not the device.
    void RequestResize(int W, int H) {
        W_.store(W, std::memory_order_relaxed);
        H_.store(H, std::memory_order_relaxed);
        ResizeReq_.store(true, std::memory_order_release);
    }

    // #73: the window was rebuilt, so the old surface is bound to a dead window-server object and a
    // Resize cannot save it — the renderer needs a full Shutdown + Init. Implies a park, because the
    // old surface must not be in use when it is replaced.
    void RequestReattach(void* NewSurface, int W, int H) {
        W_.store(W, std::memory_order_relaxed);
        H_.store(H, std::memory_order_relaxed);
        Surface_.store(NewSurface, std::memory_order_release);
        ReinitReq_.store(true, std::memory_order_release);
        ParkReq_.store(true, std::memory_order_release);
    }

    // Park the renderer at a safe point (backgrounding, or the reattach above). Pair with Resume().
    void RequestPark() { ParkReq_.store(true, std::memory_order_release); }
    void Resume()      { ParkReq_.store(false, std::memory_order_release); }
    bool ParkRequested() const { return ParkReq_.load(std::memory_order_acquire); }

    // THE topology decision (see the header comment). Poll this after RequestPark() before touching
    // anything the renderer holds.
    //
    // Inline: the caller IS the frame loop, so by the time it can ask, it is already not rendering —
    // parked is true the moment it is requested. Waiting for an ack here would be a self-deadlock.
    // Dedicated: parked only when the render thread says so, at a point where it holds no drawable.
    bool IsParked() const {
        if (Topology_ == ERenderTopology::Inline) return ParkReq_.load(std::memory_order_acquire);
        return ParkedAck_.load(std::memory_order_acquire);
    }

    // The renderer is initialised and safe to drive. False across a failed init and for the whole of
    // a reinit — a frame issued in that window draws against a torn-down device.
    bool IsReady() const { return Ready_.load(std::memory_order_acquire); }

    // Consume-once: the reattach finished, so the platform may release the retiring view. Returns
    // false until then. (Consume-once because releasing the old view twice is the bug it prevents.)
    bool TakeReattachDone() { return ReinitDone_.exchange(false, std::memory_order_acq_rel); }

    // Teardown. The frame loop's ShouldRun() goes false; a parked loop also breaks out, so Stop()
    // never has to be ordered after a Resume() to avoid a hang.
    void Stop() { Running_.store(false, std::memory_order_release); }

    // ---------------- Frame loop (render thread, or the platform thread when Inline) -------------

    void Start()          { Running_.store(true, std::memory_order_release); }
    bool ShouldRun() const { return Running_.load(std::memory_order_acquire); }

    // Has the platform published a surface yet? The frame loop idles until it has.
    bool  HasSurface() const { return SurfacePublished_.load(std::memory_order_acquire); }
    void* Surface() const    { return Surface_.load(std::memory_order_acquire); }
    int   Width() const      { return W_.load(std::memory_order_relaxed); }
    int   Height() const     { return H_.load(std::memory_order_relaxed); }

    // Init/reinit result. Gates IsReady() for the platform side.
    void SetReady(bool Ok) { Ready_.store(Ok, std::memory_order_release); }

    // Everything pending, consumed in one shot at a safe point in the frame loop.
    //
    // Reinit is deliberately NOT consumed while a park is still requested: the reattach sequence is
    // park -> ack -> platform swaps the surface -> release -> reinit, and taking the reinit early
    // would rebuild against the surface that is about to be replaced. So Park suppresses Reinit here
    // and the loop gets it on the drain after the release. (Inline has no park, so it gets the
    // reinit immediately — correct, because there is no other thread to have raced with.)
    RenderWork TakeWork() {
        RenderWork Out;
        Out.Surface = Surface_.load(std::memory_order_acquire);
        Out.W       = W_.load(std::memory_order_relaxed);
        Out.H       = H_.load(std::memory_order_relaxed);
        if (Topology_ == ERenderTopology::Dedicated && ParkRequested()) {
            Out.Park = true;
            return Out;   // resize/reinit wait for the release; nothing is lost, both are level flags
        }
        Out.Reinit = ReinitReq_.exchange(false, std::memory_order_acq_rel);
        Out.Resize = ResizeReq_.exchange(false, std::memory_order_acq_rel);
        return Out;
    }

    // Parked / unparked, from the frame loop. Dedicated only; harmless elsewhere.
    void AckParked(bool Parked) { ParkedAck_.store(Parked, std::memory_order_release); }

    // The reinit finished (successfully or not — SetReady carries that); the platform may now drop
    // the retiring view.
    void SignalReinitDone() { ReinitDone_.store(true, std::memory_order_release); }

private:
    ERenderTopology Topology_ = ERenderTopology::Inline;   // the conservative default: no extra thread

    std::atomic<bool>  Running_{false};
    std::atomic<bool>  Ready_{false};
    std::atomic<bool>  SurfacePublished_{false};
    std::atomic<void*> Surface_{nullptr};
    std::atomic<int>   W_{0}, H_{0};

    std::atomic<bool> ResizeReq_{false};
    std::atomic<bool> ReinitReq_{false};
    std::atomic<bool> ReinitDone_{false};
    std::atomic<bool> ParkReq_{false};
    std::atomic<bool> ParkedAck_{false};
};

}  // namespace Lur::App
