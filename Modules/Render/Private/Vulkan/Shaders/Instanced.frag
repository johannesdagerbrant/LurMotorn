#version 450

// Atlas-sampled instance (RTS units, #85): the cooked R8G8 glyph atlas supplies
// R = shade, G = coverage; the per-instance colour is the fill tint. Silhouette-tint
// path: colour = tint x shade, alpha = tint.a x coverage — so a white-glyph atlas
// draws pure team-coloured alpha cutouts. A flat material binds the default 1x1
// white texture (shade = coverage = 1), which degrades to the old plain tinted quad.
layout(location = 0) in vec4 InColor;
layout(location = 1) in vec2 InUv;

layout(set = 0, binding = 0) uniform sampler2D BaseColor;

layout(location = 0) out vec4 OutColor;

void main() {
    vec2 s = texture(BaseColor, InUv).rg;
    OutColor = vec4(InColor.rgb * s.r, InColor.a * s.g);
}
