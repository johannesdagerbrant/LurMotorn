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

#include "WindowsBleTransport.h"

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

    G.Match.SetOnMatchEnd([&G] {
        G.Sync->Persist();
        Lur::Log::Info("MATCH END result=%d WLD(lo/hi/dr)=%u/%u/%u total=%u",
                       static_cast<int>(G.Match.LastResult()), G.Match.Record().WinsLower,
                       G.Match.Record().WinsHigher, G.Match.Record().Draws,
                       G.Match.Record().TotalMatches());
    });

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
    G.Session.SetStateHashFn([&G] { return G.Match.PositionHash(); });  // desync detection (#72)
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

// Parse an algebraic square ("e2") to an index 0..63 (a1=0, h8=63), or -1 if bad.
int ParseSquare(const char* S) {
    if (S[0] < 'a' || S[0] > 'h' || S[1] < '1' || S[1] > '8') return -1;
    return (S[1] - '1') * 8 + (S[0] - 'a');
}

const char* ColorName(Chess::EColor C) { return C == Chess::EColor::White ? "White" : "Black"; }

// The dev-rig path (issue #58): ONE desktop window, wired to the phone over real BLE
// via WindowsBleTransport instead of the loopback pair. The phone talks to it as if
// it were a second phone — the whole point of the Workbench's third radio. Returns a
// process exit code.
//
// Two dev drivers (LUR_INTERNAL only), both firing on the SAME frame our turn opens:
//   * Auto: play a random legal move every frame it's our turn (fully automated match).
//   * Script: a comma-separated move list ("f2f3,g2g4,..."); play the first entry legal
//     on our turn and drop it (the peer's entries are played on the phone).
// GamesToPlay > 0 stops after that many completed matches (0 = run until the window
// closes). Every ~1s it logs a connection-quality line; on exit it reports the
// same-frame reply tally (did our reply ship the frame we received the peer's move?).
int RunBle(const char* RadioExe, int MaxFrames, const std::string& Script, bool Auto, int GamesToPlay,
           int LoopMs) {
    Lur::Log::Info("LurMotorn desktop (Workbench) - BLE peer vs phone (radio=%s)", RadioExe);

    GameInstance G;
    if (!Setup(G, "OnlyChess - BLE peer", ".lur-desktop-save/ble", 320)) {
        Lur::Log::Error("setup failed");
        return 1;
    }

    Lur::DevRig::WindowsBleTransport Ble(RadioExe);
    Ble.SetLogger([](const char* M) { Lur::Log::Info("%s", M); });
    if (!Ble.Start()) {
        Lur::Log::Error("BLE radio failed to start - build it first: "
                        "powershell -File Tools\\BleDevRig\\build.ps1 -Source BleRadio.cs");
        return 1;
    }

    G.Session.Start(&Ble, G.DeviceId);
    Lur::Log::Info("session started (id %.8s); waiting for the phone to advertise", G.DeviceId.c_str());

    // Parse the move script into (From,To) pairs we play on our turn.
    std::vector<std::pair<int, int>> Moves;
    for (std::size_t i = 0; i + 3 < Script.size();) {
        while (i < Script.size() && Script[i] == ',') ++i;
        if (i + 3 >= Script.size() + 1) break;
        int From = ParseSquare(&Script[i]);
        int To   = (i + 2 < Script.size()) ? ParseSquare(&Script[i + 2]) : -1;
        if (From >= 0 && To >= 0) Moves.emplace_back(From, To);
        i += 4;
    }
    if (!Moves.empty()) Lur::Log::Info("script: %zu moves loaded (we play the ones legal on our turn)", Moves.size());
    if (Auto) Lur::Log::Info("autoplay: random legal move every frame it's our turn (games=%d)", GamesToPlay);

    int Frame = 0;
    bool Linked = false;
    uint32_t Rng = 0xC0FFEEu ^ static_cast<uint32_t>(G.DeviceId.size());  // per-device autoplay seed
    uint64_t TraceAccumNs = 0;   // ~1 Hz connection-quality trace
    // Same-frame reply accounting: did we move the frame we received the peer's move?
    uint64_t PeerReplies = 0, SameFrame = 0, NewGameOpens = 0, DelayedReplies = 0;
    bool Draining = false;          // hit the games target: stop playing, keep the link alive
    uint64_t DrainAccumNs = 0;      // so the deciding move flushes + the peer converges before exit
    auto PrevTime = std::chrono::steady_clock::now();
    while (G.Win.PumpEvents()) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

        const bool     WasMyTurn = G.Match.IsMyTurn();
        const uint32_t MatchesBefore = G.Match.Record().TotalMatches();

        G.Session.Tick(ElapsedNs);  // pumps the radio inbox -> applies any peer move THIS frame

        if (!Linked && G.Session.IsReady()) {
            Linked = true;
            G.Recorder.Record(Lur::Core::EFlightEvent::LinkUp, 0, nullptr, 0);
            Lur::Log::Info("handshake complete - live over BLE with peer %.8s; we are %s",
                           G.Session.GetPeerGuid().c_str(), ColorName(G.Match.MyColor()));
        }

        const bool     NowMyTurn = G.Match.IsMyTurn();
        const uint32_t MatchesAfterTick = G.Match.Record().TotalMatches();
        const bool     GotPeerMove   = !WasMyTurn && NowMyTurn;   // peer moved -> our turn, this frame
        const bool     PeerEndedGame = MatchesAfterTick != MatchesBefore;

        // Move driver — fires in the SAME loop iteration the peer's move was applied.
        bool Played = false;
#if LUR_INTERNAL
        if (Linked && NowMyTurn && !Draining) {
            if (Auto) {
                Played = G.View.AutoPlayRandomLegalMove(Rng);
            } else {
                for (std::size_t i = 0; i < Moves.size(); ++i)
                    if (G.View.PlayMove(static_cast<Chess::Square>(Moves[i].first),
                                        static_cast<Chess::Square>(Moves[i].second))) {
                        Moves.erase(Moves.begin() + i); Played = true; break;
                    }
            }
        }
#endif
        if (GotPeerMove) {
            ++PeerReplies;
            if (PeerEndedGame)   ++NewGameOpens;   // peer's move ended a game; we opened the next, same frame
            else if (Played)     ++SameFrame;      // replied the same frame we received
            else               { ++DelayedReplies; Lur::Log::Info("WARN: our turn but no same-frame reply @frame %d", Frame); }
        }

        // Connection-quality trace (~1 Hz): link, byte/datagram throughput, liveness.
        TraceAccumNs += ElapsedNs;
        if (Linked && TraceAccumNs > 1'000'000'000ull) {
            TraceAccumNs = 0;
            Lur::Log::Info("QUAL link=%d game=%u tx=%u/%lluB rx=%u/%lluB sinceRx=%llums sameFrame=%llu/%llu",
                           static_cast<int>(G.Session.GetLinkState()), MatchesAfterTick,
                           G.Session.GetDatagramsSent(), (unsigned long long)Ble.GetBytesOut(),
                           G.Session.GetDatagramsReceived(), (unsigned long long)Ble.GetBytesIn(),
                           (unsigned long long)(G.Session.GetNsSinceRecv() / 1'000'000ull),
                           (unsigned long long)SameFrame, (unsigned long long)PeerReplies);
        }

        const uint64_t NowNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now.time_since_epoch()).count();
        G.FrameMs = static_cast<float>(ElapsedNs) / 1.0e6f;
        PumpInput(G, NowNs);

        ++Frame;
        // Reached the games target: DON'T exit instantly — the deciding move we just
        // made is still being written to the peer (a ~100ms write-with-response), and
        // killing the radio now strands the peer mid-game (it never concludes/resets).
        // Drain: stop playing, keep ticking so the move flushes + the peer converges.
        if (GamesToPlay > 0 && !Draining && static_cast<int>(MatchesAfterTick) >= GamesToPlay) {
            Draining = true;
            Lur::Log::Info("completed %d games - draining ~3s so the peer gets the deciding move + resets",
                           GamesToPlay);
        }
        if (Draining) {
            DrainAccumNs += ElapsedNs;
            if (DrainAccumNs > 3'000'000'000ull) { Lur::Log::Info("drain complete - stopping"); break; }
        }
        if (MaxFrames > 0 && Frame >= MaxFrames) {
            Lur::Log::Info("rendered %d frames headless (linked=%d) - exiting", Frame, Linked ? 1 : 0);
            break;
        }
        if (LoopMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(LoopMs));
    }

    Lur::Log::Info("SAME-FRAME REPLY REPORT: %llu/%llu peer moves answered same frame "
                   "(%llu new-game opens same frame, %llu DELAYED)",
                   (unsigned long long)SameFrame, (unsigned long long)PeerReplies,
                   (unsigned long long)NewGameOpens, (unsigned long long)DelayedReplies);

    if (G.Recorder.WriteFile(G.RecPath.c_str()))
        Lur::Log::Info("flight recording written: %s (%zu events)", G.RecPath.c_str(), G.Recorder.Count());
    G.Renderer->Shutdown();
    Lur::Log::Info("clean exit");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so logs flush live/on kill
    Lur::Log::Init(nullptr, "Desktop");        // built-in stdout/stderr sink for the host

    int MaxFrames = 0;  // 0 = run until a window is closed; "--frames N" = headless smoke
    bool Ble = false;   // "--ble [path]" = one window, live over real BLE to the phone
    std::string RadioExe = "Tools\\BleDevRig\\BleRadio.exe";  // relative to the repo root
    std::string Script;  // "--script f2f3,e7e5,..." = scripted match (we play our colour's)
    bool Auto = false;   // "--auto" = play a random legal move every frame it's our turn
    int Games = 0;       // "--games N" = stop after N completed matches (0 = until closed)
    int LoopMs = 8;      // "--loop-ms N" = per-frame sleep (0 = busy loop; probes loop-cadence latency)
    for (int i = 1; i < argc; ++i) {
        std::string Arg = argv[i];
        if (Arg == "--frames" && i + 1 < argc) MaxFrames = std::atoi(argv[++i]);
        else if (Arg == "--script" && i + 1 < argc) Script = argv[++i];
        else if (Arg == "--auto") Auto = true;
        else if (Arg == "--games" && i + 1 < argc) Games = std::atoi(argv[++i]);
        else if (Arg == "--loop-ms" && i + 1 < argc) LoopMs = std::atoi(argv[++i]);
        else if (Arg == "--ble") {
            Ble = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') RadioExe = argv[++i];
        }
    }

    // Dev-rig mode: a single window driven over real Bluetooth by the C# radio.
    if (Ble) return RunBle(RadioExe.c_str(), MaxFrames, Script, Auto, Games, LoopMs);

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
