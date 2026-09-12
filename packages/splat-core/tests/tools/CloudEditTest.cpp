#include "CloudEdit.h"

#include <cmath>

#include <gtest/gtest.h>

using splat::tools::packedAlpha;
using splat::tools::pruneAlpha;
using splat::tools::truncateSh;

namespace {

float logit(float opacity) {
  return std::log(opacity / (1.0f - opacity));
}

// Four splats with opacities that pack to 0, 1, 128 and 255, SH degree 1, each field
// carrying the splat's index so a survivor can be told apart from its neighbours.
spz::GaussianCloud four() {
  spz::GaussianCloud c;
  c.numPoints = 4;
  c.shDegree = 1;
  const float opacities[4] = {0.001f, 0.004f, 0.5f, 0.999f};
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 3; ++k) {
      c.positions.push_back(static_cast<float>(i));
      c.scales.push_back(static_cast<float>(i));
      c.colors.push_back(static_cast<float>(i));
    }
    for (int k = 0; k < 4; ++k) c.rotations.push_back(static_cast<float>(i));
    c.alphas.push_back(logit(opacities[i]));
    for (int k = 0; k < 9; ++k) c.sh.push_back(static_cast<float>(i));
  }
  return c;
}

TEST(CloudEdit, PacksAlphaTheWaySpzDoes) {
  EXPECT_EQ(packedAlpha(logit(0.001f)), 0);
  EXPECT_EQ(packedAlpha(logit(0.004f)), 1);
  EXPECT_EQ(packedAlpha(logit(0.5f)), 128);
  EXPECT_EQ(packedAlpha(logit(0.999f)), 255);
}

TEST(CloudEdit, PruningAtOneOver255DropsOnlyWhatDrawsNothing) {
  spz::GaussianCloud c = four();
  EXPECT_EQ(pruneAlpha(c, 1.0f / 255.0f), 1);
  ASSERT_EQ(c.numPoints, 3);
  EXPECT_EQ(c.positions[0], 1.0f);
  EXPECT_EQ(c.rotations[0], 1.0f);
  EXPECT_EQ(c.sh[0], 1.0f);
  EXPECT_EQ(c.sh.size(), 27u);
  EXPECT_EQ(c.positions[6], 3.0f);
}

TEST(CloudEdit, PruningHigherDropsTheFaintLayers) {
  spz::GaussianCloud c = four();
  EXPECT_EQ(pruneAlpha(c, 0.5f), 2);
  ASSERT_EQ(c.numPoints, 2);
  EXPECT_EQ(c.positions[0], 2.0f);
  EXPECT_EQ(c.alphas.size(), 2u);
}

TEST(CloudEdit, PruningAtZeroKeepsEverything) {
  spz::GaussianCloud c = four();
  EXPECT_EQ(pruneAlpha(c, 0.0f), 0);
  EXPECT_EQ(c.numPoints, 4);
}

TEST(CloudEdit, TruncatesHarmonicsPerPoint) {
  spz::GaussianCloud c = four();
  truncateSh(c, 0);
  EXPECT_EQ(c.shDegree, 0);
  EXPECT_TRUE(c.sh.empty());
}

}  // namespace
