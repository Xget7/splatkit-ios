#include "splatkit/pick/PickIndex.h"

#include <array>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

namespace splatkit {
namespace {

using Covariance = std::array<float, 6>;  // xx, xy, xz, yy, yz, zz

Covariance round(float sigma) {
  const float v = sigma * sigma;
  return {v, 0, 0, v, 0, v};
}

void add(splat::SplatCloud& cloud, splat::Vec3 at, Covariance covariance, float alpha,
         std::uint16_t label) {
  cloud.positions.insert(cloud.positions.end(), {at.x, at.y, at.z});
  cloud.covariances.insert(cloud.covariances.end(), covariance.begin(), covariance.end());
  cloud.alphas.push_back(alpha);
  cloud.labels.push_back(label);
}

Ray lookingAt(float x, float y) {
  return {{0, 0, 0}, splat::normalize({x, y, -1})};
}

std::uint16_t pick(splat::SplatCloud cloud, const Ray& ray) {
  return PickIndex::take(cloud)->pick(ray);
}

TEST(PickIndex, HiddenForegroundDoesNotOccludeAndTakeConsumesGeometry) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -2}, round(0.1f), 0.99f, 1);
  add(cloud, {0, 0, -4}, round(0.1f), 0.99f, 2);
  const auto index = PickIndex::take(cloud);
  EXPECT_TRUE(cloud.positions.empty());
  EXPECT_TRUE(cloud.labels.empty());
  LabelStyles styles(3);
  auto states = styles.states();
  states[1] = 1;
  styles.setStates(states);
  auto palette = styles.palette();
  palette[1].opacity = 0;
  styles.setPalette(palette);
  EXPECT_EQ(index->pick(lookingAt(0, 0), &styles), 2);
}

TEST(PickIndex, SharesConstTreeAndSkipsInteriorGeometry) {
  auto tree = std::make_shared<splat::LodTree>();
  add(tree->nodes, {0, 0, -1}, round(0.1f), 0.99f, 1);
  add(tree->nodes, {0, 0, -3}, round(0.1f), 0.99f, 2);
  tree->layout.resize(2);
  tree->layout[0].childCount = 1;
  const std::weak_ptr<const splat::LodTree> retained = tree;
  auto index = PickIndex::share(tree);
  EXPECT_EQ(tree->nodes.count(), 2u);
  tree.reset();
  EXPECT_FALSE(retained.expired());
  EXPECT_EQ(index->pick(lookingAt(0, 0)), 2);
  index.reset();
  EXPECT_TRUE(retained.expired());
}

TEST(PickIndex, PreservesTheHighestPartLabel) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -5}, round(0.1f), 0.99f, 65535);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 65535);
}

TEST(PickIndex, ARayThroughAPartPicksItAndOnePastThreeSigmaMisses) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -5}, round(0.1f), 0.99f, 7);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 7);
  EXPECT_EQ(pick(cloud, {{0.35f, 0, 0}, {0, 0, -1}}), 0);  // 3.5 sigma to the side
  EXPECT_EQ(pick(cloud, {{0, 0, 0}, {0, 0, 1}}), 0);       // looking away
}

TEST(PickIndex, TheNearerOpaquePartWinsWhateverTheStorageOrder) {
  splat::SplatCloud nearFirst;
  add(nearFirst, {0, 0, -2}, round(0.1f), 0.99f, 1);
  add(nearFirst, {0, 0, -4}, round(0.1f), 0.99f, 2);
  EXPECT_EQ(pick(nearFirst, lookingAt(0, 0)), 1);
  splat::SplatCloud farFirst;
  add(farFirst, {0, 0, -4}, round(0.1f), 0.99f, 2);
  add(farFirst, {0, 0, -2}, round(0.1f), 0.99f, 1);
  EXPECT_EQ(pick(farFirst, lookingAt(0, 0)), 1);
}

TEST(PickIndex, AFaintSplatInFrontDoesNotHideThePartBehind) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -2}, round(0.1f), 0.2f, 1);
  add(cloud, {0, 0, -4}, round(0.1f), 0.99f, 2);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 2);
}

TEST(PickIndex, AnUnlabelledThingInFrontPicksNothing) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -2}, round(0.1f), 0.9f, 0);
  add(cloud, {0, 0, -4}, round(0.1f), 0.99f, 6);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 0);
}

TEST(PickIndex, ManyFaintSplatsOfAPartAddUp) {
  splat::SplatCloud cloud;
  for (int i = 0; i < 12; ++i) add(cloud, {0, 0, -2.0f - 0.1f * i}, round(0.1f), 0.15f, 3);
  add(cloud, {0, 0, -5}, round(0.1f), 0.99f, 4);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 3);  // 86% of the pixel against the rest
}

TEST(PickIndex, LessThanHalfAPixelCoveredPicksNothing) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -5}, round(0.1f), 0.4f, 5);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 0);
  // An opaque splat grazed at two sigma shows at 13%, background mostly.
  splat::SplatCloud grazed;
  add(grazed, {0.2f, 0, -5}, round(0.1f), 0.99f, 5);
  EXPECT_EQ(pick(grazed, {{0, 0, 0}, {0, 0, -1}}), 0);
}

TEST(PickIndex, AnElongatedSplatIsHitAlongItsLengthOnly) {
  // Long in x (sigma 1), thin in y (sigma 0.05), rotated 45 degrees about z.
  const float along = 1.0f;
  const float across = 0.05f * 0.05f;
  const float mean = (along + across) / 2;
  const float half = (along - across) / 2;
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -5}, {mean, half, 0, mean, 0, across}, 0.99f, 9);
  EXPECT_EQ(pick(cloud, {{0.5f, 0.5f, 0}, {0, 0, -1}}), 9);   // 0.7 sigma along the length
  EXPECT_EQ(pick(cloud, {{0.1f, -0.1f, 0}, {0, 0, -1}}), 0);  // 2.8 sigma across it
}

TEST(PickIndex, ACloudWithoutLabelsPicksNothingAndTakingKeepsTheColours) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -5}, round(0.1f), 0.99f, 1);
  cloud.colors = {1, 0, 0};
  auto index = PickIndex::take(cloud);
  EXPECT_EQ(index->pick(lookingAt(0, 0)), 1);
  EXPECT_TRUE(cloud.positions.empty());
  EXPECT_EQ(cloud.colors.size(), 3u);

  splat::SplatCloud unlabelled;
  add(unlabelled, {0, 0, -5}, round(0.1f), 0.99f, 1);
  unlabelled.labels.clear();
  EXPECT_EQ(pick(unlabelled, lookingAt(0, 0)), 0);
}

TEST(PickIndex, ADegenerateSplatIsSkipped) {
  splat::SplatCloud cloud;
  add(cloud, {0, 0, -2}, {0, 0, 0, 0, 0, 0}, 0.99f, 1);
  add(cloud, {0, 0, -3}, {0.01f, 0, 0, 0.01f, 0, 0}, 0.99f, 2);  // flat: no volume
  add(cloud, {0, 0, -4}, round(0.1f), 0.99f, 3);
  EXPECT_EQ(pick(cloud, lookingAt(0, 0)), 3);
}

}  // namespace
}  // namespace splatkit
