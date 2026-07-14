#include "Lur/Text/TextLayout.h"

namespace Lur::Text {
namespace {

constexpr int   MaxLines    = 64;
constexpr float OverflowEps = 0.5f;   // px slack so float noise isn't "overflow"

struct LineRange { int Start; int End; };   // [Start, End) byte range; break-space excluded

uint32_t NormCp(char C) {
    const uint32_t Cp = static_cast<unsigned char>(C);
    return (C == '\t') ? uint32_t{' '} : Cp;   // tabs render as a space
}

// The glyph to render for a code point: the glyph itself, else '?', else nullptr.
const CookedGlyph* GlyphFor(const Font& F, uint32_t Cp) {
    if (const CookedGlyph* G = F.Find(Cp)) return G;
    return F.Find('?');
}

float AdvanceEm(const Font& F, uint32_t Cp) {
    const CookedGlyph* G = GlyphFor(F, Cp);
    return G ? G->Advance : 0.0f;
}

bool HasQuad(const CookedGlyph& G) { return G.UvRight > G.UvLeft; }

// Greedy word-wrap into line ranges. Breaks on '\n' (hard) and spaces (soft); a word
// longer than MaxWidthEm falls back to a character break. Returns the line count;
// sets Truncated if it ran out of line slots.
int WrapLines(const Font& F, const char* Text, int Len, float MaxWidthEm, bool Wrap,
              LineRange* Out, bool& Truncated) {
    int   Count = 0;
    int   Start = 0;
    float WidthEm = 0.0f;
    int   LastSpace = -1;

    auto Push = [&](int S, int E) {
        if (Count >= MaxLines) { Truncated = true; return; }
        Out[Count++] = {S, E};
    };

    int i = 0;
    while (i < Len && Count < MaxLines) {
        const char Ch = Text[i];
        if (Ch == '\r') { ++i; continue; }
        if (Ch == '\n') {                       // hard break
            Push(Start, i);
            Start = i + 1; ++i; WidthEm = 0.0f; LastSpace = -1;
            continue;
        }
        const uint32_t Cp  = NormCp(Ch);
        const float    Adv = AdvanceEm(F, Cp);

        if (Wrap && WidthEm + Adv > MaxWidthEm && i > Start) {
            if (LastSpace >= 0 && LastSpace > Start) {   // break at the last space
                Push(Start, LastSpace);
                Start = LastSpace + 1; i = Start;        // re-scan the tail
            } else {                                      // unbreakable word: char break
                Push(Start, i);
                Start = i;                                // re-process this glyph next line
            }
            WidthEm = 0.0f; LastSpace = -1;
            continue;
        }
        if (Cp == ' ') LastSpace = i;
        WidthEm += Adv;
        ++i;
    }
    if (Count < MaxLines) Push(Start, Len);
    else Truncated = true;
    return Count;
}

float LineWidthEm(const Font& F, const char* Text, const LineRange& L) {
    float W = 0.0f;
    for (int i = L.Start; i < L.End; ++i) {
        if (Text[i] == '\r') continue;
        W += AdvanceEm(F, NormCp(Text[i]));
    }
    return W;
}

} // namespace

void LayoutText(const Font& Font, const char* Text, int Len,
                const LayoutSpec& Spec, LayoutResult& Out) {
    Out = LayoutResult{};
    if (!Font.IsValid() || Text == nullptr || Len <= 0) return;

    const float Ps = Spec.PixelSize;
    const float MaxWidthEm =
        (Spec.Wrap && Spec.W > 0.0f && Ps > 0.0f) ? (Spec.W / Ps) : 1e30f;

    LineRange Lines[MaxLines];
    const int LineCount = WrapLines(Font, Text, Len, MaxWidthEm, Spec.Wrap, Lines,
                                    Out.Truncated);

    const float LineHeightPx = Font.LineHeight() * Ps;
    const float BlockH       = LineCount * LineHeightPx;

    Out.LineCount = LineCount;
    Out.HeightPx  = BlockH;
    Out.OverflowY = BlockH > Spec.H + OverflowEps;

    // Vertical block origin (top of the first line's cell).
    float YTop = Spec.Y;
    if (Spec.VAlign == EVAlign::Middle)      YTop = Spec.Y + (Spec.H - BlockH) * 0.5f;
    else if (Spec.VAlign == EVAlign::Bottom) YTop = Spec.Y + (Spec.H - BlockH);

    for (int li = 0; li < LineCount; ++li) {
        const LineRange& L = Lines[li];
        const float LineWEm = LineWidthEm(Font, Text, L);
        const float LineWPx = LineWEm * Ps;
        if (LineWPx > Out.WidthPx) Out.WidthPx = LineWPx;
        if (LineWPx > Spec.W + OverflowEps) Out.OverflowX = true;

        // Horizontal pen origin.
        float PenX = Spec.X;
        if (Spec.HAlign == EHAlign::Center)     PenX = Spec.X + (Spec.W - LineWPx) * 0.5f;
        else if (Spec.HAlign == EHAlign::Right) PenX = Spec.X + (Spec.W - LineWPx);

        // Baseline for this line (Ascender is negative in y-down, so -Ascender > 0).
        const float BaselineY = YTop - Font.Ascender() * Ps + li * LineHeightPx;

        for (int k = L.Start; k < L.End; ++k) {
            if (Text[k] == '\r') continue;
            const uint32_t Cp = NormCp(Text[k]);
            const CookedGlyph* G = GlyphFor(Font, Cp);
            if (G == nullptr) continue;
            if (HasQuad(*G)) {
                if (Out.Count >= LayoutResult::MaxGlyphs) { Out.Truncated = true; }
                else {
                    PlacedGlyph& P = Out.Glyphs[Out.Count++];
                    P.X0 = PenX + G->PlaneLeft   * Ps;
                    P.X1 = PenX + G->PlaneRight  * Ps;
                    P.Y0 = BaselineY + G->PlaneTop    * Ps;   // PlaneTop < 0 -> above baseline
                    P.Y1 = BaselineY + G->PlaneBottom * Ps;
                    P.U0 = G->UvLeft;  P.V0 = G->UvTop;
                    P.U1 = G->UvRight; P.V1 = G->UvBottom;
                }
            }
            PenX += G->Advance * Ps;
        }
    }
}

void MeasureText(const Font& Font, const char* Text, int Len,
                 float MaxWidthPx, float PixelSize, bool Wrap,
                 float& OutWidthPx, float& OutHeightPx, int& OutLineCount) {
    OutWidthPx = OutHeightPx = 0.0f;
    OutLineCount = 0;
    if (!Font.IsValid() || Text == nullptr || Len <= 0) return;

    const float MaxWidthEm =
        (Wrap && MaxWidthPx > 0.0f && PixelSize > 0.0f) ? (MaxWidthPx / PixelSize) : 1e30f;

    LineRange Lines[MaxLines];
    bool Truncated = false;
    const int LineCount = WrapLines(Font, Text, Len, MaxWidthEm, Wrap, Lines, Truncated);

    float MaxW = 0.0f;
    for (int li = 0; li < LineCount; ++li) {
        const float W = LineWidthEm(Font, Text, Lines[li]) * PixelSize;
        if (W > MaxW) MaxW = W;
    }
    OutWidthPx   = MaxW;
    OutHeightPx  = LineCount * Font.LineHeight() * PixelSize;
    OutLineCount = LineCount;
}

} // namespace Lur::Text
