// Desktop entry point for the Workbench build (roadmap Phase 0.5). A thin platform
// shim — like AndroidMain.cpp / AppMain.mm — that owns the Win32 windows + the loop,
// creates the shared Vulkan renderer(s), and drives the shared Chess::BoardView. This
// is deliberately a THIRD copy-pasted chess main (Phase-4 extraction evidence); the
// point of Phase 0.5 is to bring up the Windows platform against the known-good game.
//
// TWO windows, two full game instances, one process, a LoopbackTransport between them
// (issue #53): human-vs-human on one PC. Every net-flow bug is now reproducible in a
// debugger with both peers visible — the whole reason for the Workbench.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Lur/Core/FlightRecorder.h"
#include "Lur/Core/Log.h"
#include "Lur/Hud/DebugOverlay.h"
#include "Lur/Input/Input.h"
#include "Lur/Net/Session.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Save/SyncManager.h"
#include "Lur/Transport/Loopback.h"

namespace {

// Everything one peer needs. Two of these, linked by loopback, are a full local game.
struct GameInstance {
    Lur::Platform::Window            Win;
    Lur::Render::IRenderer*          Renderer = nullptr;
    std::unique_ptr<Lur::Save::Store>       Store;   // distinct dir -> distinct GUID -> colour
    std::string                      DeviceId;
    Chess::ChessMatchState           Match;
    std::unique_ptr<Lur::Save::SyncManager> Sync;
    Lur::Net::Session                Session;
    Chess::BoardView                 View;
    Lur::Transport::LoopbackTransport Transport;
    Lur::Core::FlightRecorder        Recorder;   // record the session for replay/debug
    std::string                      RecPath;
    Lur::Hud::DebugOverlay           Overlay;
    bool                             ShowOverlay = true;  // F1 toggles
    float                            FrameMs = 0.0f;
};

// The hijack-guarded record share, mirroring the phone mains: only send OUR game to
// the peer we're actually playing.
void SendRecord(GameInstance& G) {
    if (G.View.ActiveOpponentGuid() != G.Session.GetPeerGuid()) return;
    const std::vector<uint8_t> Snap = G.Sync->Snapshot();
    G.Session.Send(Lur::Net::EMsgType::Sync, Snap.data(), Snap.size());
}

// Link (ReadyHandler) and reconnect (ResyncHandler): adopt the peer per the hijack
// rule, and if adopted, share our record.
void OnLive(GameInstance& G) {
    if (G.View.OnPeerLinked(G.Session.GetPeerGuid())) SendRecord(G);
}

// Portrait, matching the phones (~9:20) — the HUD margins above/below the square
// board are designed for that shape, so a square desktop window mislaid them.
constexpr int kWinW = 360;
constexpr int kWinH = 800;

bool Setup(GameInstance& G, const char* Title, const char* SaveDir, int X) {
    if (!G.Win.Create(Title, kWinW, kWinH, X, 30)) return false;
    G.Renderer = Lur::Render::VulkanRenderer::Create();
    if (G.Renderer == nullptr || !G.Renderer->Init(G.Win.NativeHandle())) return false;

    G.Store    = std::make_unique<Lur::Save::Store>(SaveDir);
    G.DeviceId = Lur::Save::LoadOrCreateDeviceId(*G.Store);
    G.Sync     = std::make_unique<Lur::Save::SyncManager>(*G.Store, G.Match);

    G.Match.SetOnMatchEnd([&G] { G.Sync->Persist(); });

    G.View.SetState(&G.Match);
    G.View.AttachSession(&G.Session);
    G.View.AttachPersistence(G.Store.get(), G.Sync.get(), G.DeviceId);
    G.View.SetLogger([](const char* M) { Lur::Log::Info("View: %s", M); });
    G.View.CreateResources(G.Renderer);
    G.Overlay.CreateResources(G.Renderer);

    // Debug overlay drawn inside the frame, on top of the board (F1 toggles it).
    G.View.SetPostGuiHook([&G] {
        if (!G.ShowOverlay) return;
        int W = 0, H = 0;
        G.Win.GetSize(&W, &H);
        const std::string& Peer = G.Session.GetPeerGuid();
        const std::string Short = Peer.empty() ? std::string() : Peer.substr(0, 8);
        Lur::Hud::DebugStats S;
        S.FrameMs = G.FrameMs;
        S.Link = G.Session.GetLinkState();
        S.NsSinceRecv = G.Session.GetNsSinceRecv();
        S.Sent = G.Session.GetDatagramsSent();
        S.Recv = G.Session.GetDatagramsReceived();
        S.PeerShort = Short.c_str();
        G.Overlay.Draw(G.Renderer, static_cast<float>(W), static_cast<float>(H), S);
    });

    G.Session.SetLogger([](const char* M) { Lur::Log::Info("Net: %s", M); });

    G.Session.SetReadyHandler([&G] { OnLive(G); });
    G.Session.SetResyncHandler([&G] { OnLive(G); });
    G.Session.SetHandler(Lur::Net::EMsgType::Sync, [&G](const uint8_t* D, std::size_t N) {
        G.Recorder.Record(Lur::Core::EFlightEvent::DatagramIn, 0, D, N);  // Sync in
        if (G.View.ActiveOpponentGuid() == G.Session.GetPeerGuid()) G.Sync->OnSync(D, N);
    });
    G.RecPath = std::string(SaveDir) + ".flightrec";
    return true;
}

void PumpInput(GameInstance& G, uint64_t TimeNs) {
    if (G.Win.TakeOverlayToggle()) G.ShowOverlay = !G.ShowOverlay;  // F1
    int W = 0, H = 0;
    G.Win.GetSize(&W, &H);
    for (const Lur::Input::TouchEvent& T : G.Win.TakeTouches()) {
        // Record every touch (Review #2: record everything). Payload: phase + x + y.
        uint8_t Blob[9];
        Blob[0] = static_cast<uint8_t>(T.Phase);
        std::memcpy(Blob + 1, &T.XPx, 4);
        std::memcpy(Blob + 5, &T.YPx, 4);
        G.Recorder.Record(Lur::Core::EFlightEvent::Input, TimeNs, Blob, sizeof(Blob));
        if (T.Phase == Lur::Input::ETouchPhase::Ended && W > 0 && H > 0)
            G.View.OnTap(T.XPx, T.YPx, static_cast<float>(W), static_cast<float>(H));
    }
    if (W > 0 && H > 0) G.View.Render(G.Renderer, static_cast<float>(W), static_cast<float>(H));
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so logs flush live/on kill
    Lur::Log::Init(nullptr, "Desktop");        // built-in stdout/stderr sink for the host

    int MaxFrames = 0;  // 0 = run until a window is closed; "--frames N" = headless smoke
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--frames" && i + 1 < argc) MaxFrames = std::atoi(argv[++i]);

    Lur::Log::Info("LurMotorn desktop (Workbench) - two-window loopback");

    GameInstance A, B;
    if (!Setup(A, "OnlyChess - White side", ".lur-desktop-save/a", 120) ||
        !Setup(B, "OnlyChess - Black side", ".lur-desktop-save/b", 520)) {
        Lur::Log::Error("setup failed");
        return 1;
    }
    Lur::Log::Info("two renderers up; ids A=%.8s B=%.8s", A.DeviceId.c_str(), B.DeviceId.c_str());

    // One in-process link. Start both, then Tick drives the Hello handshake to Ready.
    Lur::Transport::LoopbackTransport::Link(A.Transport, B.Transport);
    A.Session.Start(&A.Transport, A.DeviceId);
    B.Session.Start(&B.Transport, B.DeviceId);

    Lur::Log::Info("entering frame loop (%s)",
                   MaxFrames > 0 ? "headless --frames" : "close either window to quit");
    int Frame = 0;
    bool Linked = false;
    auto PrevTime = std::chrono::steady_clock::now();
    while (A.Win.PumpEvents() && B.Win.PumpEvents()) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

        A.Session.Tick(ElapsedNs);
        B.Session.Tick(ElapsedNs);
        if (!Linked && A.Session.IsReady() && B.Session.IsReady()) {
            Linked = true;
            A.Recorder.Record(Lur::Core::EFlightEvent::LinkUp, 0, nullptr, 0);
            B.Recorder.Record(Lur::Core::EFlightEvent::LinkUp, 0, nullptr, 0);
            Lur::Log::Info("handshake complete - both sessions linked");
        }

        const uint64_t NowNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now.time_since_epoch()).count();
        A.FrameMs = B.FrameMs = static_cast<float>(ElapsedNs) / 1.0e6f;
        PumpInput(A, NowNs);
        PumpInput(B, NowNs);

        if (MaxFrames > 0 && ++Frame >= MaxFrames) {
            Lur::Log::Info("rendered %d frames headless (linked=%d) - exiting", Frame, Linked ? 1 : 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Write each session's flight recording — a crash/desync now ships as a file that
    // replays through the loopback path (proven by NetTests). Bounded ring, tiny.
    for (GameInstance* G : {&A, &B})
        if (G->Recorder.WriteFile(G->RecPath.c_str()))
            Lur::Log::Info("flight recording written: %s (%zu events)",
                           G->RecPath.c_str(), G->Recorder.Count());

    A.Renderer->Shutdown();
    B.Renderer->Shutdown();
    Lur::Log::Info("clean exit");
    return 0;
}
