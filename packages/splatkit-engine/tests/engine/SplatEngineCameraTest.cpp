#include <gtest/gtest.h>

#include "splatkit/engine/SplatEngine.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

TEST(SplatEngineEvents, ReportsTerminalGpuFailureOnceOnRender) {
  class FailedRenderer : public StubRenderer {
   public:
    bool gpuFailed() override { return true; }
  };
  SplatEngine engine(std::make_unique<FailedRenderer>());
  int failures = 0;
  engine.setEventSink([&](SplatEngine::Event event, const std::string&, uint32_t) {
    EXPECT_EQ(event, SplatEngine::Event::gpuFailed);
    ++failures;
  });
  engine.render(1);
  engine.render(2);
  EXPECT_EQ(failures, 1);
}

// A host that sets a pose and reads it back before any frame ran must see the new pose,
// even without a world.
TEST(SplatEngineCamera, ATeleportIsReadableBeforeTheNextFrame) {
  SplatEngine engine(std::make_unique<StubRenderer>());
  CameraPose pose;
  pose.x = 1;
  pose.y = 2;
  pose.z = 3;
  pose.yaw = 0.5f;
  engine.setCameraPose(pose);
  EXPECT_FLOAT_EQ(engine.cameraPose().x, 1);
  EXPECT_FLOAT_EQ(engine.cameraPose().z, 3);
  EXPECT_FLOAT_EQ(engine.cameraPose().yaw, 0.5f);

  engine.setCameraAnchor({0, 0, 0});
  EXPECT_TRUE(engine.orbit(1.0f, 0.0f));
  EXPECT_NE(engine.cameraPose().x, 1);
  EXPECT_FALSE(engine.dolly(std::numeric_limits<float>::quiet_NaN()));
}

}  // namespace
}  // namespace splatkit
