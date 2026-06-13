#pragma once
#include <cmath>

namespace Lur::Math {

// Float vectors for rendering and scene transforms (NOT gameplay simulation —
// that uses the deterministic Lur::Sim::Fixed type).

struct Vec2 {
    float X = 0.0f, Y = 0.0f;
};

struct Vec3 {
    float X = 0.0f, Y = 0.0f, Z = 0.0f;

    constexpr Vec3 operator+(Vec3 O) const { return {X + O.X, Y + O.Y, Z + O.Z}; }
    constexpr Vec3 operator-(Vec3 O) const { return {X - O.X, Y - O.Y, Z - O.Z}; }
    constexpr Vec3 operator*(float S) const { return {X * S, Y * S, Z * S}; }
};

struct Vec4 {
    float X = 0.0f, Y = 0.0f, Z = 0.0f, W = 0.0f;
};

constexpr float Dot(Vec3 A, Vec3 B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }

constexpr Vec3 Cross(Vec3 A, Vec3 B) {
    return {A.Y * B.Z - A.Z * B.Y,
            A.Z * B.X - A.X * B.Z,
            A.X * B.Y - A.Y * B.X};
}

inline float Length(Vec3 V) { return std::sqrt(Dot(V, V)); }

inline Vec3 Normalize(Vec3 V) {
    const float Len = Length(V);
    return Len > 0.0f ? V * (1.0f / Len) : V;
}

} // namespace Lur::Math
