#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "load-spz.h"

namespace splat {

// `points` splats spread through a 4 unit cube, with every coefficient up to `shDegree`.
inline spz::GaussianCloud randomCloud(int points, int shDegree) {
  spz::GaussianCloud cloud;
  cloud.numPoints = points;
  cloud.shDegree = shDegree;
  const int coefficients = (shDegree + 1) * (shDegree + 1) - 1;
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  for (int i = 0; i < points; ++i) {
    for (int k = 0; k < 3; ++k) cloud.positions.push_back(u(rng));
    for (int k = 0; k < 3; ++k) cloud.scales.push_back(-3.0f);
    cloud.rotations.insert(cloud.rotations.end(), {0.0f, 0.0f, 0.0f, 1.0f});
    cloud.alphas.push_back(0.0f);
    for (int k = 0; k < 3; ++k) cloud.colors.push_back(0.0f);
    for (int k = 0; k < 3 * coefficients; ++k) cloud.sh.push_back(u(rng) / 4.0f);
  }
  return cloud;
}

// The cloud as an SPZ of `version`: gzip up to 3, NGSP from 4.
inline std::vector<std::uint8_t> encodeSpz(const spz::GaussianCloud& cloud,
                                           std::uint32_t version = spz::LATEST_SPZ_HEADER_VERSION) {
  spz::PackOptions pack;
  pack.version = version;
  std::vector<std::uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, pack, &bytes));
  return bytes;
}

}  // namespace splat
