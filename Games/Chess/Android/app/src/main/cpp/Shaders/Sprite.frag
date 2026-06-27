#version 450

// Unlit fragment shader for 2D quads. For 1b this just emits the interpolated
// vertex colour (already multiplied by the material tint in the vertex stage),
// which is all a flat-coloured board needs. 1c multiplies in a sampled texture
// for the pieces.
layout(location = 0) in vec2 InUv;
layout(location = 1) in vec4 InColor;

layout(location = 0) out vec4 OutColor;

void main() {
    OutColor = InColor;
}
