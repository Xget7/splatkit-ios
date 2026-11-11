#pragma once
#include "splatkit/labels/LabelStyles.h"
namespace splatkit {
// Fence ownership belongs to the backend. Never read mapped GPU memory, which may be uncached.
class LabelStyleUpload {
 public:
  bool copy(void* destination, std::size_t count, const LabelStyles* styles) {
    if (styles && !styles->validFor(count))
      throw std::invalid_argument("label state table must match world");
    const uint64_t states = styles ? styles->stateRevision() : 0;
    const uint64_t palette = styles ? styles->paletteRevision() : 0;
    const bool writeStates = !valid_ || states_ != states;
    const bool writePalette = !valid_ || palette_ != palette;
    auto* bytes = static_cast<unsigned char*>(destination);
    if (writePalette) {
      const std::array<LabelStyle, kStyleStates> defaults{};
      std::memcpy(bytes, styles ? styles->palette().data() : defaults.data(), kPaletteBytes);
    }
    if (writeStates) {
      auto* table = reinterpret_cast<uint32_t*>(bytes + kPaletteBytes);
      for (std::size_t i = 0; i < count; ++i) table[i] = styles ? styles->states()[i] : 0;
    }
    offset_ = writePalette ? 0 : kPaletteBytes;
    bytes_ = (writePalette ? kPaletteBytes : 0) + (writeStates ? count * sizeof(uint32_t) : 0);
    states_ = states;
    palette_ = palette;
    valid_ = true;
    return bytes_ != 0;
  }
  std::size_t uploadOffset() const { return offset_; }
  std::size_t uploadedBytes() const { return bytes_; }

 private:
  uint64_t states_ = 0, palette_ = 0;
  std::size_t offset_ = 0, bytes_ = 0;
  bool valid_ = false;
};
}  // namespace splatkit
