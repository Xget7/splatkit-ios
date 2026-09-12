#pragma once

#include <array>
#include <cmath>

#include "splat/math/Vec3.h"

namespace splat {

// 4x4 float matrix, column major like GLSL, Metal and simd: m[col * 4 + row].
// Conventions shared by every renderer: right handed, camera looks down -Z, +Y up,
// clip depth in [0, 1] (Vulkan and Metal).
struct Mat4 {
  std::array<float, 16> m{};

  static Mat4 identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }

  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }

  static Mat4 translation(Vec3 t) {
    Mat4 r = identity();
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
  }

  // Rotation of `radians` about a unit axis (Rodrigues).
  static Mat4 rotation(float radians, Vec3 axis) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float t = 1 - c;
    const float x = axis.x;
    const float y = axis.y;
    const float z = axis.z;
    Mat4 r = identity();
    r.at(0, 0) = t * x * x + c;
    r.at(0, 1) = t * x * y - s * z;
    r.at(0, 2) = t * x * z + s * y;
    r.at(1, 0) = t * x * y + s * z;
    r.at(1, 1) = t * y * y + c;
    r.at(1, 2) = t * y * z - s * x;
    r.at(2, 0) = t * x * z - s * y;
    r.at(2, 1) = t * y * z + s * x;
    r.at(2, 2) = t * z * z + c;
    return r;
  }

  // Perspective with vertical field of view, mapping z in [-near, -far] to depth [0, 1].
  static Mat4 perspective(float fovyRadians, float aspect, float near, float far) {
    const float ys = 1 / std::tan(fovyRadians * 0.5f);
    const float xs = ys / aspect;
    const float zs = far / (near - far);
    Mat4 r;
    r.at(0, 0) = xs;
    r.at(1, 1) = ys;
    r.at(2, 2) = zs;
    r.at(2, 3) = near * zs;
    r.at(3, 2) = -1;
    return r;
  }

  Mat4 transposed() const {
    Mat4 r;
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col) r.at(row, col) = at(col, row);
    return r;
  }

  // Inverse of a rigid transform (rotation + translation): R^T and -R^T t.
  Mat4 rigidInverse() const {
    Mat4 r = identity();
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col) r.at(row, col) = at(col, row);
    const float tx = at(0, 3);
    const float ty = at(1, 3);
    const float tz = at(2, 3);
    r.at(0, 3) = -(r.at(0, 0) * tx + r.at(0, 1) * ty + r.at(0, 2) * tz);
    r.at(1, 3) = -(r.at(1, 0) * tx + r.at(1, 1) * ty + r.at(1, 2) * tz);
    r.at(2, 3) = -(r.at(2, 0) * tx + r.at(2, 1) * ty + r.at(2, 2) * tz);
    return r;
  }

  Mat4 operator*(const Mat4& b) const {
    Mat4 r;
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += at(row, k) * b.at(k, col);
        r.at(row, col) = sum;
      }
    return r;
  }

  // Transforms a point (w = 1), ignoring the projective row.
  Vec3 transformPoint(Vec3 p) const {
    return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
            at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
            at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
  }

  // Transforms a direction (w = 0).
  Vec3 transformDirection(Vec3 d) const {
    return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z,
            at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
            at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
  }

  std::array<float, 4> operator*(const std::array<float, 4>& v) const {
    std::array<float, 4> r{};
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        r[static_cast<size_t>(row)] += at(row, k) * v[static_cast<size_t>(k)];
    return r;
  }
};

}  // namespace splat
