#include "splat/formats/PartLabels.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace splat {
namespace {

constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kWidthOffset = 6;
constexpr std::size_t kCountOffset = 8;
constexpr std::size_t kReservedOffset = 12;

std::uint16_t readU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void putU16(std::uint8_t* p, std::uint16_t value) {
  p[0] = static_cast<std::uint8_t>(value);
  p[1] = static_cast<std::uint8_t>(value >> 8);
}

void putU32(std::uint8_t* p, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t readU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

Result<std::vector<std::uint16_t>> decodePartLabels(const std::uint8_t* data, std::size_t size) {
  using part_labels::kBytesPerLabel;
  using part_labels::kHeaderBytes;
  using part_labels::kMagic;
  using part_labels::kVersion;
  if (data == nullptr || size < kHeaderBytes || std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    return Error{ErrorCode::unsupportedFormat, "not a part labels file"};
  }
  const std::uint16_t version = readU16(data + kVersionOffset);
  const std::uint16_t width = readU16(data + kWidthOffset);
  if (version != kVersion || width != kBytesPerLabel) {
    return Error{ErrorCode::unsupportedFormat, "part labels version " + std::to_string(version) +
                                                   " with " + std::to_string(width) +
                                                   " bytes per label"};
  }
  const std::uint32_t count = readU32(data + kCountOffset);
  if (readU32(data + kReservedOffset) != 0 || (size - kHeaderBytes) / kBytesPerLabel != count ||
      (size - kHeaderBytes) % kBytesPerLabel != 0) {
    return Error{ErrorCode::corrupt, "part labels file of " + std::to_string(size) + " bytes for " +
                                         std::to_string(count) + " labels"};
  }
  std::vector<std::uint16_t> labels(count);
  for (std::size_t i = 0; i < count; ++i) labels[i] = readU16(data + kHeaderBytes + i * 2);
  return labels;
}

std::vector<std::uint8_t> encodePartLabels(const std::vector<std::uint16_t>& labels) {
  using part_labels::kBytesPerLabel;
  using part_labels::kHeaderBytes;
  using part_labels::kMagic;
  using part_labels::kVersion;
  if (labels.size() > std::numeric_limits<uint32_t>::max())
    throw std::length_error("part label count exceeds u32");
  std::vector<std::uint8_t> bytes(kHeaderBytes, 0);
  std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
  putU16(bytes.data() + kVersionOffset, kVersion);
  putU16(bytes.data() + kWidthOffset, kBytesPerLabel);
  putU32(bytes.data() + kCountOffset, static_cast<std::uint32_t>(labels.size()));
  for (const auto label : labels) {
    bytes.push_back(static_cast<std::uint8_t>(label));
    bytes.push_back(static_cast<std::uint8_t>(label >> 8));
  }
  return bytes;
}

Result<std::vector<std::uint16_t>> decodePartLabels(const std::uint8_t* data, std::size_t size,
                                                    std::size_t expectedCount) {
  auto decoded = decodePartLabels(data, size);
  if (!decoded) return decoded.error();
  if (decoded.value().size() != expectedCount)
    return Error{ErrorCode::corrupt, "part label node count mismatch: expected " +
                                         std::to_string(expectedCount) + ", got " +
                                         std::to_string(decoded.value().size())};
  return decoded;
}

}  // namespace splat
