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

constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;

}  // namespace

std::optional<Lens> Lens::with(const LensSettings& settings) {
  Lens lens;
  // Each test passes only for a number in range, so NaN fails all of them.
  if (const auto degrees = settings.fieldOfViewDegrees) {
    const bool inRange = *degrees >= kMinFieldOfViewDegrees && *degrees <= kMaxFieldOfViewDegrees;
    if (!inRange) return std::nullopt;
    lens.fieldOfViewRadians = *degrees * kRadiansPerDegree;
  }
  if (const auto range = settings.depthRange) {
    const bool inRange = range->nearPlane > 0 && std::isfinite(range->farPlane) &&
                         range->farPlane > range->nearPlane &&
                         range->farPlane / range->nearPlane <= kMaxDepthRatio;
    if (!inRange) return std::nullopt;
    lens.nearPlane = range->nearPlane;
    lens.farPlane = range->farPlane;
  }
  return lens;
}

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
