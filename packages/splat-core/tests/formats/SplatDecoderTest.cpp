#include "splat/formats/SplatDecoder.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "load-spz.h"

namespace splat {
namespace {

std::vector<std::uint8_t> encodeSpz(std::uint32_t version) {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {0.0f, 0.0f, 1.0f};
  cloud.scales = {0.0f, 0.0f, 0.0f};
  cloud.rotations = {0.0f, 0.0f, 0.0f, 1.0f};
  cloud.alphas = {0.0f};
  cloud.colors = {0.0f, 0.0f, 0.0f};
  spz::PackOptions pack;
  pack.version = version;
  std::vector<std::uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, pack, &bytes));
  return bytes;
}

TEST(SplatDecoder, DetectsBothSpzContainers) {
  auto gzip = encodeSpz(2);
  auto ngsp = encodeSpz(4);
  EXPECT_EQ(detectSplatFormat(gzip.data(), gzip.size()), SplatFormat::spz);
  EXPECT_EQ(detectSplatFormat(ngsp.data(), ngsp.size()), SplatFormat::spz);
}

TEST(SplatDecoder, UnknownBytesAreUnsupportedNotCorrupt) {
  const std::uint8_t ply[] = {'p', 'l', 'y', '\n', 'f', 'o', 'r', 'm'};
  EXPECT_EQ(detectSplatFormat(ply, sizeof(ply)), SplatFormat::unknown);
  auto result = decodeSplatFile(ply, sizeof(ply));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, ErrorCode::unsupportedFormat);
  EXPECT_EQ(detectSplatFormat(ply, 2), SplatFormat::unknown);
}

TEST(SplatDecoder, DispatchesSpzAndAppliesTheOptions) {
  auto bytes = encodeSpz(2);
  auto result = decodeSplatFile(bytes.data(), bytes.size());
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().count(), 1u);
  // RDF (the default source frame) to RUB negates z.
  EXPECT_NEAR(result.value().positions[2], -1.0f, 1e-3f);

  SplatDecodeOptions tiny;
  tiny.maxDecodedBytes = 1;
  auto rejected = decodeSplatFile(bytes.data(), bytes.size(), tiny);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, ErrorCode::corrupt);
}

}  // namespace
}  // namespace splat
