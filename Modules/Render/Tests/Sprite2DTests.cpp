// Lur::Render::MakeFlatMaterial — the flat-colour 2D material, promoted in #201 from five copies.
//
// This is five lines of struct-filling, so the thing worth testing is not that it compiles: it is the
// CONTRACT those five lines encode, because each field is load-bearing in a way that fails silently if
// it drifts.
//
//   * `BaseColor = 0` reads like an oversight. It is the whole mechanism — handle 0 means "flat
//     white" and the shader multiplies sampled colour by tint, so sampling white and tinting is how
//     you get a solid colour without allocating a 1x1 texture. Set a real texture here and every flat
//     panel in both games silently samples piece art.
//   * The tint must pass through UNCHANGED, alpha included. Most callers are translucent — the dev
//     console's panel, the HUD's bars, the placement ghost — so a dropped or clamped alpha turns a
//     see-through surface opaque, which reads as a z-order bug rather than a material bug.
//   * `Lit = false`. A lit material shades by a surface normal, and a 2D quad's normal means nothing;
//     the result is geometry that dims depending on where a light happens to be.
//
// The two named copies this replaced had already drifted on that last field — one set it explicitly,
// one relied on the default. Harmless while the default is false, which is exactly why nobody noticed.
#include <cstdio>
#include <vector>

#include "Lur/Render/Sprite2D.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Render::Color;
using Lur::Render::MaterialDesc;

namespace {

// Captures every MaterialDesc handed to CreateMaterial, and hands back distinct handles.
class CapturingRenderer final : public Lur::Render::IRenderer {
public:
    std::vector<MaterialDesc> Descs;

    Lur::Render::MaterialHandle CreateMaterial(const MaterialDesc& D) override {
        Descs.push_back(D);
        return ++Next;
    }

    bool Init(void*) override { return true; }
    void Resize(int, int) override {}
    void Shutdown() override {}
    Lur::Render::MeshHandle CreateMesh(const Lur::Render::Vertex*, uint32_t, const uint32_t*,
                                       uint32_t) override {
        return 1;
    }
    Lur::Render::TextureHandle LoadTexture(const uint8_t*, int, int,
                                           Lur::Render::ETextureFormat) override {
        return 1;
    }
    void BeginFrame(const Lur::Render::Camera&) override {}
    void DrawMesh(Lur::Render::MeshHandle, Lur::Render::MaterialHandle,
                  const Lur::Math::Mat4&) override {}
    void EndFrame() override {}

private:
    Lur::Render::MaterialHandle Next = 0;
};

}  // namespace

// ---- The three fields, each asserted for its own reason -----------------------------------------
static void TestFlatMaterialContract() {
    CapturingRenderer R;
    const Color Want{0.25f, 0.5f, 0.75f, 0.4f};   // deliberately NOT opaque
    const Lur::Render::MaterialHandle H = Lur::Render::MakeFlatMaterial(&R, Want);

    CHECK(H != 0);                    // a usable handle came back
    CHECK(R.Descs.size() == 1);       // exactly one material, not one per channel or per call retry
    if (R.Descs.empty()) return;
    const MaterialDesc& D = R.Descs[0];

    // No texture: handle 0 is "flat white", which is what makes the tint the colour.
    CHECK(D.BaseColor == 0);
    // Tint passes through bit-exact on all four channels.
    CHECK(D.Tint.R == Want.R);
    CHECK(D.Tint.G == Want.G);
    CHECK(D.Tint.B == Want.B);
    CHECK(D.Tint.A == Want.A);        // the one most likely to be dropped, and the most visible
    // Unlit: a 2D quad has no meaningful normal.
    CHECK(D.Lit == false);

    // The recolour path must stay OFF. InkHi <= InkLo disables it (see MaterialDesc); a flat colour
    // that accidentally enabled the ink band would get its darker half replaced by the outline
    // colour — which on a solid panel means a band of black across it.
    CHECK(!(D.InkHi > D.InkLo));
    CHECK(D.Gamma == 1.0f);           // 1 = linear, i.e. the tint is the colour you asked for
}

// ---- Alpha is not clamped or premultiplied on the way through ----------------------------------
// Called out separately because "it looked right" is the usual defence here: a fully-opaque test
// colour cannot tell a passthrough from a clamp-to-1.
static void TestTranslucentTintSurvives() {
    CapturingRenderer R;
    Lur::Render::MakeFlatMaterial(&R, Color{1.0f, 1.0f, 1.0f, 0.0f});     // fully transparent
    Lur::Render::MakeFlatMaterial(&R, Color{0.0f, 0.0f, 0.0f, 0.62f});    // the console's panel alpha
    CHECK(R.Descs.size() == 2);
    if (R.Descs.size() < 2) return;
    CHECK(R.Descs[0].Tint.A == 0.0f);
    CHECK(R.Descs[1].Tint.A == 0.62f);
    // ...and an RGB of 0 is not confused for "unset" and replaced with white.
    CHECK(R.Descs[1].Tint.R == 0.0f);
    CHECK(R.Descs[1].Tint.G == 0.0f);
    CHECK(R.Descs[1].Tint.B == 0.0f);
}

// ---- Repeated calls yield DISTINCT materials --------------------------------------------------
// Callers hold several at once (the console alone keeps a panel, a key face, an accent, a white and a
// ring of sixteen swatches). If two calls collapsed to one handle, retinting one would move another.
static void TestEachCallIsItsOwnMaterial() {
    CapturingRenderer R;
    // CONSECUTIVE identical colours, deliberately. An A,B,A order would pass even against a
    // one-entry cache (by the time the second A arrives the cache holds B), so it cannot detect the
    // most natural version of this "optimisation" — which is exactly the sabotage that slipped
    // through the first time this test was written.
    const auto A = Lur::Render::MakeFlatMaterial(&R, Color{1, 0, 0, 1});
    const auto B = Lur::Render::MakeFlatMaterial(&R, Color{1, 0, 0, 1});   // same colour, back to back
    const auto C = Lur::Render::MakeFlatMaterial(&R, Color{0, 1, 0, 1});
    CHECK(A != B);   // same colour must NOT be deduplicated into one handle
    CHECK(A != C);
    CHECK(B != C);
    CHECK(R.Descs.size() == 3);   // three calls, three materials — no caching
}

int main() {
    TestFlatMaterialContract();
    TestTranslucentTintSurvives();
    TestEachCallIsItsOwnMaterial();
    if (GFailures == 0) std::printf("render_sprite2d_tests: ALL PASS\n");
    else std::printf("render_sprite2d_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
