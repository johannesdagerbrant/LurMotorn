#version 450

// Instanced unit renderer (RTS): ONE draw for all units. Binding 0 is the shared unit
// quad (only Position is consumed); binding 1 is per-instance data at INSTANCE rate.
// The unit's centre interpolates Prev->Cur by the alpha push constant IN THE SHADER
// (design doc §6: CPU per-unit interpolation cost is zero). Everything is pixel space
// — the game maps world->pixels when building instances — so the renderer stays generic
// (mix + MVP, exactly like the sprite/text pipelines).
layout(location = 0) in vec3 InPosition;   // unit quad corner, xy in [0,1]

layout(location = 4) in vec2  InPrev;      // per-instance: pixel centre, previous tick
layout(location = 5) in vec2  InCur;       //               pixel centre, current tick
layout(location = 6) in vec4  InColor;
layout(location = 7) in float InSize;      // pixel size (quad spans Size x Size, centred)

layout(push_constant) uniform Push {
    mat4 Mvp;
    vec4 Tint;
    vec4 Outline;
    vec4 Shape;    // x = interpolation alpha
} P;

layout(location = 0) out vec4 OutColor;

void main() {
    vec2 centre = mix(InPrev, InCur, P.Shape.x);
    vec2 corner = centre + (InPosition.xy - vec2(0.5)) * InSize;
    gl_Position = P.Mvp * vec4(corner, 0.0, 1.0);
    OutColor = InColor;
}
