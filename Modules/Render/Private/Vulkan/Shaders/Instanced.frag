#version 450

// Flat-tinted instance (RTS units, bring-up). Colour arrives per-instance from the
// vertex shader; no texture sample yet — the cooked R8G8 sprite atlas + per-instance
// Type/Team UV selection is a later pass, and slots in here without touching the
// pipeline wiring.
layout(location = 0) in vec4 InColor;
layout(location = 0) out vec4 OutColor;

void main() {
    OutColor = InColor;
}
