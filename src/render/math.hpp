#pragma once

#include <array>
#include <cmath>

namespace ts2::render {

using Vec3 = std::array<float, 3>;

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 normalize(const Vec3& a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? scale(a, 1 / l) : a;
}

// A 4x4 matrix, row-major, applied to column vectors (p' = M p).
struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // Every object takes one of these each frame, so this skips the arrays' bounds checks.
    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        const float* const a = m.data();
        const float* const c = b.m.data();
        float* const out = r.m.data();
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float v = 0;
                for (int k = 0; k < 4; ++k) {
                    v += a[i * 4 + k] * c[k * 4 + j];
                }
                out[i * 4 + j] = v;
            }
        }
        return r;
    }

    // A camera at `eye` looking at `target`, with `up` as the world's up direction.
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        const Vec3 f = normalize(sub(target, eye));
        const Vec3 s = normalize(cross(f, up));
        const Vec3 u = cross(s, f);
        Mat4 r;
        r.m = {s[0],  s[1],  s[2],  -dot(s, eye),
               u[0],  u[1],  u[2],  -dot(u, eye),
               -f[0], -f[1], -f[2], dot(f, eye),
               0,     0,     0,     1};
        return r;
    }

    // A view square on, with the same depth as `perspective`: 1 at the near plane, 0 at the far one.
    static Mat4 orthographic(float left, float right, float bottom, float top, float nearZ, float farZ) {
        Mat4 r;
        r.m = {2 / (right - left), 0, 0, -(right + left) / (right - left),
               0, 2 / (top - bottom), 0, -(top + bottom) / (top - bottom),
               0, 0, 1 / (farZ - nearZ), farZ / (farZ - nearZ),
               0, 0, 0, 1};
        return r;
    }

    // The matrix undoing this one, or the identity when it has no inverse.
    Mat4 inverse() const {
        const auto& a = m;
        const float s[6] = {a[0] * a[5] - a[4] * a[1], a[0] * a[6] - a[4] * a[2], a[0] * a[7] - a[4] * a[3],
                            a[1] * a[6] - a[5] * a[2], a[1] * a[7] - a[5] * a[3], a[2] * a[7] - a[6] * a[3]};
        const float c[6] = {a[8] * a[13] - a[12] * a[9], a[8] * a[14] - a[12] * a[10], a[8] * a[15] - a[12] * a[11],
                            a[9] * a[14] - a[13] * a[10], a[9] * a[15] - a[13] * a[11], a[10] * a[15] - a[14] * a[11]};
        const float det = s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
        Mat4 r;
        if (det == 0) {
            r.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            return r;
        }
        const float d = 1 / det;
        r.m = {(a[5] * c[5] - a[6] * c[4] + a[7] * c[3]) * d, (-a[1] * c[5] + a[2] * c[4] - a[3] * c[3]) * d,
               (a[13] * s[5] - a[14] * s[4] + a[15] * s[3]) * d, (-a[9] * s[5] + a[10] * s[4] - a[11] * s[3]) * d,
               (-a[4] * c[5] + a[6] * c[2] - a[7] * c[1]) * d, (a[0] * c[5] - a[2] * c[2] + a[3] * c[1]) * d,
               (-a[12] * s[5] + a[14] * s[2] - a[15] * s[1]) * d, (a[8] * s[5] - a[10] * s[2] + a[11] * s[1]) * d,
               (a[4] * c[4] - a[5] * c[2] + a[7] * c[0]) * d, (-a[0] * c[4] + a[1] * c[2] - a[3] * c[0]) * d,
               (a[12] * s[4] - a[13] * s[2] + a[15] * s[0]) * d, (-a[8] * s[4] + a[9] * s[2] - a[11] * s[0]) * d,
               (-a[4] * c[3] + a[5] * c[1] - a[6] * c[0]) * d, (a[0] * c[3] - a[1] * c[1] + a[2] * c[0]) * d,
               (-a[12] * s[3] + a[13] * s[1] - a[14] * s[0]) * d, (a[8] * s[3] - a[9] * s[1] + a[10] * s[0]) * d};
        return r;
    }

    // Perspective projection to depth 0 (near) .. 1 (far), right-handed view space looking down -z.
    // Depth runs from 1 at the near plane to 0 at the far one (reversed), which keeps a float depth
    // buffer about as precise far away as close by.
    static Mat4 perspective(float fovY, float aspect, float nearZ, float farZ) {
        const float t = 1 / std::tan(fovY / 2);
        Mat4 r;
        r.m = {t / aspect, 0, 0, 0,
               0, t, 0, 0,
               0, 0, nearZ / (farZ - nearZ), nearZ * farZ / (farZ - nearZ),
               0, 0, -1, 0};
        return r;
    }
};

} // namespace ts2::render
