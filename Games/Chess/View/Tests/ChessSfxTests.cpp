// Host tests for the chess SFX variation policy (issue #78): an event owns a group of
// interchangeable clips, and the frequent events must never play the same one twice in a
// row. Pure logic — no audio device, no cooked-clip decode needed for the picker itself.
#include <cstdio>

#include "Chess/View/MoveSound.h"
#include "Chess/View/SfxLibrary.h"

using namespace Chess;

static int GFailures = 0;
#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// The frequent events are backed by a group; the alerts are single, fixed signatures.
static void TestVariantCounts() {
    CHECK(SfxLibrary::VariantCount(EMoveSound::Move) == 3);
    CHECK(SfxLibrary::VariantCount(EMoveSound::Capture) == 3);
    CHECK(SfxLibrary::VariantCount(EMoveSound::Check) == 1);
    CHECK(SfxLibrary::VariantCount(EMoveSound::Checkmate) == 1);
}

// The whole point: two identical clicks back to back is the artefact this replaced, so a
// repeat must be IMPOSSIBLE, not merely unlikely — uniform random would still do it one
// time in three. Also check every variant is reachable, so a modulo slip that only ever
// picks two of the three doesn't pass silently.
static void TestNoImmediateRepeat() {
    for (EMoveSound E : {EMoveSound::Move, EMoveSound::Capture}) {
        SfxLibrary L;
        const int N = SfxLibrary::VariantCount(E);
        int Seen[8] = {0};
        int Prev = -1;
        for (int i = 0; i < 500; ++i) {
            const int P = L.PickVariant(E);
            CHECK(P >= 0 && P < N);          // never off the end of the group
            CHECK(P != Prev);                // never the same clip twice running
            if (P >= 0 && P < 8) ++Seen[P];
            Prev = P;
        }
        for (int v = 0; v < N; ++v) CHECK(Seen[v] > 0);   // every variant actually reachable
    }
}

// A single-clip alert has nothing to choose: it must return the one slot, every time,
// rather than walking off the end of a group of size 1.
static void TestAlertsAreFixed() {
    SfxLibrary L;
    for (EMoveSound E : {EMoveSound::Check, EMoveSound::Checkmate})
        for (int i = 0; i < 20; ++i) CHECK(L.PickVariant(E) == 0);
}

// Each event keeps its OWN no-repeat memory — interleaving moves and captures (what a real
// game does) must not let one event's history suppress or force the other's choice.
static void TestPerEventMemoryIsIndependent() {
    SfxLibrary L;
    int PrevMove = -1, PrevCap = -1;
    for (int i = 0; i < 200; ++i) {
        const int M = L.PickVariant(EMoveSound::Move);
        const int C = L.PickVariant(EMoveSound::Capture);
        CHECK(M != PrevMove);
        CHECK(C != PrevCap);
        PrevMove = M;
        PrevCap = C;
    }
}

int main() {
    TestVariantCounts();
    TestNoImmediateRepeat();
    TestAlertsAreFixed();
    TestPerEventMemoryIsIndependent();

    if (GFailures == 0) {
        std::printf("All chess SFX tests passed.\n");
        return 0;
    }
    std::printf("%d chess SFX test(s) failed.\n", GFailures);
    return 1;
}
