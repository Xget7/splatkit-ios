#include <gtest/gtest.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>
#include <vector>

#include "SplatKit/SKSplatEngine.h"
#include "load-spz.h"

namespace {

constexpr int64_t kFrameNanos = 16'000'000;

TEST(SKSplatEngineSourceFrame, LettersInEitherCaseNameTheirFrame) {
  EXPECT_STREQ([SKSplatEngine canonicalSourceFrame:@"rub"].UTF8String, "rub");
  EXPECT_STREQ([SKSplatEngine canonicalSourceFrame:@"RDF"].UTF8String, "rdf");
  EXPECT_STREQ([SKSplatEngine canonicalSourceFrame:@"rFu"].UTF8String, "rfu");
}

TEST(SKSplatEngineSourceFrame, LettersThatNameNoFrameAreRejected) {
  for (NSString* letters in @[ @"rrr", @"ru", @"xyz", @"", @"rubf", @"rlu" ]) {
    EXPECT_EQ([SKSplatEngine canonicalSourceFrame:letters], nil) << letters.UTF8String;
  }
  const unichar withNul[] = {'r', 'd', 'f', 0};
  EXPECT_EQ([SKSplatEngine canonicalSourceFrame:[NSString stringWithCharacters:withNul length:4]],
            nil);
}

// Draws through the bridge, so the source frame is checked where a host sets it.
class SKSplatEngineTest : public testing::Test {
 protected:
  void SetUp() override {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7])
      GTEST_SKIP() << "Apple GPU family 7 unavailable; the engine requires A14/M1+";
    engine_ = [SKSplatEngine create];
    ASSERT_NE(engine_, nil);
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.drawableSize = CGSizeMake(64, 64);
    [engine_ setLayer:layer];
    [engine_ setDrawableSize:CGSizeMake(64, 64)];
    path_ = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID UUID].UUIDString];
    path_ = [path_ stringByAppendingPathExtension:@"spz"];
    ASSERT_TRUE([raisedPairBytes() writeToFile:path_ atomically:NO]);
  }

  void TearDown() override {
    [engine_ setLayer:nil];
    if (path_ != nil) [[NSFileManager defaultManager] removeItemAtPath:path_ error:nil];
  }

  // Two splats 2 m apart at file Y = 1, in an SPZ that declares no frame.
  static NSData* raisedPairBytes() {
    spz::GaussianCloud cloud;
    cloud.numPoints = 2;
    cloud.positions = {-1, 1, 2, 1, 1, 2};
    cloud.scales = {0, 0, 0, 0, 0, 0};
    cloud.rotations = {0, 0, 0, 1, 0, 0, 0, 1};
    cloud.alphas = {0, 0};
    cloud.colors = {0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(spz::saveSpz(cloud, spz::PackOptions{}, &bytes));
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
  }

  // The engine's Y of the world's centre, which the default orbit anchors on.
  float loadedCentreY() {
    [engine_ loadWorldFile:path_];
    [engine_ render:now_ += kFrameNanos];
    const SKCameraState state = engine_.cameraState;
    EXPECT_TRUE(state.hasAnchor);
    return state.anchor.y;
  }

  SKSplatEngine* engine_ = nil;
  NSString* path_ = nil;
  int64_t now_ = 0;
};

TEST_F(SKSplatEngineTest, AnUndeclaredSpzLoadsAsRubByDefault) {
  EXPECT_NEAR(loadedCentreY(), 1.0f, 0.01f);
}

TEST_F(SKSplatEngineTest, ASourceFrameTurnsTheNextWorld) {
  ASSERT_TRUE([engine_ setSourceFrame:@"rdf"]);
  EXPECT_NEAR(loadedCentreY(), -1.0f, 0.01f);
}

TEST_F(SKSplatEngineTest, RejectedLettersKeepThePreviousFrame) {
  ASSERT_TRUE([engine_ setSourceFrame:@"RDF"]);
  for (NSString* letters in @[ @"rrr", @"ru", @"xyz" ]) {
    EXPECT_FALSE([engine_ setSourceFrame:letters]) << letters.UTF8String;
  }
  EXPECT_NEAR(loadedCentreY(), -1.0f, 0.01f);
}

TEST_F(SKSplatEngineTest, NilClearsTheSourceFrame) {
  ASSERT_TRUE([engine_ setSourceFrame:@"rdf"]);
  ASSERT_TRUE([engine_ setSourceFrame:nil]);
  EXPECT_NEAR(loadedCentreY(), 1.0f, 0.01f);
}

}  // namespace
