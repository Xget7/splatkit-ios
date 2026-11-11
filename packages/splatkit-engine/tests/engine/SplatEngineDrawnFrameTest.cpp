#include "splatkit/engine/SplatEngine.h"

#include <atomic>
#include <cmath>
#include <thread>

#include <gtest/gtest.h>

#include "load-spz.h"
#include "splat/formats/GlbEncoder.h"
#include "splat/formats/PartLabels.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

class QueryRenderer : public StubRenderer {
 public:
  Extent extent{100, 100};
  bool accept = true;
  bool completed = true;
  bool rebuild = false;
  uint32_t revision = 0;
  RevealUniform reveal;
  Extent drawExtent() const override { return extent; }
  uint32_t generation() const override { return revision; }
  bool surfaceRebuildPending() const override { return rebuild; }
  bool hasCompletedWorldFrame() override { return completed; }
  bool draw(const Frame& frame) override {
    reveal = frame.reveal;
    return accept && StubRenderer::draw(frame);
  }
  uint64_t completedFrames() override { return completed ? frames : 0; }
};

void loadLabelled(SplatEngine& engine) {
  spz::GaussianCloud cloud;
  cloud.numPoints = 2;
  cloud.positions = {0, -1, 0, 0, 1, 0};
  cloud.scales = {0, 0, 0, 0, 0, 0};
  cloud.rotations = {0, 0, 0, 1, 0, 0, 0, 1};
  cloud.alphas = {5, 5};
  cloud.colors = {0, 0, 0, 0, 0, 0};
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(spz::saveSpz(cloud, spz::PackOptions{}, &bytes));
  const auto labels = splat::encodePartLabels({1, 1});
  engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size());
}

TEST(SplatEngineDrawnFrame, IdleRedrawFailureAndSurfaceWork) {
  auto renderer = std::make_unique<QueryRenderer>();
  auto* gpu = renderer.get();
  SplatEngine engine(std::move(renderer));
  EXPECT_TRUE(engine.needsFrame());
  EXPECT_TRUE(engine.render(1));
  EXPECT_FALSE(engine.needsFrame());
  EXPECT_FALSE(engine.render(2));
  engine.requestRedraw();
  gpu->accept = false;
  EXPECT_FALSE(engine.render(3));
  EXPECT_TRUE(engine.needsFrame());
  gpu->accept = true;
  EXPECT_TRUE(engine.render(4));
  gpu->revision++;
  EXPECT_TRUE(engine.needsFrame());
  EXPECT_TRUE(engine.render(5));
  gpu->rebuild = true;
  EXPECT_TRUE(engine.needsFrame());
}

