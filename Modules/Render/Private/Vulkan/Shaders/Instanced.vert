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
layout(location = 8) in vec4  InUvRect;    // atlas UV rect (u0,v0,u1,v1) for this glyph
layout(location = 9) in vec2  InFace;      // facing dir (pixel space); (0,0) = upright

layout(push_constant) uniform Push {
    mat4 Mvp;
    vec4 Tint;
    vec4 Outline;
    vec4 Shape;    // x = interpolation alpha
} P;

layout(location = 0) out vec4 OutColor;
layout(location = 1) out vec2 OutUv;

// If InFace is non-zero, orient the quad so the glyph's TOP edge (its "forward") points
// along InFace; (0,0) draws upright. The caller decides what facing means — here the game
// passes a velocity/target blend so units point where they move, and toward their quarry
// when they slow. Rotating only the corner offset (not the UVs) turns the whole sprite.
void main() {
    vec2 centre = mix(InPrev, InCur, P.Shape.x);
    vec2 off = (InPosition.xy - vec2(0.5)) * InSize;
    if (dot(InFace, InFace) > 0.0001) {
        vec2 dir = normalize(InFace);
        float c = -dir.y, s = dir.x;             // rotation mapping local up (0,-1) -> dir
        off = vec2(c * off.x - s * off.y, s * off.x + c * off.y);
    }
    gl_Position = P.Mvp * vec4(centre + off, 0.0, 1.0);
    OutColor = InColor;
    OutUv = mix(InUvRect.xy, InUvRect.zw, InPosition.xy);
}
