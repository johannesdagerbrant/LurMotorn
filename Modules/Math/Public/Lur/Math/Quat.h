#pragma once
#include <cmath>
#include "Lur/Math/Mat4.h"
#include "Lur/Math/Vec.h"

namespace Lur::Math {

// Unit quaternion for 3D rotations (orientation, skeletal animation later).
struct Quat {
    float X = 0.0f, Y = 0.0f, Z = 0.0f, W = 1.0f;  // identity

    static Quat FromAxisAngle(Vec3 Axis, float Radians) {
        const Vec3  N    = Normalize(Axis);
        const float Half = Radians * 0.5f;
        const float S    = std::sin(Half);
        return {N.X * S, N.Y * S, N.Z * S, std::cos(Half)};
    }

    // Hamilton product (compose rotations: this then O, applied right-to-left).
    Quat operator*(const Quat& O) const {
        return {
            W * O.X + X * O.W + Y * O.Z - Z * O.Y,
            W * O.Y - X * O.Z + Y * O.W + Z * O.X,
            W * O.Z + X * O.Y - Y * O.X + Z * O.W,
            W * O.W - X * O.X - Y * O.Y - Z * O.Z,
        };
    }

    Mat4 ToMat4() const {
        const float Xx = X * X, Yy = Y * Y, Zz = Z * Z;
        const float Xy = X * Y, Xz = X * Z, Yz = Y * Z;
        const float Wx = W * X, Wy = W * Y, Wz = W * Z;
        Mat4 R;
        R.M[0] = 1.0f - 2.0f * (Yy + Zz); R.M[1] = 2.0f * (Xy + Wz);        R.M[2]  = 2.0f * (Xz - Wy);
        R.M[4] = 2.0f * (Xy - Wz);        R.M[5] = 1.0f - 2.0f * (Xx + Zz); R.M[6]  = 2.0f * (Yz + Wx);
        R.M[8] = 2.0f * (Xz + Wy);        R.M[9] = 2.0f * (Yz - Wx);        R.M[10] = 1.0f - 2.0f * (Xx + Yy);
        return R;
    }
};

} // namespace Lur::Math