TEST(SplatEngineDrawnFrame, PendingLoadCompletionRevealHighlightAndCamera) {
  auto renderer = std::make_unique<QueryRenderer>();
  auto* gpu = renderer.get();
  SplatEngine engine(std::move(renderer));
  engine.render(1);
  RevealSettings reveal;
  reveal.seconds = 0.2f;
  ASSERT_TRUE(engine.setReveal(reveal));
  loadLabelled(engine);
  EXPECT_TRUE(engine.needsFrame());
  gpu->completed = false;
  EXPECT_TRUE(engine.render(16'000'000));
  EXPECT_TRUE(engine.needsFrame());
  engine.render(1'000'000'000);
  EXPECT_TRUE(engine.needsFrame());
  gpu->completed = true;
  for (int i = 1; i <= 20; ++i) engine.render(1'000'000'000 + i * 1'000'000LL + i * 16'000'000LL);
  EXPECT_FALSE(engine.needsFrame());
  const uint16_t label = 1;
  engine.highlightParts(&label, 1);
  EXPECT_TRUE(engine.needsFrame());
  engine.render(1'032'000'000);
  EXPECT_TRUE(engine.needsFrame());
  engine.render(2'000'000'000);
  EXPECT_FALSE(engine.needsFrame());
  engine.setVelocity(1, 0);
  EXPECT_TRUE(engine.needsFrame());
  engine.render(2'016'000'000);
  engine.setVelocity(0, 0);
  engine.render(2'032'000'000);
  EXPECT_FALSE(engine.needsFrame());
  engine.setCameraAnchor({0, 0, 0});
  ASSERT_TRUE(engine.startOrbitAnimation(90, 90, false));
  EXPECT_TRUE(engine.needsFrame());
  engine.startBenchmark(1);
  EXPECT_TRUE(engine.needsFrame());
}

TEST(SplatEngineDrawnFrame, OrbitStopsAtAzimuthLimit) {
  for (const bool continuous : {false, true}) {
    SplatEngine engine(std::make_unique<QueryRenderer>());
    loadLabelled(engine);
    engine.render(1);
    WalkCamera::OrbitLimits limits;
    limits.minAzimuth = -0.1f;
    limits.maxAzimuth = 0.1f;
    ASSERT_TRUE(engine.setOrbitLimits(limits));
    if (continuous) {
      CameraRequest request;
      request.mode = CameraMode::Orbit;
      request.orbitRadiansPerSecond = 1;
      ASSERT_TRUE(engine.applyCameraRequest(request).accepted);
    } else {
      ASSERT_TRUE(engine.startOrbitAnimation(90, 10, false));
    }
    for (int i = 1; i <= 20; ++i) engine.render(i * 100'000'000LL);
    EXPECT_FALSE(engine.needsFrame());
    EXPECT_FALSE(engine.render(3'000'000'000));
  }
}

TEST(SplatEngineDrawnFrame, NewLoadReplacesUndrawnWorld) {
  auto renderer = std::make_unique<QueryRenderer>();
  auto* gpu = renderer.get();
  SplatEngine engine(std::move(renderer));
  std::vector<SplatEngine::Event> events;
  engine.setEventSink([&](auto event, const std::string&, uint32_t) { events.push_back(event); });
  RevealSettings settings;
  settings.seconds = 1;
  settings.tintAmount = 0.2f;
  ASSERT_TRUE(engine.setReveal(settings));
  loadLabelled(engine);
  engine.beginLoad();
  settings.tintAmount = 0.8f;
  ASSERT_TRUE(engine.setReveal(settings));
  loadLabelled(engine);
  EXPECT_TRUE(events.empty());
  ASSERT_TRUE(engine.render(1));
  EXPECT_FLOAT_EQ(gpu->reveal.tintAmount, 0.8f);
  const float first = gpu->reveal.level;
  engine.render(16'000'001);
  EXPECT_GT(gpu->reveal.level, first);
  engine.render(32'000'001);
  EXPECT_EQ(events, (std::vector<SplatEngine::Event>{SplatEngine::Event::worldReady,
                                                     SplatEngine::Event::worldFrameReady}));
}

TEST(SplatEngineDrawnFrame, ColliderWaitingForUploadWakesAnIdleScene) {
  SplatEngine engine(std::make_unique<QueryRenderer>());
  engine.render(1);
  ASSERT_FALSE(engine.needsFrame());
  splat::TriangleMesh mesh;
  mesh.positions = {-10, 0, -10, 10, 0, -10, 10, 0, 10, -10, 0, 10};
  mesh.indices = {0, 2, 1, 0, 3, 2};
  const auto bytes = splat::encodeGlb(mesh);
  engine.loadCollider(bytes.data(), bytes.size());
  EXPECT_TRUE(engine.needsFrame());
  engine.render(16'000'000);
}

TEST(SplatEngineDrawnFrame, QueriesRetainSuccessfulFrameAcrossThreads) {
  auto renderer = std::make_unique<QueryRenderer>();
  auto* gpu = renderer.get();
  SplatEngine engine(std::move(renderer));
  engine.setPartPickingEnabled(true);
  engine.setDrawnFrameQueriesEnabled(true);
  SplatEngine::ViewDirection direction{7, 8};
  EXPECT_FALSE(engine.drawnDirection(direction));
  EXPECT_EQ(direction.azimuth, 7);
  const float points[] = {0, 0, 0, 0, 0, 10};
  float xy[4];
  EXPECT_EQ(engine.project(points, 2, xy), 0u);
  EXPECT_TRUE(std::isnan(xy[0]));
  EXPECT_EQ(engine.pick(0.5f, 0.5f), 0);
  loadLabelled(engine);
  engine.render(1);
  engine.setCameraLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
  ASSERT_TRUE(engine.render(16'000'000));
  EXPECT_EQ(engine.pick(0.5f, 0.5f), 1);
  EXPECT_EQ(engine.project(points, 2, xy), 1u);
  EXPECT_FLOAT_EQ(xy[0], 0.5f);
  EXPECT_TRUE(std::isnan(xy[2]));
  const uint16_t unlabelled = 0;
  engine.highlightParts(&unlabelled, 1);
  engine.render(32'000'000);
  engine.render(400'000'000);
  // Dimming changes colour, not opacity, and must preserve the existing ray pick rule.
  EXPECT_EQ(engine.pick(0.5f, 0.5f), engine.pickPart({{0, 0, 5}, {0, 0, -1}}));
  std::atomic<bool> done{false};
  std::atomic<unsigned> reads{0};
  std::thread reader([&] {
    while (!done.load()) {
      SplatEngine::ViewDirection d;
      EXPECT_TRUE(engine.drawnDirection(d));
      float uv[4];
      EXPECT_EQ(engine.project(points, 2, uv), 1u);
      EXPECT_EQ(engine.pick(0.5f, 0.5f), 1);
      ++reads;
    }
  });
  while (reads.load() == 0) std::this_thread::yield();
  for (int i = 30; i < 70; ++i) {
    engine.requestRedraw();
    engine.render(i * 16'000'000LL);
  }
  done = true;
  reader.join();
  gpu->accept = false;
  engine.setCameraLookAt({5, 0, 0}, {0, 0, 0}, {0, 1, 0});
  EXPECT_FALSE(engine.render(1'000'000'000));
  ASSERT_TRUE(engine.drawnDirection(direction));
  EXPECT_NEAR(direction.azimuth, 0, 1e-6f);
  EXPECT_EQ(engine.pick(0.5f, 0.5f), 1);
}

TEST(SplatEngineDrawnFrame, FramingDirectionResizeAndPinch) {
  auto renderer = std::make_unique<QueryRenderer>();
  auto* gpu = renderer.get();
  SplatEngine engine(std::move(renderer));
  engine.setDrawnFrameQueriesEnabled(true);
  loadLabelled(engine);
  engine.render(1);
  const splat::Bounds bounds{{-1, -1, -1}, {1, 1, 1}};
  ASSERT_TRUE(engine.frameBounds(bounds, 0.2f, SplatEngine::ViewDirection{0.7f, 0.3f}));
  EXPECT_TRUE(engine.needsFrame());
  for (int i = 1; i < 20; ++i) engine.render(i * 16'000'000LL);
  SplatEngine::ViewDirection direction;
  ASSERT_TRUE(engine.drawnDirection(direction));
  EXPECT_NEAR(direction.azimuth, 0.7f, 1e-5f);
  EXPECT_NEAR(direction.elevation, 0.3f, 1e-5f);
  ASSERT_TRUE(engine.frameBounds(bounds, 0));
  engine.render(400'000'000);
  const auto before = engine.cameraPose();
  gpu->extent = {50, 100};
  ++gpu->revision;
  engine.render(416'000'000);
  EXPECT_GT(engine.cameraPose().z, before.z);
  ASSERT_TRUE(engine.dolly(1));
  engine.render(432'000'000);
  const auto pinched = engine.cameraPose();
  gpu->extent = {100, 100};
  ++gpu->revision;
  engine.render(448'000'000);
  EXPECT_FLOAT_EQ(engine.cameraPose().z, pinched.z);
}

}  // namespace
}  // namespace splatkit
