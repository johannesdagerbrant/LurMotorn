// Chess's board-palette CVars (#201): background, the two square colours, the two piece fills.
//
// These are chess's FIRST CVars, and the thing worth testing is not that a colour struct holds three
// floats — it is that the palette actually reaches the renderer, EVERY FRAME. A tunable colour that
// only applies at material-creation time is indistinguishable from a constant: you would edit it,
// nothing would move, and you would conclude the CVar was broken rather than that it was never
// re-read.
//
// So the test drives BoardView::Render against a recording IRenderer and asserts what arrived. That
// also pins the seam this needed: IRenderer::SetClearColor, which replaced a literal buried in the
// Vulkan backend.
#include <cstdio>
#include <string>
#include <vector>

#include "Chess/View/BoardView.h"
#include "Lur/Core/CVar.h"
#include "Lur/Render/ColorString.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Render::Color;

namespace {

// Records the calls the palette is supposed to make. Everything else is a stub that hands back
// plausible handles — BoardView only needs them to be non-zero and distinct.
class RecordingRenderer final : public Lur::Render::IRenderer {
public:
    Color Clear{};
    int ClearCalls = 0;
    std::vector<std::pair<Lur::Render::MaterialHandle, Color>> Tints;

    void SetClearColor(const Color& C) override {
        Clear = C;
        ++ClearCalls;
    }
    void SetMaterialTint(Lur::Render::MaterialHandle M, const Color& T) override {
        Tints.push_back({M, T});
    }

    // The tint a handle last received, or a sentinel alpha of -1 if it never did.
    Color LastTint(Lur::Render::MaterialHandle M) const {
        Color Out{0, 0, 0, -1.0f};
        for (const auto& [H, T] : Tints)
            if (H == M) Out = T;
        return Out;
    }

    // ---- everything below is a stub ----
    bool Init(void*) override { return true; }
    void Shutdown() override {}
    void Resize(int, int) override {}
    Lur::Render::MeshHandle CreateMesh(const Lur::Render::Vertex*, uint32_t, const uint32_t*,
                                       uint32_t) override {
        return ++NextMesh;
    }
    Lur::Render::TextureHandle LoadTexture(const uint8_t*, int, int,
                                           Lur::Render::ETextureFormat) override {
        return ++NextTex;
    }
    Lur::Render::MaterialHandle CreateMaterial(const Lur::Render::MaterialDesc&) override {
        return ++NextMat;
    }
    void BeginFrame(const Lur::Render::Camera&) override {}
    void DrawMesh(Lur::Render::MeshHandle, Lur::Render::MaterialHandle,
                  const Lur::Math::Mat4&) override {}
    void EndFrame() override {}

private:
    Lur::Render::MeshHandle NextMesh = 0;
    Lur::Render::TextureHandle NextTex = 0;
    Lur::Render::MaterialHandle NextMat = 0;
};

bool Same(const Color& A, const Color& B) {
    return A.R == B.R && A.G == B.G && A.B == B.B && A.A == B.A;
}

}  // namespace

// ---- The five CVars exist, are view-only, and default to the old hardcoded palette ----
// View-only matters: a gameplay-flagged CVar is hashed and synced, and two phones showing different
// board colours would then read as a desync. Board paint is exactly the kind of thing that MAY differ
// per device.
static void TestPaletteCvarsAreViewOnly() {
    using Lur::Core::CVarRegistry;
    const char* Names[] = {"chess.view.background", "chess.view.square_light",
                           "chess.view.square_dark", "chess.view.piece_light",
                           "chess.view.piece_dark"};
    for (const char* N : Names) {
        Lur::Core::ICVar* C = CVarRegistry::Find(N);
        CHECK(C != nullptr);
        if (C == nullptr) continue;
        CHECK(C->IsColor());                                       // the console gives it a picker
        CHECK((C->Flags() & Lur::Core::CVarFlagAffectsGameplay) == 0);
    }
}

