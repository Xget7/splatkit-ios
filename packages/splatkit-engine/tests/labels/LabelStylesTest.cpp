#include "splatkit/labels/LabelStyles.h"

#include <stdexcept>

#include <gtest/gtest.h>

TEST(LabelStyles, RejectsWrongCountsAndPaletteIndices) {
  constexpr std::size_t kWorldLabels = 45;
  splatkit::LabelStyles styles(kWorldLabels);
  EXPECT_FALSE(styles.validFor(kWorldLabels - 1));
  auto states = styles.states();
  states.back() = splatkit::kStyleStates;
  EXPECT_THROW(styles.setStates(states), std::invalid_argument);
}

namespace splatkit {
TEST(LabelStyles, PacksWideLabelsWithoutChangingExistingRecords) {
  EXPECT_EQ(sizeof(LabelStyle), 32u);
  EXPECT_EQ(offsetof(LabelStyle, opacity), 20u);
  splat::SplatCloud cloud;
  cloud.positions.resize(9);
  cloud.alphas = {1, 1, 1};
  EXPECT_TRUE(packLabels(cloud).empty());
  cloud.labels = {256, 65535, 42};
  EXPECT_EQ(packLabels(cloud), (std::vector<uint32_t>{0xffff0100u, 42u}));
  cloud.labels.pop_back();
  EXPECT_TRUE(packLabels(cloud).empty());
}
}  // namespace splatkit
