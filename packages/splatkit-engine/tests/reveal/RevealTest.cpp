#include "splatkit/reveal/Reveal.h"

#include <gtest/gtest.h>

namespace splatkit {
TEST(Reveal, RisesAndRestoresIdle) {
  Reveal sweep;
  splat::Bounds bounds{{0, 0, 0}, {1, 10, 1}};
  RevealSettings settings;
  settings.seconds = 1;
  sweep.start(bounds, settings);
  EXPECT_LT(sweep.level(), 0);
  EXPECT_FLOAT_EQ(sweep.band(), 0.8f);
  float previous = sweep.level();
  for (int i = 0; i < 9; ++i) {
    EXPECT_TRUE(sweep.update(0.1f));
    EXPECT_GT(sweep.level(), previous);
    previous = sweep.level();
  }
  EXPECT_FALSE(sweep.update(-1));
  EXPECT_FALSE(sweep.update(std::numeric_limits<float>::quiet_NaN()));
  sweep.update(1);
  EXPECT_EQ(sweep.level(), std::numeric_limits<float>::infinity());
  EXPECT_EQ(sweep.band(), 0);
  EXPECT_FALSE(sweep.update(1));
  settings.seconds = 0;
  sweep.start(bounds, settings);
  EXPECT_FALSE(sweep.active());
  settings.seconds = 1;
  bounds.max[1] = 0;
  sweep.start(bounds, settings);
  EXPECT_FALSE(sweep.active());
}
}  // namespace splatkit
