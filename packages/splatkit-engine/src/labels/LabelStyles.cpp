#include "splatkit/labels/LabelStyles.h"
#include <atomic>
namespace splatkit {
namespace {
uint64_t revision() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace
LabelStyles::LabelStyles(std::size_t count)
    : states_(count, 0), stateRevision_(revision()), paletteRevision_(revision()) {}
void LabelStyles::setStates(std::vector<uint32_t> states) {
  if (std::any_of(states.begin(), states.end(), [](uint32_t s) { return s >= kStyleStates; }))
    throw std::invalid_argument("label state outside palette");
  if (states_ == states) return;
  states_ = std::move(states);
  stateRevision_ = revision();
}
void LabelStyles::setPalette(std::array<LabelStyle, kStyleStates> palette) {
  const auto equal = [](const LabelStyle& a, const LabelStyle& b) {
    return std::equal(std::begin(a.tint), std::end(a.tint), std::begin(b.tint)) &&
           a.tintAmount == b.tintAmount && a.brightness == b.brightness && a.opacity == b.opacity;
  };
  if (std::equal(palette_.begin(), palette_.end(), palette.begin(), equal)) return;
  palette_ = palette;
  paletteRevision_ = revision();
}
bool LabelStyles::validFor(std::size_t count) const {
  return states_.size() == count;
}
LabelStyle LabelStyles::operator[](std::size_t label) const {
  return palette_[label < states_.size() ? states_[label] : 0];
}

std::vector<uint32_t> packLabels(const splat::SplatCloud& cloud) {
  if (cloud.labels.empty() || cloud.labels.size() != cloud.count()) return {};
  std::vector<uint32_t> words((cloud.count() + 1) / 2, 0);
  for (std::size_t i = 0; i < cloud.count(); ++i)
    words[i / 2] |= uint32_t{cloud.labels[i]} << ((i % 2) * 16);
  return words;
}
}  // namespace splatkit
