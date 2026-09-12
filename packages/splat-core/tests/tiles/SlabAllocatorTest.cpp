#include "splat/tiles/SlabAllocator.h"

#include <gtest/gtest.h>

namespace splat {
namespace {

TEST(SlabAllocator, HandsOutDisjointRangesUntilFull) {
  SlabAllocator slab(100);
  const auto a = slab.allocate(40);
  const auto b = slab.allocate(40);
  ASSERT_TRUE(a && b);
  EXPECT_NE(*a, *b);
  EXPECT_EQ(slab.used(), 80u);
  EXPECT_FALSE(slab.allocate(30));
  EXPECT_TRUE(slab.allocate(20));
  EXPECT_EQ(slab.used(), 100u);
}

TEST(SlabAllocator, ReleasedNeighboursMergeBackIntoOneRange) {
  SlabAllocator slab(100);
  const auto a = slab.allocate(30);
  const auto b = slab.allocate(30);
  const auto c = slab.allocate(40);
  ASSERT_TRUE(a && b && c);
  slab.release(*a, 30);
  slab.release(*c, 40);
  EXPECT_FALSE(slab.allocate(50));  // two holes of 30 and 40
  slab.release(*b, 30);
  EXPECT_TRUE(slab.allocate(100));  // one hole again
  EXPECT_EQ(slab.used(), 100u);
}

TEST(SlabAllocator, PrefersTheSmallestHoleThatFits) {
  SlabAllocator slab(100);
  const auto a = slab.allocate(20);
  const auto b = slab.allocate(30);
  ASSERT_TRUE(a && b);
  slab.release(*a, 20);              // a hole of 20 at 0, and 50 free at the end
  EXPECT_EQ(slab.allocate(10), 0u);  // goes in the small hole, not the big one
  EXPECT_TRUE(slab.allocate(50));    // the big hole is still whole
}

TEST(SlabAllocator, RefusesNothingAndTooMuch) {
  SlabAllocator slab(10);
  EXPECT_FALSE(slab.allocate(0));
  EXPECT_FALSE(slab.allocate(11));
  EXPECT_TRUE(slab.allocate(10));
}

}  // namespace
}  // namespace splat
