#include "splatkit/camera/Lens.h"

#include <cmath>

#include <gtest/gtest.h>

namespace splatkit {
namespace {

constexpr float kPi = 3.14159265358979f;

TEST(Lens, ProjectionIsThePerspectiveOfTheViewShape) {
  const Lens lens;
  const splat::Mat4 proj = lens.projection({200, 100});
  const splat::Mat4 expected =
      splat::Mat4::perspective(lens.fieldOfViewRadians, 2.0f, lens.nearPlane, lens.farPlane);
  for (size_t i = 0; i < expected.m.size(); ++i) EXPECT_FLOAT_EQ(proj.m[i], expected.m[i]);
}

TEST(Lens, TanHalfFollowsTheAspect) {
  const Lens lens;
  const Lens::TanHalf tan = lens.tanHalf({200, 100});
  EXPECT_FLOAT_EQ(tan.y, std::tan(lens.fieldOfViewRadians * 0.5f));
  EXPECT_FLOAT_EQ(tan.x, 2.0f * tan.y);
}

TEST(Lens, FramingRadiusFitsTheBoundingSphereWithPadding) {
  const Lens lens;
  splat::Bounds bounds;
  bounds.min = {-1, -1, -1};
  bounds.max = {1, 1, 1};
  const float sphere = std::sqrt(3.0f);
  // Square view: the vertical half angle is the limiting one.
  const float halfY = lens.fieldOfViewRadians * 0.5f;
  EXPECT_NEAR(lens.framingRadius(bounds, {100, 100}), sphere * 1.05f / std::sin(halfY), 1e-4f);
  // Portrait: the narrower horizontal half angle limits instead, so the radius grows.
  EXPECT_GT(lens.framingRadius(bounds, {50, 100}), lens.framingRadius(bounds, {100, 100}));
}

TEST(Lens, FramingNeverGetsCloserThanTheMinimumRadiusAndSurvivesAnEmptyView) {
  const Lens lens;
  splat::Bounds point;
  point.min = {0, 0, 0};
  point.max = {0, 0, 0};
  EXPECT_FLOAT_EQ(lens.framingRadius(point, {100, 100}), 0.05f);
  splat::Bounds bounds;
  bounds.min = {-1, -1, -1};
  bounds.max = {1, 1, 1};
  EXPECT_TRUE(std::isfinite(lens.framingRadius(bounds, {0, 0})));
  EXPECT_GT(lens.framingRadius(bounds, {0, 0}), 0.05f);
  (void)kPi;
}

}  // namespace
}  // namespace splatkit
