// Desktop entry point for RocksPapersScissors (Phase 1, slice 0). A thin platform
// shim — like chess's DesktopMain.cpp, copy-pasted on purpose (Phase-4 GameHost is
// what earns the extraction). It owns the Win32 window + the render loop, creates the
// Vulkan renderer, and drives the SimRunner (the sim on its own thread) + GameView
// (draws the published snapshots). No net yet: playable vs yourself.
//
// Input: keys 1-4 queue units for YOU (team 0), 5-8 for the FOE (team 1) — two-handed
// self-play; drag pans the camera (design doc §9). --auto spawns random soldiers for
// both sides so combat is visible without typing; --frames N runs headless (smoke).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "Lur/Core/Log.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Sim/Random.h"
#include "Rps/GameView.h"
#include "Rps/SimRunner.h"
#include "Rps/Snapshot.h"
#include "Rps/Tunables.h"

namespace {

// Shared with the sim thread: presses accumulate here (main thread ORs), and the sim
// thread drains them once per tick (exchange to 0), so each press lands on exactly one
// tick. Atomics make the hand-off race-free without a lock.
struct Inputs {
    std::atomic<uint8_t> P0{0};
    std::atomic<uint8_t> P1{0};
};

void SampleInputs(void* Ctx, uint32_t /*Tick*/, uint8_t& M0, uint8_t& M1) {
    Inputs* In = static_cast<Inputs*>(Ctx);
    M0 = In->P0.exchange(0, std::memory_order_relaxed);
    M1 = In->P1.exchange(0, std::memory_order_relaxed);
}

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Portrait, ~phone aspect. Width fills WorldWidth; the field's height (the balance
// knob, §9) fits this window today, so the camera pan is present but a no-op until the
// height tunable grows — honest to the design, not faked.
constexpr int kWinW = 360;
constexpr int kWinH = 780;

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Lur::Log::Init(nullptr, "RpsDesktop");

    int MaxFrames = 0;      // "--frames N" = headless smoke; 0 = until the window closes
    bool Auto = false;      // "--auto" = random soldier presses for both sides
    uint64_t Seed = 0x1234; // "--seed S"
    int Stress = 0;         // "--stress N" = bulk-spawn N soldiers/side (the #75 stress scene)
    for (int I = 1; I < argc; ++I) {
        std::string A = argv[I];
        if (A == "--frames" && I + 1 < argc) MaxFrames = std::atoi(argv[++I]);
        else if (A == "--auto") Auto = true;
        else if (A == "--seed" && I + 1 < argc) Seed = std::strtoull(argv[++I], nullptr, 0);
        else if (A == "--stress" && I + 1 < argc) Stress = std::atoi(argv[++I]);
    }

    Lur::Platform::Window Win;
    if (!Win.Create("RocksPapersScissors", kWinW, kWinH, 200, 60)) {
        Lur::Log::Error("window create failed");
        return 1;
    }
    Lur::Render::IRenderer* Renderer = Lur::Render::VulkanRenderer::Create();
    if (Renderer == nullptr || !Renderer->Init(Win.NativeHandle())) {
        Lur::Log::Error("renderer init failed");
        return 1;
    }

    Rps::GameView View;
    View.CreateResources(Renderer);

    Inputs In;
    auto Runner = std::make_unique<Rps::SimRunner>();
    Runner->Start(Seed, SampleInputs, &In, static_cast<uint32_t>(Stress < 0 ? 0 : Stress));
    Lur::Log::Info("RPS desktop up (seed 0x%llx%s%s)", static_cast<unsigned long long>(Seed),
                   Auto ? ", auto" : "", Stress > 0 ? ", stress" : "");

    const float Ppu = static_cast<float>(kWinW) /
                      (static_cast<float>(Rps::WorldWidth.Raw) / static_cast<float>(Rps::Fixed::One));
    const float WorldH = static_cast<float>(Rps::WorldHeight.Raw) / static_cast<float>(Rps::Fixed::One);

    float CameraY = 0.0f;
    bool Dragging = false;
    float PrevTouchY = 0.0f;
    Lur::Sim::SplitMix64 Rng(Seed ^ 0xA11CE);
    uint64_t AutoAccumNs = 0;
    uint64_t PrevNs = NowNs();

    static Rps::Snapshot Snap;  // reused each frame (~90 KB; keep it off the stack loop)
    int Frame = 0;
    while (Win.PumpEvents()) {
        const uint64_t Now = NowNs();
        const uint64_t ElapsedNs = Now - PrevNs;
        PrevNs = Now;

        // Keyboard -> pending presses. 1-4 (0x31..0x34) = team 0; 5-8 (0x35..0x38) = team 1.
        for (uint32_t Vk : Win.TakeKeys()) {
            if (Vk >= 0x31 && Vk <= 0x34) In.P0.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x31)));
            else if (Vk >= 0x35 && Vk <= 0x38) In.P1.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x35)));
        }

        // Drag -> camera pan (view-only; never touches the sim).
        for (const Lur::Input::TouchEvent& T : Win.TakeTouches()) {
            if (T.Phase == Lur::Input::ETouchPhase::Began) { Dragging = true; PrevTouchY = T.YPx; }
            else if (T.Phase == Lur::Input::ETouchPhase::Moved && Dragging) {
                CameraY -= (T.YPx - PrevTouchY) / Ppu;  // grab-the-world: drag up reveals higher Y
                PrevTouchY = T.YPx;
            } else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                       T.Phase == Lur::Input::ETouchPhase::Cancelled) {
                Dragging = false;
            }
        }

#if LUR_INTERNAL
        if (Auto) {
            AutoAccumNs += ElapsedNs;
            if (AutoAccumNs > 700'000'000ull) {  // ~every 0.7 s, both sides press a random soldier
                AutoAccumNs = 0;
                In.P0.fetch_or(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
                In.P1.fetch_or(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
            }
        }
#else
        (void)Auto;
#endif

        if (Runner->LatestSnapshot(Snap)) {
            int W = 0, H = 0;
            Win.GetSize(&W, &H);
            if (W > 0 && H > 0) {
                const float VisibleH = static_cast<float>(H) / Ppu;
                const float MaxCam = WorldH - VisibleH > 0.0f ? WorldH - VisibleH : 0.0f;
                if (CameraY < 0.0f) CameraY = 0.0f;
                if (CameraY > MaxCam) CameraY = MaxCam;
                View.Render(Renderer, Snap, Snap.AlphaAt(Now), CameraY, static_cast<float>(W),
                            static_cast<float>(H));
            }
        }

        if (MaxFrames > 0 && ++Frame >= MaxFrames) {
            Lur::Log::Info("rendered %d frames headless (tick %u) - exiting", Frame,
                           Runner->PublishedTick());
            break;
        }
    }

    Runner->Stop();
    Renderer->Shutdown();
    Lur::Log::Info("clean exit");
    return 0;
}
