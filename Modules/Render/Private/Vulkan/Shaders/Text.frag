#version 450

// MSDF text fragment shader. Reconstructs crisp, scalable glyph coverage from a
// multi-channel signed distance field (msdf-atlas-gen), so text stays smooth at any
// size/resolution — median(r,g,b) recovers the true edge (keeping sharp corners), and
// screenPxRange() derives the screen-space smoothing width from the field's atlas-texel
// range and the on-screen derivative (fwidth), so no CPU-side pixel-size is needed.
//
// Shares the pipeline layout with Sprite.*: Mvp+Tint at offsets 0/64, plus DistanceRange
// (the msdfgen -pxrange, in atlas texels) at offset 80. InColor already carries the tint.
layout(set = 0, binding = 0) uniform sampler2D Msdf;

layout(push_constant) uniform Push {
    mat4  Mvp;
    vec4  Tint;
    float DistanceRange;
} P;

layout(location = 0) in vec2 InUv;
layout(location = 1) in vec4 InColor;

layout(location = 0) out vec4 OutColor;

float Median(float R, float G, float B) {
    return max(min(R, G), min(max(R, G), B));
}

float ScreenPxRange() {
    vec2 UnitRange     = vec2(P.DistanceRange) / vec2(textureSize(Msdf, 0));
    vec2 ScreenTexSize = vec2(1.0) / fwidth(InUv);
    return max(0.5 * dot(UnitRange, ScreenTexSize), 1.0);
}

void main() {
    vec3  S     = texture(Msdf, InUv).rgb;
    float Sd    = Median(S.r, S.g, S.b);
    float Px    = ScreenPxRange();
    float Alpha = clamp((Sd - 0.5) * Px + 0.5, 0.0, 1.0);
    OutColor = vec4(InColor.rgb, InColor.a * Alpha);
}
