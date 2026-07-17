// Desktop entry point for the Workbench build (roadmap Phase 0.5). A thin platform
// shim — like AndroidMain.cpp / AppMain.mm — that owns a Win32 window + the loop,
// creates the shared Vulkan renderer, and drives the shared Chess::BoardView. This is
// deliberately a THIRD copy-pasted chess main (Phase-4 extraction evidence); the
// point of Phase 0.5 is to bring up the Windows platform against the known-good game.
//
// Single-window for now (issue #51: platform bring-up + render + input). Two windows
// over a LoopbackTransport for real local play arrives in issue #53.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Lur/Input/Input.h"
#include "Lur/Net/Session.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Transport/Loopback.h"

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so logs flush live/on kill

    // Headless smoke mode: "--frames N" renders N frames then exits 0. Lets CI / an
    // agent confirm the platform + Vulkan bring-up without a human closing the window.
    int MaxFrames = 0;  // 0 = run until the window is closed
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--frames" && i + 1 < argc) MaxFrames = std::atoi(argv[++i]);

    std::printf("[Desktop] LurMotorn desktop (Workbench) starting\n");

    Lur::Platform::Window Win;
    if (!Win.Create("OnlyChess - Desktop", 720, 720)) {
        std::fprintf(stderr, "[Desktop] window creation failed\n");
        return 1;
    }

    Lur::Render::IRenderer* Renderer = Lur::Render::VulkanRenderer::Create();
    if (Renderer == nullptr || !Renderer->Init(Win.NativeHandle())) {
        std::fprintf(stderr, "[Desktop] renderer init failed\n");
        return 1;
    }
    std::printf("[Desktop] renderer initialized\n");

    // Smoke test: the shared, perft-verified core runs on the desktop too.
    {
        Chess::Board B = Chess::Board::StartPosition();
        Chess::MoveList M;
        Chess::GenerateLegalMoves(B, M);
        std::printf("[Desktop] chess core alive: %d legal moves from the start\n", M.Count);
    }

    // Full game wiring, mirroring the phone mains (persistence + session) so the shared
    // BoardView runs against the exact same code paths. No peer this issue, so the
    // session stays in Searching — the board renders and taps are handled, but real
    // 2-player play needs the two-window loopback of issue #53.
    Lur::Save::Store Store(".lur-desktop-save");
    const std::string DeviceId = Lur::Save::LoadOrCreateDeviceId(Store);
    Chess::ChessMatchState Match;
    Lur::Save::SyncManager Sync(Store, Match);
    Lur::Net::Session Session;
    Lur::Transport::LoopbackTransport Transport;  // unlinked: no peer this issue

    Chess::BoardView View;
    View.SetState(&Match);
    View.AttachSession(&Session);
    View.AttachPersistence(&Store, &Sync, DeviceId);
    View.SetLogger([](const char* M) { std::printf("[View] %s\n", M); });
    View.CreateResources(Renderer);

    Session.SetLogger([](const char* M) { std::printf("[Net] %s\n", M); });
    Match.SetOnMatchEnd([&Sync] { Sync.Persist(); });
    Session.Start(&Transport, DeviceId);

    std::printf("[Desktop] entering frame loop (%s)\n",
                MaxFrames > 0 ? "headless --frames" : "close the window to quit");
    int Frame = 0;
    auto PrevTime = std::chrono::steady_clock::now();
    while (Win.PumpEvents()) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

        Session.Tick(ElapsedNs);

        int W = 0, H = 0;
        Win.GetSize(&W, &H);
        for (const Lur::Input::TouchEvent& T : Win.TakeTouches()) {
            if (T.Phase == Lur::Input::ETouchPhase::Ended && W > 0 && H > 0)
                View.OnTap(T.XPx, T.YPx, static_cast<float>(W), static_cast<float>(H));
        }

        if (W > 0 && H > 0)
            View.Render(Renderer, static_cast<float>(W), static_cast<float>(H));

        if (MaxFrames > 0 && ++Frame >= MaxFrames) {
            std::printf("[Desktop] rendered %d frames headless — exiting\n", Frame);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    Renderer->Shutdown();
    std::printf("[Desktop] clean exit\n");
    return 0;
}
