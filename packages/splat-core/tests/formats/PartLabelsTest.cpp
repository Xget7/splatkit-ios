#include "splat/formats/PartLabels.h"

#include <vector>

#include <gtest/gtest.h>

namespace splat {
namespace {

// A header for `count` labels; the bytes after it are the caller's.
std::vector<std::uint8_t> header(std::uint32_t count) {
  return {'S',
          'F',
          'G',
          'L',  // magic
          2,
          0,  // version
          2,
          0,  // bytes per label
          static_cast<std::uint8_t>(count),
          static_cast<std::uint8_t>(count >> 8),
          static_cast<std::uint8_t>(count >> 16),
          static_cast<std::uint8_t>(count >> 24),
          0,
          0,
          0,
          0};  // reserved
}

std::vector<std::uint8_t> file(const std::vector<std::uint16_t>& labels) {
  auto bytes = header(static_cast<std::uint32_t>(labels.size()));
  for (auto label : labels) {
    bytes.push_back(label & 255);
    bytes.push_back(label >> 8);
  }
  return bytes;
}

Result<std::vector<std::uint16_t>> decode(const std::vector<std::uint8_t>& bytes) {
  return decodePartLabels(bytes.data(), bytes.size());
}

TEST(PartLabels, DecodesOneLabelPerSplat) {
  auto labels = decode(file({0, 3, 65535, 256}));
  ASSERT_TRUE(labels.ok()) << labels.error().message;
  EXPECT_EQ(labels.value(), (std::vector<std::uint16_t>{0, 3, 65535, 256}));
  // A cloud of 70,000 splats needs the count's upper bytes.
  labels = decode(file(std::vector<std::uint16_t>(70000, 2)));
  ASSERT_TRUE(labels.ok());
  EXPECT_EQ(labels.value().size(), 70000u);
  labels = decode(file({}));
  ASSERT_TRUE(labels.ok());
  EXPECT_TRUE(labels.value().empty());
}

TEST(PartLabels, EncodesTheFileItDecodes) {
  const std::vector<std::uint16_t> labels(70000, 9);
  EXPECT_EQ(encodePartLabels(labels), file(labels));
  EXPECT_EQ(encodePartLabels({}), file({}));
}

TEST(PartLabels, RefusesAnotherFormat) {
  auto bytes = file({1, 2});
  bytes[0] = 'X';
  EXPECT_EQ(decode(bytes).error().code, ErrorCode::unsupportedFormat);
  bytes = file({1, 2});
  bytes[4] = 1;  // obsolete byte format
  EXPECT_EQ(decode(bytes).error().code, ErrorCode::unsupportedFormat);
  bytes = file({1, 2});
  bytes[6] = 1;  // obsolete byte width
  EXPECT_EQ(decode(bytes).error().code, ErrorCode::unsupportedFormat);
  EXPECT_EQ(decode({'S', 'F', 'G'}).error().code, ErrorCode::unsupportedFormat);
  EXPECT_EQ(decodePartLabels(nullptr, 0).error().code, ErrorCode::unsupportedFormat);
}

TEST(PartLabels, RefusesASizeThatDisagreesWithTheCount) {
  auto bytes = file({1, 2, 3});
  bytes.pop_back();
  EXPECT_EQ(decode(bytes).error().code, ErrorCode::corrupt);
  bytes = file({1, 2, 3});
  bytes.push_back(4);
  EXPECT_EQ(decode(bytes).error().code, ErrorCode::corrupt);
  bytes = file({1, 2, 3});
  bytes[12] = 1;  // reserved
  EXPECT_EQ(decode(bytes).error().code, ErrorCode::corrupt);
}

TEST(PartLabels, RejectsNodeCountMismatchAndRoundTripsWideLabels) {
  const auto bytes = encodePartLabels({0, 256, 65535});
  const auto labels = decodePartLabels(bytes.data(), bytes.size(), 3);
  ASSERT_TRUE(labels);
  EXPECT_EQ(labels.value(), (std::vector<std::uint16_t>{0, 256, 65535}));
  const auto mismatch = decodePartLabels(bytes.data(), bytes.size(), 2);
  ASSERT_FALSE(mismatch);
  EXPECT_NE(mismatch.error().message.find("node count mismatch"), std::string::npos);
  auto old = bytes;
  old[4] = old[6] = 1;
  const auto rejected = decodePartLabels(old.data(), old.size());
  ASSERT_FALSE(rejected);
  EXPECT_NE(rejected.error().message.find("version 1"), std::string::npos);
}

}  // namespace
}  // namespace splat
