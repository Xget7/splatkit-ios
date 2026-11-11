#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "load-spz.h"
#include "splat/formats/PartLabels.h"
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

class LabelRecordingRenderer final : public StubRenderer {
 public:
  bool uploadWorld(const splat::SplatCloud& cloud, int degree) override {
    labelled = !cloud.labels.empty();
    return StubRenderer::uploadWorld(cloud, degree);
  }
  bool draw(const Frame& frame) override {
    styled = frame.labelStyles != nullptr;
    if (styled) {
      brightness = (*frame.labelStyles)[65535].brightness;
      selectedStyle = (*frame.labelStyles)[65535];
      otherStyle = (*frame.labelStyles)[1];
    }
    return StubRenderer::draw(frame);
  }
  bool labelled = false, styled = false;
  float brightness = 1;
  LabelStyle selectedStyle, otherStyle;
};

TEST(SplatEngineLabels, LoadHighlightPickAndReplacementCrossTheRendererInterface) {
  auto renderer = std::make_unique<LabelRecordingRenderer>();
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  engine.setPartPickingEnabled(true);
  const auto bytes = pairWorldBytes();
  const auto labels = splat::encodePartLabels({65535, 65535});
  engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size());
  engine.render(kFrameNanos);
  ASSERT_TRUE(observed->labelled);
  EXPECT_FALSE(observed->styled);
  EXPECT_EQ(engine.pickPart({{-1, 0, 5}, {0, 0, -1}}), 65535);
  const uint16_t selected = 65535;
  engine.highlightParts(&selected, 1);
  engine.render(kFrameNanos * 30);
  EXPECT_TRUE(observed->styled);
  EXPECT_FLOAT_EQ(observed->brightness, HighlightLook{}.partBrightness);
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(kFrameNanos * 31);
  EXPECT_FALSE(observed->labelled);
  EXPECT_FALSE(observed->styled);
  EXPECT_EQ(engine.pickPart({{-1, 0, 5}, {0, 0, -1}}), 0);
}

TEST(SplatEngineLabels, HighlightLookFadesRetargetsAndRestoresWithoutDisablingDimmedPicking) {
  auto renderer = std::make_unique<LabelRecordingRenderer>();
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  engine.setPartPickingEnabled(true);
  const auto bytes = pairWorldBytes();
  const auto labels = splat::encodePartLabels({65535, 1});
  engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size());
  int64_t now = kFrameNanos;
  engine.render(now);
  uint16_t selected = 65535;
  engine.highlightParts(&selected, 1);
  engine.render(now += 125'000'000);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tintAmount, 0.06f);
  EXPECT_FLOAT_EQ(observed->otherStyle.brightness, 0.775f);
  engine.render(now += 125'000'000);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tintAmount, 0.12f);
  EXPECT_FLOAT_EQ(observed->selectedStyle.brightness, 1.15f);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tint[0], 43 / 255.0f);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tint[1], 184 / 255.0f);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tint[2], 204 / 255.0f);
  EXPECT_FLOAT_EQ(observed->otherStyle.brightness, 0.55f);
  EXPECT_FLOAT_EQ(observed->otherStyle.opacity, 1.0f);
  EXPECT_EQ(engine.pickPart({{1, 0, 5}, {0, 0, -1}}), 1);
  selected = 1;
  engine.highlightParts(&selected, 1);
  engine.render(now += 250'000'000);
  EXPECT_FLOAT_EQ(observed->selectedStyle.brightness, 0.55f);
  EXPECT_FLOAT_EQ(observed->otherStyle.brightness, 1.15f);
  engine.highlightParts(nullptr, 0);
  engine.render(now + 250'000'000);
  for (const auto& style : {observed->selectedStyle, observed->otherStyle}) {
    EXPECT_FLOAT_EQ(style.tintAmount, 0);
    EXPECT_FLOAT_EQ(style.brightness, 1);
    EXPECT_FLOAT_EQ(style.opacity, 1);
  }
}

TEST(SplatEngineLabels, HostHighlightLookRestylesPartsAndOutlivesReplacement) {
  auto renderer = std::make_unique<LabelRecordingRenderer>();
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  HighlightLook look;
  look.tint[0] = 0x2A / 255.0f;
  look.tint[1] = 0x5B / 255.0f;
  look.tint[2] = 0xD7 / 255.0f;
  look.tintAmount = 0.5f;
  look.partBrightness = 1.2f;
  look.otherBrightness = 1;
  auto invalid = look;
  invalid.tintAmount = 2;
  EXPECT_FALSE(engine.setHighlightLook(invalid));
  EXPECT_FLOAT_EQ(engine.highlightLook().tintAmount, HighlightLook{}.tintAmount);

  const auto bytes = pairWorldBytes();
  const auto labels = splat::encodePartLabels({65535, 1});
  engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size());
  int64_t now = kFrameNanos;
  engine.render(now);
  const uint16_t selected = 65535;
  engine.highlightParts(&selected, 1);
  engine.render(now += 250'000'000);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tintAmount, HighlightLook{}.tintAmount);
  ASSERT_TRUE(engine.setHighlightLook(look));
  EXPECT_TRUE(engine.needsFrame());
  engine.render(now += 250'000'000);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tintAmount, 0.5f);
  EXPECT_FLOAT_EQ(observed->selectedStyle.brightness, 1.2f);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tint[1], 0x5B / 255.0f);
  EXPECT_FLOAT_EQ(observed->otherStyle.brightness, 1.0f);

  engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size());
  engine.render(now += kFrameNanos);
  engine.highlightParts(&selected, 1);
  engine.render(now + 250'000'000);
  EXPECT_FLOAT_EQ(observed->selectedStyle.tintAmount, 0.5f);
  EXPECT_FLOAT_EQ(observed->otherStyle.brightness, 1.0f);
}

TEST(SplatEngineLabels, RejectsEarlyHighlightsAndPreparesTreeLeaves) {
  auto renderer = std::make_unique<LabelRecordingRenderer>();
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  const uint16_t selected = 44;
  engine.highlightParts(&selected, 1);
  engine.setPartPickingEnabled(true);
  engine.setSplatBudget(1);
  const auto bytes = pairWorldBytes();
  const auto labels = splat::encodePartLabels({selected, selected});
  engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size());
  engine.render(kFrameNanos);
  EXPECT_GT(observed->lodBudget, 0u);
  EXPECT_FALSE(observed->styled);
  EXPECT_EQ(engine.pickPart({{-1, 0, 5}, {0, 0, -1}}), selected);
  engine.highlightParts(&selected, 1);
  engine.render(kFrameNanos * 30);
  EXPECT_TRUE(observed->styled);
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
