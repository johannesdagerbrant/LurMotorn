#version 450

// Unlit fragment shader for 2D quads. Samples a base-colour texture and
// multiplies by the interpolated vertex colour (which already carries the
// material tint from the vertex stage). Board squares bind a 1x1 white texture,
// so they come out as the flat tint; pieces bind an alpha-mask silhouette, so
// the tint colours the silhouette and the mask's alpha composites it over the
// board via straight alpha blending.
layout(set = 0, binding = 0) uniform sampler2D BaseColor;

layout(location = 0) in vec2 InUv;
layout(location = 1) in vec4 InColor;

layout(location = 0) out vec4 OutColor;

void main() {
    OutColor = texture(BaseColor, InUv) * InColor;
}
