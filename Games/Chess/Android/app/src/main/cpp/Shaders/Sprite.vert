#version 450

// 2D sprite / board-quad vertex shader. Matches Lur::Render::Vertex exactly
// (Position, Normal, Uv, Color) so one vertex layout serves 2D and 3D. The MVP
// and per-draw tint arrive as push constants — no descriptor sets needed until
// textures land (1c).
layout(location = 0) in vec3 InPosition;
layout(location = 1) in vec3 InNormal;
layout(location = 2) in vec2 InUv;
layout(location = 3) in vec4 InColor;

layout(push_constant) uniform Push {
    mat4 Mvp;
    vec4 Tint;
} P;

layout(location = 0) out vec2 OutUv;
layout(location = 1) out vec4 OutColor;

void main() {
    gl_Position = P.Mvp * vec4(InPosition, 1.0);
    OutUv = InUv;
    OutColor = InColor * P.Tint;
}
