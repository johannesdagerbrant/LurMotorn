#pragma once
#include <cmath>
#include "Lur/Math/Vec.h"

namespace Lur::Math {

// Column-major 4x4 matrix (matches GLSL / Vulkan). Element at (Row, Col) is
// M[Col * 4 + Row]. Projections target Vulkan clip space (depth range 0..1).
//
// These are reasonable starting implementations; they'll be validated against the
// Vulkan backend when rendering is actually wired up (the Y-axis flip Vulkan
// wants can be applied here or at the viewport).
struct Mat4 {
    float M[16] = {1, 0, 0, 0,
                   0, 1, 0, 0,
                   0, 0, 1, 0,
                   0, 0, 0, 1};

    static Mat4 Identity() { return Mat4{}; }

    Mat4 operator*(const Mat4& O) const {
        Mat4 R;
        for (int Col = 0; Col < 4; ++Col) {
            for (int Row = 0; Row < 4; ++Row) {
                float Sum = 0.0f;
                for (int K = 0; K < 4; ++K) {
                    Sum += M[K * 4 + Row] * O.M[Col * 4 + K];
                }
                R.M[Col * 4 + Row] = Sum;
            }
        }
        return R;
    }

    static Mat4 Translation(Vec3 T) {
        Mat4 R;
        R.M[12] = T.X; R.M[13] = T.Y; R.M[14] = T.Z;
        return R;
    }

    static Mat4 Scale(Vec3 S) {
        Mat4 R;
        R.M[0] = S.X; R.M[5] = S.Y; R.M[10] = S.Z;
        return R;
    }

    // Right-handed orthographic projection into Vulkan clip space (depth 0..1).
    static Mat4 Ortho(float Left, float Right, float Bottom, float Top,
                      float Near, float Far) {
        Mat4 R;
        R.M[0]  = 2.0f / (Right - Left);
        R.M[5]  = 2.0f / (Top - Bottom);
        R.M[10] = 1.0f / (Near - Far);
        R.M[12] = (Left + Right) / (Left - Right);
        R.M[13] = (Bottom + Top) / (Bottom - Top);
        R.M[14] = Near / (Near - Far);
        return R;
    }

    // Right-handed perspective projection into Vulkan clip space (depth 0..1).
    static Mat4 Perspective(float FovYRadians, float Aspect, float Near, float Far) {
        const float F = 1.0f / std::tan(FovYRadians * 0.5f);
        Mat4 R;
        for (float& E : R.M) E = 0.0f;
        R.M[0]  = F / Aspect;
        R.M[5]  = F;
        R.M[10] = Far / (Near - Far);
        R.M[11] = -1.0f;
        R.M[14] = (Near * Far) / (Near - Far);
        return R;
    }

    static Mat4 LookAt(Vec3 Eye, Vec3 Target, Vec3 Up) {
        const Vec3 Fwd    = Normalize(Target - Eye);
        const Vec3 Right  = Normalize(Cross(Fwd, Up));
        const Vec3 TrueUp = Cross(Right, Fwd);
        Mat4 R;
        R.M[0] = Right.X;  R.M[4] = Right.Y;  R.M[8]  = Right.Z;
        R.M[1] = TrueUp.X; R.M[5] = TrueUp.Y; R.M[9]  = TrueUp.Z;
        R.M[2] = -Fwd.X;   R.M[6] = -Fwd.Y;   R.M[10] = -Fwd.Z;
        R.M[12] = -Dot(Right, Eye);
        R.M[13] = -Dot(TrueUp, Eye);
        R.M[14] = Dot(Fwd, Eye);
        return R;
    }
};

} // namespace Lur::Math
