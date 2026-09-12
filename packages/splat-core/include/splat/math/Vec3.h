#pragma once

#include <algorithm>
#include <cmath>

namespace splat {

struct Vec3 {
  float x = 0, y = 0, z = 0;

  Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3& operator+=(Vec3 o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
  }
  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
  float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

inline float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 v) {
  return std::sqrt(dot(v, v));
}
inline Vec3 normalize(Vec3 v) {
  const float len = length(v);
  return len > 0 ? v / len : Vec3{};
}
inline Vec3 min(Vec3 a, Vec3 b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 max(Vec3 a, Vec3 b) {
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

}  // namespace splat