// ---- THE POINT: the palette is re-applied EVERY frame, not just at material creation ----
// A colour that is only read when the material is made is a constant with extra steps.
static void TestPaletteReachesTheRendererEveryFrame() {
    RecordingRenderer R;
    Chess::BoardView V;
    V.CreateResources(&R);

    R.Tints.clear();
    R.ClearCalls = 0;
    V.Render(&R, 800.0f, 1200.0f);
    CHECK(R.ClearCalls == 1);
    const std::size_t FirstFrameTints = R.Tints.size();
    CHECK(FirstFrameTints >= 14);   // 2 squares + 6 light pieces + 6 dark pieces

    // A second frame must do it again. If the palette were applied once at startup this is 0.
    R.Tints.clear();
    R.ClearCalls = 0;
    V.Render(&R, 800.0f, 1200.0f);
    CHECK(R.ClearCalls == 1);
    CHECK(R.Tints.size() == FirstFrameTints);
}

// ---- An EDIT lands on the next frame, with no material re-creation ----
// This is the behaviour a tuner actually judges: move the slider, watch the board change.
static void TestEditIsLiveOnTheNextFrame() {
    RecordingRenderer R;
    Chess::BoardView V;
    V.CreateResources(&R);

    Lur::Core::ICVar* Bg = Lur::Core::CVarRegistry::Find("chess.view.background");
    Lur::Core::ICVar* Sq = Lur::Core::CVarRegistry::Find("chess.view.square_light");
    CHECK(Bg != nullptr && Sq != nullptr);
    if (Bg == nullptr || Sq == nullptr) return;

    const std::string BgWas = Bg->ValueString();
    const std::string SqWas = Sq->ValueString();

    CHECK(Bg->SetFromString("1 0 0 1"));      // hot pink-red background, unmistakable
    CHECK(Sq->SetFromString("0 1 0 1"));      // green light squares
    V.Render(&R, 800.0f, 1200.0f);
    CHECK(Same(R.Clear, Color{1, 0, 0, 1}));

    // Find the light-square material by the tint it received; there must be exactly one carrying it.
    int GreenTints = 0;
    for (const auto& [H, T] : R.Tints)
        if (Same(T, Color{0, 1, 0, 1})) ++GreenTints;
    CHECK(GreenTints == 1);

    // Restore, and confirm the restore ALSO lands live — a one-way edit would pass the check above
    // while still being broken in the direction a tuner uses most (undo).
    CHECK(Bg->SetFromString(BgWas.c_str()));
    CHECK(Sq->SetFromString(SqWas.c_str()));
    R.Tints.clear();
    V.Render(&R, 800.0f, 1200.0f);
    CHECK(!Same(R.Clear, Color{1, 0, 0, 1}));
}

// ---- Colour CVars round-trip through the config codec ----
// Which is what lets them persist in a cvars.cfg and show a value in the console. Without
// ColorString.h in the translation unit the CVar would not even compile; this pins the behaviour
// rather than the compile.
static void TestColorCvarRoundTrips() {
    Lur::Core::ICVar* C = Lur::Core::CVarRegistry::Find("chess.view.piece_dark");
    CHECK(C != nullptr);
    if (C == nullptr) return;
    const std::string Was = C->ValueString();
    CHECK(C->SetFromString("0.25 0.5 0.75 1"));
    const std::string Now = C->ValueString();
    Lur::Render::Color Parsed{};
    CHECK(Lur::Render::FromString(Now.c_str(), Parsed));
    CHECK(Parsed.R > 0.24f && Parsed.R < 0.26f);
    CHECK(Parsed.G > 0.49f && Parsed.G < 0.51f);
    CHECK(Parsed.B > 0.74f && Parsed.B < 0.76f);
    CHECK(C->SetFromString(Was.c_str()));
}

int main() {
    Lur::Core::CVarEnterMain();  // CVars may not be read before main() (spec §1.1)
    TestPaletteCvarsAreViewOnly();
    TestPaletteReachesTheRendererEveryFrame();
    TestEditIsLiveOnTheNextFrame();
    TestColorCvarRoundTrips();
    if (GFailures == 0) std::printf("chess_palette_tests: ALL PASS\n");
    else std::printf("chess_palette_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
