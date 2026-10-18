#include "splatkit/camera/Lens.h"

#include <cmath>
#include <limits>

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

TEST(Lens, NoSettingsIsTheDefaultLens) {
  const auto lens = Lens::with({});
  ASSERT_TRUE(lens.has_value());
  const Lens defaults;
  EXPECT_FLOAT_EQ(lens->fieldOfViewRadians, defaults.fieldOfViewRadians);
  EXPECT_FLOAT_EQ(lens->nearPlane, defaults.nearPlane);
  EXPECT_FLOAT_EQ(lens->farPlane, defaults.farPlane);
}

TEST(Lens, SettingsReplaceOnlyWhatTheyName) {
  LensSettings wide;
  wide.fieldOfViewDegrees = 90;
  const auto lens = Lens::with(wide);
  ASSERT_TRUE(lens.has_value());
  EXPECT_FLOAT_EQ(lens->fieldOfViewRadians, kPi / 2);
  EXPECT_FLOAT_EQ(lens->farPlane, Lens{}.farPlane);

  LensSettings deep;
  deep.depthRange = DepthRange{0.1f, 5000};
  const auto far = Lens::with(deep);
  ASSERT_TRUE(far.has_value());
  EXPECT_FLOAT_EQ(far->nearPlane, 0.1f);
  EXPECT_FLOAT_EQ(far->farPlane, 5000);
  EXPECT_FLOAT_EQ(far->fieldOfViewRadians, Lens{}.fieldOfViewRadians);
}

TEST(Lens, OutOfRangeSettingsAreRefused) {
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInfinity = std::numeric_limits<float>::infinity();
  for (const float degrees : {9.9f, 120.1f, 0.0f, kNaN, kInfinity}) {
    LensSettings settings;
    settings.fieldOfViewDegrees = degrees;
    EXPECT_FALSE(Lens::with(settings).has_value()) << degrees;
  }
  for (const DepthRange range : {DepthRange{0, 10}, DepthRange{-1, 10}, DepthRange{1, 1},
                                 DepthRange{2, 1}, DepthRange{1, kInfinity}, DepthRange{kNaN, 1},
                                 DepthRange{1, kNaN}, DepthRange{0.5f, 50001}}) {
    LensSettings settings;
    settings.depthRange = range;
    EXPECT_FALSE(Lens::with(settings).has_value()) << range.nearPlane << " " << range.farPlane;
  }
  LensSettings limits;
  limits.fieldOfViewDegrees = Lens::kMaxFieldOfViewDegrees;
  limits.depthRange = DepthRange{0.5f, 50000};
  EXPECT_TRUE(Lens::with(limits).has_value());
}

// The shaders read the planes back from the projection as near = p[3][2] / p[2][2] and
// far = p[3][2] / (p[2][2] + 1): the widest range allowed must still come back close.
TEST(Lens, TheWidestDepthRangeSurvivesTheShadersReadingItBack) {
  LensSettings settings;
  settings.depthRange = DepthRange{0.5f, 0.5f * Lens::kMaxDepthRatio};
  const auto lens = Lens::with(settings);
  ASSERT_TRUE(lens.has_value());
  const splat::Mat4 proj = lens->projection({100, 100});
  const float nearPlane = proj.at(2, 3) / proj.at(2, 2);
  const float farPlane = proj.at(2, 3) / (proj.at(2, 2) + 1);
  EXPECT_NEAR(nearPlane, 0.5f, 0.5f * 1e-4f);
  EXPECT_NEAR(farPlane, lens->farPlane, lens->farPlane * 0.002f);
}

}  // namespace
}  // namespace splatkit
