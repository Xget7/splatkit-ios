#include "splatkit/camera/Lens.h"

#include <algorithm>
#include <cmath>

namespace splatkit {
namespace {

// The default orbit frames the bounding sphere with 5% to spare, never closer than the
// minimum radius, and treats an aspect below the minimum as that, so a zero-width view
// still frames.
constexpr float kFramingPadding = 1.05f;
constexpr float kMinFramingRadius = 0.05f;
constexpr float kMinFramingAspect = 1e-3f;

float aspectOf(Extent extent) {
  return extent.width > 0 && extent.height > 0
             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
             : 1.0f;
}

}  // namespace

Lens::TanHalf Lens::tanHalf(Extent extent) const {
  const float y = std::tan(fieldOfViewRadians * 0.5f);
  return {y * aspectOf(extent), y};
}

splat::Mat4 Lens::projection(Extent extent) const {
  return splat::Mat4::perspective(fieldOfViewRadians, aspectOf(extent), nearPlane, farPlane);
}

float Lens::framingRadius(const splat::Bounds& bounds, Extent extent) const {
  const splat::Vec3 half{(bounds.max[0] - bounds.min[0]) * 0.5f,
                         (bounds.max[1] - bounds.min[1]) * 0.5f,
                         (bounds.max[2] - bounds.min[2]) * 0.5f};
  const float sphere = splat::length(half);
  const float halfY = fieldOfViewRadians * 0.5f;
  const float halfX = std::atan(std::tan(halfY) * std::max(aspectOf(extent), kMinFramingAspect));
  return std::max(kMinFramingRadius, sphere * kFramingPadding / std::sin(std::min(halfX, halfY)));
}

}  // namespace splatkit
