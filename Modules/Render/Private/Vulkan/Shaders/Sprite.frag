#version 450

// Unlit fragment shader for 2D quads. Generic + parametrised: the game drives the
// look per material (push constants), so no game-specific logic lives here. The
// texture carries two channels: R = shade (art luminance / tonal detail), G =
// coverage (silhouette alpha). The interpolated vertex colour carries the fill Tint.
//
// Two engine knobs shape the sprite (see MaterialDesc):
//   * Shape.z = Gamma  -> tone curve on the fill's shade. 1 = linear; >1 deepens the
//     darker tones while highlights (shade ~1) stay put (pow(1, g) = 1).
//   * Shape.xy = InkLo/InkHi + Outline -> a two-band recolour: shade below the band
//     blends to Outline, above it stays the tinted fill. Disabled (plain tint*shade)
//     when InkHi <= InkLo. Board squares / UI use a 1x1 white texture (shade = 1), so
//     the ink band never triggers and Gamma is a no-op — they come out as flat Tint.
// Chess uses these to render white AND black pieces from one mask set (issue #30):
// white = dark outline + gentle gamma, black = white outline + steep gamma.
layout(set = 0, binding = 0) uniform sampler2D BaseColor;

layout(push_constant) uniform Push {
    mat4 Mvp;
    vec4 Tint;
    vec4 Outline;   // colour the dark "ink" band maps to
    vec4 Shape;     // x = InkLo, y = InkHi, z = Gamma, w = (unused by sprites)
} P;

layout(location = 0) in vec2 InUv;
layout(location = 1) in vec4 InColor;

layout(location = 0) out vec4 OutColor;

void main() {
    vec2  Tex   = texture(BaseColor, InUv).rg;                 // R = shade, G = coverage
    float Shade = Tex.r;
    vec3  Fill  = InColor.rgb * pow(Shade, P.Shape.z);         // gamma-shaped tinted fill
    float Ink   = (P.Shape.y > P.Shape.x)
                    ? (1.0 - smoothstep(P.Shape.x, P.Shape.y, Shade))
                    : 0.0;                                     // ink band (disabled by default)
    vec3  Rgb   = mix(Fill, P.Outline.rgb, Ink);
    OutColor = vec4(Rgb, InColor.a * Tex.g);
}
