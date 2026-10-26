#include "splat/formats/FrameTransform.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "load-spz.h"
#include "splat/formats/SpzFrame.h"
#include "splat/math/Covariance.h"

namespace splat {
namespace {

constexpr char kLetters[] = "rludbf";

std::vector<CoordinateFrame> allFrames() {
  std::vector<CoordinateFrame> frames;
  for (const char x : std::string(kLetters)) {
    for (const char y : std::string(kLetters)) {
      for (const char z : std::string(kLetters)) {
        if (const auto frame = CoordinateFrame::parse(std::string{x, y, z}))
          frames.push_back(*frame);
      }
    }
  }
  return frames;
}

// The SH basis above band 0 as shColor in SplatProjection.metalh evaluates it, written out here
// so the test checks the conversion against the renderer's definition.
std::array<float, 15> shBasis(const std::array<float, 3>& d) {
  const float x = d[0];
  const float y = d[1];
  const float z = d[2];
  const float xx = x * x;
  const float yy = y * y;
  const float zz = z * z;
  return {-0.4886025119f * y,
          0.4886025119f * z,
          -0.4886025119f * x,
          1.0925484306f * x * y,
          -1.0925484306f * y * z,
          0.3153915653f * (2 * zz - xx - yy),
          -1.0925484306f * x * z,
          0.5462742153f * (xx - yy),
          -0.5900435899f * y * (3 * xx - yy),
          2.8906114426f * x * y * z,
          -0.4570457995f * y * (4 * zz - xx - yy),
          0.3731763326f * z * (2 * zz - 3 * xx - 3 * yy),
          -0.4570457995f * x * (4 * zz - xx - yy),
          1.4453057213f * z * (xx - yy),
          -0.5900435899f * x * (xx - 3 * yy)};
}

// One channel of SH, coefficients RGB fastest as SplatCloud stores them.
float shValue(const std::vector<float>& sh, int channel, const std::array<float, 3>& d) {
  const auto basis = shBasis(d);
  float sum = 0;
  for (std::size_t k = 0; k < basis.size(); ++k) sum += sh[k * 3 + channel] * basis[k];
  return sum;
}

spz::GaussianCloud orientedCloud() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.shDegree = 3;
  cloud.positions = {1.125f, -2.375f, 3.625f};
  cloud.scales = {-1.0f, -2.0f, -3.0f};
  cloud.rotations = {0.2f, 0.3f, 0.4f, std::sqrt(0.71f)};
  cloud.alphas = {0.5f};
  cloud.colors = {0.1f, 0.2f, 0.3f};
  for (int i = 0; i < 45; ++i) cloud.sh.push_back(0.1f + static_cast<float>(i % 9) * 0.025f);
  return cloud;
}

std::array<float, 6> covarianceOf(const spz::GaussianCloud& cloud) {
  const float scale[3] = {std::exp(cloud.scales[0]), std::exp(cloud.scales[1]),
                          std::exp(cloud.scales[2])};
  return covarianceFromQuaternionScale(cloud.rotations.data(), scale);
}

TEST(CoordinateFrame, NamesExactlyTheFortyEightFramesOfThreeAxes) {
  const auto frames = allFrames();
  EXPECT_EQ(frames.size(), 48u);
  for (const auto& frame : frames) EXPECT_EQ(CoordinateFrame::parse(frame.name()), frame);
  EXPECT_EQ(CoordinateFrame::parse("RUB"), CoordinateFrame::kRub);
  EXPECT_EQ(CoordinateFrame::kRdf.name(), "rdf");
}

TEST(CoordinateFrame, RejectsLettersThatDoNotNameThreeAxes) {
  for (const auto* letters : {"", "ru", "rubf", "rlu", "rrr", "rux", "ubb"}) {
    EXPECT_FALSE(CoordinateFrame::parse(letters).has_value()) << letters;
  }
}

TEST(FrameTransform, MovesPointsIntoTheInternalFrame) {
  const auto internal = [](const char* letters) {
    float p[3] = {1, 2, 3};
    FrameTransform(*CoordinateFrame::parse(letters), kInternalFrame).applyToPoint(p);
    return std::array<float, 3>{p[0], p[1], p[2]};
  };
  EXPECT_EQ(internal("rub"), (std::array<float, 3>{1, 2, 3}));
  EXPECT_EQ(internal("rdf"), (std::array<float, 3>{1, -2, -3}));
  EXPECT_EQ(internal("rfu"), (std::array<float, 3>{1, 3, -2}));  // Blender
  EXPECT_EQ(internal("fru"), (std::array<float, 3>{2, 3, -1}));  // Unreal
  EXPECT_TRUE(FrameTransform(CoordinateFrame::kRdf, CoordinateFrame::kRdf).isIdentity());
}

TEST(FrameTransform, MovesBoundsWithTheirCorners) {
  Bounds bounds;
  bounds.min = {0, 1, 2};
  bounds.max = {3, 4, 5};
  const Bounds out =
      FrameTransform(CoordinateFrame::kRdf, CoordinateFrame::kRub).applyToBounds(bounds);
  EXPECT_EQ(out.min, (std::array<float, 3>{0, -4, -5}));
  EXPECT_EQ(out.max, (std::array<float, 3>{3, -1, -2}));
}

// spz's own conversion is an independent reference for its eight frames whose Y is up or down.
// Its other eight are left out: there its axis flips read the letters in the wrong order, so
// for instance lfd to lfu negates Y (pinned spz, affd0ec). spz turns quaternions and scales
// differently, so shapes are compared as covariances.
TEST(FrameTransform, AgreesWithSpzOnItsFramesWithAVerticalY) {
  for (int from = 1; from <= 8; ++from) {
    for (int to = 1; to <= 8; ++to) {
      const auto spzFrom = static_cast<spz::CoordinateSystem>(from);
      const auto spzTo = static_cast<spz::CoordinateSystem>(to);
      SCOPED_TRACE(frameOfSpz(spzFrom)->name() + " to " + frameOfSpz(spzTo)->name());
      auto expected = orientedCloud();
      expected.convertCoordinates(spzFrom, spzTo);
      auto actual = orientedCloud();
      ASSERT_TRUE(convertFrame(actual, *frameOfSpz(spzFrom), *frameOfSpz(spzTo)));
      for (std::size_t i = 0; i < 3; ++i)
        EXPECT_NEAR(actual.positions[i], expected.positions[i], 1e-6f);
      const auto expectedCovariance = covarianceOf(expected);
      const auto actualCovariance = covarianceOf(actual);
      for (std::size_t i = 0; i < 6; ++i)
        EXPECT_NEAR(actualCovariance[i], expectedCovariance[i], 1e-5f);
      for (std::size_t i = 0; i < actual.sh.size(); ++i)
        EXPECT_NEAR(actual.sh[i], expected.sh[i], 1e-5f) << "coefficient " << i;
    }
  }
}

TEST(FrameTransform, KeepsEveryViewsColorAcrossAllFrames) {
  std::mt19937 random(7);
  std::uniform_real_distribution<float> uniform(-1, 1);
  const auto frames = allFrames();
  for (const auto& from : frames) {
    for (const auto& to : frames) {
      SCOPED_TRACE(from.name() + " to " + to.name());
      const FrameTransform transform(from, to);
      std::vector<float> sh(45);
      for (float& c : sh) c = uniform(random);
      std::vector<float> moved = sh;
      transform.applyToSh(moved.data(), 3);
      for (int sample = 0; sample < 4; ++sample) {
        std::array<float, 3> d{uniform(random), uniform(random), uniform(random)};
        const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& c : d) c /= length;
        std::array<float, 3> movedDirection = d;
        transform.applyToPoint(movedDirection.data());
        for (int channel = 0; channel < 3; ++channel) {
          EXPECT_NEAR(shValue(moved, channel, movedDirection), shValue(sh, channel, d), 1e-4f);
        }
      }
    }
  }
}

TEST(FrameTransform, MovesAShapeTheSameWayAsItsCovariance) {
  const auto frames = allFrames();
  const float quaternion[4] = {0.2f, 0.3f, 0.4f, std::sqrt(0.71f)};
  const float scale[3] = {0.5f, 0.25f, 0.125f};
  for (const auto& from : frames) {
    for (const auto& to : frames) {
      SCOPED_TRACE(from.name() + " to " + to.name());
      const FrameTransform transform(from, to);
      float movedQuaternion[4] = {quaternion[0], quaternion[1], quaternion[2], quaternion[3]};
      float movedScale[3] = {scale[0], scale[1], scale[2]};
      transform.applyToRotationAndScale(movedQuaternion, movedScale);
      auto expected = covarianceFromQuaternionScale(quaternion, scale);
      transform.applyToCovariance(expected.data());
      const auto actual = covarianceFromQuaternionScale(movedQuaternion, movedScale);
      for (std::size_t i = 0; i < 6; ++i) EXPECT_NEAR(actual[i], expected[i], 1e-6f);
    }
  }
}

}  // namespace
}  // namespace splat
