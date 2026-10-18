#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "load-spz.h"
#include "splatkit/engine/SplatEngine.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

constexpr int64_t kFrameNanos = 16'000'000;

// Two splats 2 m apart, so the default orbit has a size to frame.
std::vector<uint8_t> pairWorldBytes() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 2;
  cloud.positions = {-1, 0, 2, 1, 0, 2};
  cloud.scales = {0, 0, 0, 0, 0, 0};
  cloud.rotations = {0, 0, 0, 1, 0, 0, 0, 1};
  cloud.alphas = {0, 0};
  cloud.colors = {0, 0, 0, 0, 0, 0};
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, spz::PackOptions{}, &bytes));
  return bytes;
}

void expectProjection(const splat::Mat4& drawn, const Lens& lens, Extent extent) {
  const splat::Mat4 expected = lens.projection(extent);
  for (size_t i = 0; i < expected.m.size(); ++i) EXPECT_FLOAT_EQ(drawn.m[i], expected.m[i]) << i;
}

struct LoadedEngine {
  StubRenderer* renderer;
  SplatEngine engine;
  int64_t now = 0;

  LoadedEngine() : LoadedEngine(std::make_unique<StubRenderer>()) {}
  explicit LoadedEngine(std::unique_ptr<StubRenderer> stub)
      : renderer(stub.get()), engine(std::move(stub)) {}

  void load() {
    const auto bytes = pairWorldBytes();
    engine.loadWorld(bytes.data(), bytes.size());
    frame();
  }
  void frame() { engine.render(now += kFrameNanos); }
};

TEST(SplatEngineLens, ALensSetBeforeTheLoadDrawsAndFramesTheWorld) {
  LoadedEngine loaded;
  LensSettings settings;
  settings.fieldOfViewDegrees = 40;
  settings.depthRange = DepthRange{0.5f, 2000};
  ASSERT_TRUE(loaded.engine.setLens(settings));
  loaded.load();

  const Lens lens = *Lens::with(settings);
  ASSERT_GT(loaded.renderer->frames, 0u);
  expectProjection(loaded.renderer->proj, lens, loaded.renderer->drawExtent());
  splat::Bounds bounds;
  bounds.min = {-1, 0, 2};
  bounds.max = {1, 0, 2};
  EXPECT_FLOAT_EQ(loaded.engine.cameraState().radius,
                  lens.framingRadius(bounds, loaded.renderer->drawExtent()));
}

TEST(SplatEngineLens, ANewLensRedrawsAndReframesTheDefaultOrbit) {
  LoadedEngine loaded;
  loaded.load();
  const float framed = loaded.engine.cameraState().radius;
  loaded.frame();
  const uint32_t frames = loaded.renderer->frames;

  LensSettings narrow;
  narrow.fieldOfViewDegrees = 30;
  ASSERT_TRUE(loaded.engine.setLens(narrow));
  loaded.frame();
  EXPECT_EQ(loaded.renderer->frames, frames + 1);
  expectProjection(loaded.renderer->proj, *Lens::with(narrow), loaded.renderer->drawExtent());
  // A narrower view needs the camera further back to fit the same world.
  EXPECT_GT(loaded.engine.cameraState().radius, framed);
}

TEST(SplatEngineLens, RefusedSettingsKeepTheLensInUse) {
  SplatEngine engine(std::make_unique<StubRenderer>());
  LensSettings wide;
  wide.fieldOfViewDegrees = 100;
  ASSERT_TRUE(engine.setLens(wide));

  LensSettings bad;
  bad.fieldOfViewDegrees = 50;
  bad.depthRange = DepthRange{1, 0.5f};
  EXPECT_FALSE(engine.setLens(bad));
  EXPECT_FLOAT_EQ(engine.lens().fieldOfViewRadians, Lens::with(wide)->fieldOfViewRadians);
  EXPECT_FLOAT_EQ(engine.lens().farPlane, Lens{}.farPlane);
}

}  // namespace
}  // namespace splatkit
