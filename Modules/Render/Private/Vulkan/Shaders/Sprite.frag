#version 450

// Unlit fragment shader for 2D quads. The texture carries two meaningful channels:
//   R = shade  (source-art luminance / tonal detail, 0..1)
//   G = coverage (silhouette alpha for straight-alpha compositing)
// The interpolated vertex colour already carries the material tint. Piece sprites
// bind an R8G8 shade+coverage texture, so the tint colours the art while R keeps
// the internal tones (highlights, mid-tones, dark outline) instead of a flat blob
// (issue #30). Board squares / UI bind a 1x1 white RGBA texture, which reads
// R=G=1, so they come out as the flat, fully-opaque tint — the same as before.
layout(set = 0, binding = 0) uniform sampler2D BaseColor;

layout(location = 0) in vec2 InUv;
layout(location = 1) in vec4 InColor;

layout(location = 0) out vec4 OutColor;

void main() {
    vec2 Tex = texture(BaseColor, InUv).rg;   // R = shade, G = coverage
    OutColor = vec4(InColor.rgb * Tex.r, InColor.a * Tex.g);
}
