#version 450

// MSDF text vertex shader. Same vertex layout as Sprite.vert (Lur::Render::Vertex),
// and the same push-constant prefix (Mvp + Tint) so both pipelines share ONE pipeline
// layout — the fragment stage additionally reads DistanceRange at offset 80. Text is
// emitted in pixel space (screen-fixed) with the ortho MVP, one quad per glyph, UVs
// and colour baked per vertex by the HUD layer.
layout(location = 0) in vec3 InPosition;
layout(location = 1) in vec3 InNormal;
layout(location = 2) in vec2 InUv;
layout(location = 3) in vec4 InColor;

layout(push_constant) uniform Push {
    mat4  Mvp;
    vec4  Tint;
} P;

layout(location = 0) out vec2 OutUv;
layout(location = 1) out vec4 OutColor;

void main() {
    gl_Position = P.Mvp * vec4(InPosition, 1.0);
    OutUv = InUv;
    OutColor = InColor * P.Tint;
}
