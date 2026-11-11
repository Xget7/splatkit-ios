#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "splat/formats/SplatCloud.h"

namespace splatkit {

// The stride matches both shader ABIs so slots can copy palettes without conversion.
struct LabelStyle {
  float tint[3] = {0, 0, 0};
  float tintAmount = 0;
  float brightness = 1;
  float opacity = 1;
  float reserved[2] = {0, 0};
};
static_assert(sizeof(LabelStyle) == 32);
static_assert(offsetof(LabelStyle, brightness) == 16);
// State indices select a small resolved palette, independent of the number of labels.
inline constexpr std::size_t kStyleStates = 10;
inline constexpr std::size_t kPaletteBytes = kStyleStates * sizeof(LabelStyle);
inline std::size_t labelStyleBytes(std::size_t count) {
  return kPaletteBytes + count * sizeof(uint32_t);
}
class LabelStyles {
 public:
  explicit LabelStyles(std::size_t count = 0);
  const std::vector<uint32_t>& states() const { return states_; }
  const std::array<LabelStyle, kStyleStates>& palette() const { return palette_; }
  uint64_t stateRevision() const { return stateRevision_; }
  uint64_t paletteRevision() const { return paletteRevision_; }
  void setStates(std::vector<uint32_t> states);
  void setPalette(std::array<LabelStyle, kStyleStates> palette);
  bool validFor(std::size_t count) const;
  LabelStyle operator[](std::size_t label) const;

 private:
  std::vector<uint32_t> states_;
  std::array<LabelStyle, kStyleStates> palette_{};
  uint64_t stateRevision_;
  uint64_t paletteRevision_;
};
// Two halfwords per word avoid requiring Vulkan 16-bit storage support.
std::vector<uint32_t> packLabels(const splat::SplatCloud& cloud);
}  // namespace splatkit
