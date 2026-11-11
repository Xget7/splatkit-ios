#include <gtest/gtest.h>
#import <objc/runtime.h>
#include <cstdlib>
#include <string>

#include "rendering/metal/MetalSplatRenderer.h"
#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/sorting/SpatialOrder.h"
#include "splatkit/camera/WalkCamera.h"
#include "splatkit/highlight/Highlight.h"

namespace splatkit {
namespace {

// Intercept only backdrop resources at the real device boundary, while tests use the host API.
// Each test process runs one case; restoring the methods also keeps later in-process cases safe.
class BackdropResources {
 public:
  static inline int gLibraries = 0;
  static inline bool gFailLibrary = false;
  static inline IMP gOriginalLibrary = nullptr;
  Method libraryMethod;
  static inline int gPipelines = 0;
  static inline int gTextures = 0;
  static inline bool gFailPipeline = false;
  static inline bool gFailTexture = false;
  static inline bool gFailDepth = false;
  static inline IMP gOriginalPipeline = nullptr;
  static inline IMP gOriginalTexture = nullptr;
  BackdropResources(const BackdropResources&) = delete;
  BackdropResources& operator=(const BackdropResources&) = delete;
  Method pipelineMethod;
  Method textureMethod;

  static id pipeline(id device, SEL selector, MTLRenderPipelineDescriptor* descriptor,
                     NSError** error) {
    if ([descriptor.fragmentFunction.name hasPrefix:@"backdrop"]) {
      ++gPipelines;
      if (gFailPipeline) return nil;
    }
    using Function = id (*)(id, SEL, MTLRenderPipelineDescriptor*, NSError**);
    return reinterpret_cast<Function>(gOriginalPipeline)(device, selector, descriptor, error);
  }
  static id texture(id device, SEL selector, MTLTextureDescriptor* descriptor) {
    if (gFailDepth && descriptor.pixelFormat == MTLPixelFormatDepth16Unorm) return nil;
    if (descriptor.pixelFormat == MTLPixelFormatRGBA16Float && descriptor.width <= 32 &&
        descriptor.height <= 32) {
      ++gTextures;
      if (gFailTexture) return nil;
    }
    using Function = id (*)(id, SEL, MTLTextureDescriptor*);
    return reinterpret_cast<Function>(gOriginalTexture)(device, selector, descriptor);
  }
  static id library(id device, SEL selector, NSString* source, MTLCompileOptions* options,
                    NSError** error) {
    if ([source containsString:@"backdropPush"]) {
      ++gLibraries;
      if (gFailLibrary) return nil;
    }
    using Function = id (*)(id, SEL, NSString*, MTLCompileOptions*, NSError**);
    return reinterpret_cast<Function>(gOriginalLibrary)(device, selector, source, options, error);
  }
  BackdropResources() {
    gLibraries = 0;
    gFailLibrary = false;
    gFailDepth = false;
    gPipelines = gTextures = 0;
    gFailPipeline = gFailTexture = false;
    const Class device = object_getClass(MTLCreateSystemDefaultDevice());
    libraryMethod = class_getInstanceMethod(device, @selector(newLibraryWithSource:options:error:));
    gOriginalLibrary = method_setImplementation(libraryMethod, reinterpret_cast<IMP>(library));
    pipelineMethod =
        class_getInstanceMethod(device, @selector(newRenderPipelineStateWithDescriptor:error:));
    textureMethod = class_getInstanceMethod(device, @selector(newTextureWithDescriptor:));
    if (pipelineMethod == nullptr || textureMethod == nullptr) std::abort();
    gOriginalPipeline = method_setImplementation(pipelineMethod, reinterpret_cast<IMP>(pipeline));
    gOriginalTexture = method_setImplementation(textureMethod, reinterpret_cast<IMP>(texture));
  }
  ~BackdropResources() {
    method_setImplementation(libraryMethod, gOriginalLibrary);
    method_setImplementation(pipelineMethod, gOriginalPipeline);
    method_setImplementation(textureMethod, gOriginalTexture);
  }
};

class MetalSplatRendererTest : public testing::Test {
 protected:
  void SetUp() override {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7])
      GTEST_SKIP() << "Apple GPU family 7 unavailable; renderer validation requires A14/M1+";
  }
};

TEST_F(MetalSplatRendererTest, RevealClipsAndTintsThenRestoresIdenticalImage) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  std::vector<uint8_t> reference;
  std::vector<uint8_t> clipped;
  std::vector<uint8_t> tinted;
  std::vector<uint8_t> restored;
  capture(&reference);
  frame.reveal.level = -1;
  frame.reveal.band = 1;
  capture(&clipped);
  const size_t center = ((32 * 64) + 32) * 4;
  EXPECT_GT(reference[center + 2], 200);
  EXPECT_LT(clipped[center + 2], 40);
  frame.reveal.level = 0;
  frame.reveal.tintAmount = 1;
  frame.reveal.brightnessBoost = 0.5f;
  frame.reveal.tint[0] = 0;
  frame.reveal.tint[1] = 1;
  frame.reveal.tint[2] = 0;
  capture(&tinted);
  EXPECT_GT(tinted[center + 1], 200);
  EXPECT_LT(tinted[center + 2], 20);
  frame.reveal.level = 0.5f;
  std::vector<uint8_t> fading;
  capture(&fading);
  EXPECT_LT(fading[center + 1], tinted[center + 1]);
  EXPECT_GT(fading[center + 2], tinted[center + 2]);
  LabelStyles styles(2);
  auto palette = styles.palette();
  palette[0].opacity = 0;
  styles.setPalette(palette);
  frame.labelStyles = &styles;
  std::vector<uint8_t> hidden;
  capture(&hidden);
  EXPECT_EQ(hidden, clipped);
  frame.labelStyles = nullptr;
  frame.reveal = {};
  capture(&restored);
  EXPECT_EQ(reference, restored);
}

TEST_F(MetalSplatRendererTest, UndimmedHighlightLeavesOtherPartsPixelIdentical) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {0.6f, 0.3f, 0.2f};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  std::vector<uint8_t> reference;
  capture(&reference);
  // Highlight another part, so the drawn splat is in the surroundings.
  HighlightLook look;
  look.tint[0] = 0x2A / 255.0f;
  look.tint[1] = 0x5B / 255.0f;
  look.tint[2] = 0xD7 / 255.0f;
  look.tintAmount = 0.5f;
  look.partBrightness = 1.2f;
  look.otherBrightness = 1;
  Highlight highlight(2, look);
  const uint16_t other = 0;
  highlight.set(&other, 1);
  while (highlight.update(0.05f)) {
  }
  frame.labelStyles = &highlight.styles();
  std::vector<uint8_t> highlighted;
  capture(&highlighted);
  EXPECT_EQ(reference, highlighted);
  // The same splat as the highlighted part does change, so the styles reach the shader.
  const uint16_t drawn = 1;
  highlight.set(&drawn, 1);
  while (highlight.update(0.05f)) {
  }
  std::vector<uint8_t> emphasised;
  capture(&emphasised);
  EXPECT_NE(reference, emphasised);
}

TEST_F(MetalSplatRendererTest, SceneBackdropFillsOpaqueAndTogglesWithoutChangingTheOffImage) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  std::vector<uint8_t> reference;
  std::vector<uint8_t> explicitOff;
  std::vector<uint8_t> filled;
  std::vector<uint8_t> restored;
  capture(&reference);
  ASSERT_TRUE(renderer->setSceneBackdrop({}, nullptr));
  capture(&explicitOff);
  EXPECT_EQ(reference, explicitOff);
  SceneBackdrop settings;
  settings.enabled = true;
  settings.gain = 1;
  settings.saturation = 1;
  settings.vignette = 0;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  capture(&filled);
  ASSERT_EQ(filled.size(), reference.size());
  for (size_t i = 3; i < filled.size(); i += 4) EXPECT_EQ(filled[i], 255);
  const size_t center = ((32 * 64) + 32) * 4;
  EXPECT_NEAR(filled[center + 2], reference[center + 2], 3);
  EXPECT_GT(filled[2], 200);
  EXPECT_LT(filled[0], 3);
  EXPECT_LT(filled[1], 3);
  settings.gain = 0;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  std::vector<uint8_t> dark;
  capture(&dark);
  EXPECT_LT(dark[2], 3);
  frame.reveal.level = -1;
  frame.reveal.band = 1;
  settings.gain = 1;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  capture(&dark);
  EXPECT_NEAR(dark[center + 2], 13, 2);
  frame.reveal = {};
  LabelStyles styles(2);
  auto palette = styles.palette();
  palette[0].opacity = 0;
  styles.setPalette(palette);
  frame.labelStyles = &styles;
  capture(&dark);
  EXPECT_NEAR(dark[center + 2], 13, 2);
  frame.labelStyles = nullptr;
  ASSERT_TRUE(renderer->setSceneBackdrop({}, nullptr));
  capture(&restored);
  EXPECT_EQ(reference, restored);
}

TEST_F(MetalSplatRendererTest, SceneBackdropFillsFromNearbyCoverage) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {-0.8f, 0, -2, 0.8f, 0, -2};
  cloud.colors = {1, 0, 0, 0, 1, 0};
  cloud.alphas = {1, 1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f, 0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1, 1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 2};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  SceneBackdrop settings;
  settings.enabled = true;
  settings.gain = settings.saturation = 1;
  settings.vignette = 0;
  settings.blurSpacing = 0;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  std::vector<uint8_t> filled;
  capture(&filled);
  const size_t left = 32 * 64 * 4;
  const size_t right = left + (63 * 4);
  EXPECT_GT(filled[left + 2], filled[left + 1] + 30);
  EXPECT_GT(filled[right + 1], filled[right + 2] + 30);
}

TEST_F(MetalSplatRendererTest, SceneBackdropAllocatesOnlyOnEnableAndResize) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  const BackdropResources resources;
  std::vector<uint8_t> reference;
  std::vector<uint8_t> actual;
  capture(&reference);
  EXPECT_EQ(BackdropResources::gLibraries, 0);
  EXPECT_EQ(BackdropResources::gPipelines, 0);
  EXPECT_EQ(BackdropResources::gTextures, 0);
  SceneBackdrop settings;
  settings.enabled = true;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  capture(&actual);
  EXPECT_EQ(BackdropResources::gLibraries, 1);
  EXPECT_EQ(BackdropResources::gPipelines, 4);
  EXPECT_EQ(BackdropResources::gTextures, 3);
  capture(&actual);
  EXPECT_EQ(BackdropResources::gTextures, 3);
  // Render at half scale into 32x32, with the same 64x64 output capture.
  renderer->setRenderScale(0.5f);
  capture(&actual);
  // One scene target and three backdrop textures.
  EXPECT_EQ(BackdropResources::gTextures, 7);
  capture(&actual);
  EXPECT_EQ(BackdropResources::gTextures, 7);
  EXPECT_EQ(BackdropResources::gLibraries, 1);
  EXPECT_EQ(BackdropResources::gPipelines, 4);
}

TEST_F(MetalSplatRendererTest, SceneBackdropAllocationFailureKeepsTheReferenceWithoutRetry) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  const BackdropResources resources;
  std::vector<uint8_t> reference;
  std::vector<uint8_t> actual;
  capture(&reference);
  BackdropResources::gFailTexture = true;
  SceneBackdrop settings;
  settings.enabled = true;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  capture(&actual);
  EXPECT_EQ(actual, reference);
  const int pipelines = BackdropResources::gPipelines;
  const int textures = BackdropResources::gTextures;
  capture(&actual);
  EXPECT_EQ(actual, reference);
  EXPECT_EQ(BackdropResources::gPipelines, pipelines);
  EXPECT_EQ(BackdropResources::gTextures, textures);
}

TEST_F(MetalSplatRendererTest, SceneBackdropPipelineFailureKeepsTheReferenceWithoutRetry) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {1};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  const auto capture = [&](std::vector<uint8_t>* out) {
    auto done = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(done);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  const BackdropResources resources;
  std::vector<uint8_t> reference;
  std::vector<uint8_t> actual;
  capture(&reference);
  BackdropResources::gFailPipeline = true;
  SceneBackdrop settings;
  settings.enabled = true;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  capture(&actual);
  EXPECT_EQ(actual, reference);
  const int pipelines = BackdropResources::gPipelines;
  const int textures = BackdropResources::gTextures;
  capture(&actual);
  EXPECT_EQ(actual, reference);
  EXPECT_EQ(BackdropResources::gPipelines, pipelines);
  EXPECT_EQ(BackdropResources::gTextures, textures);
}

TEST_F(MetalSplatRendererTest, WorldReadinessRequiresGpuCompletionAndResetsOnReplacement) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  auto completed = dispatch_semaphore_create(0);
  auto drawAndWait = [&](const SplatRenderer::Frame& frame) {
    renderer->captureNextFrame(
        [&](std::vector<uint8_t>, uint32_t, uint32_t) { dispatch_semaphore_signal(completed); });
    EXPECT_TRUE(renderer->draw(frame));
    EXPECT_EQ(
        dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  };
  drawAndWait({});
  EXPECT_FALSE(renderer->hasCompletedWorldFrame());
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  for (int replacement = 0; replacement < 2; ++replacement) {
    ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
    EXPECT_FALSE(renderer->hasCompletedWorldFrame());
    drawAndWait({});  // Uploaded, but no CPU order or GPU visibility has run yet.
    EXPECT_FALSE(renderer->hasCompletedWorldFrame());
    drawAndWait(frame);
    EXPECT_TRUE(renderer->hasCompletedWorldFrame());
  }
  ASSERT_TRUE(renderer->createSlab(10, 0));
  EXPECT_FALSE(renderer->hasCompletedWorldFrame());
}

TEST_F(MetalSplatRendererTest, LabelledWorldRejectsCpuOrderAndTiles) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  cloud.labels = {44};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::cpu;
  EXPECT_FALSE(renderer->draw(frame));
  ASSERT_TRUE(renderer->createSlab(1, 0));
  EXPECT_FALSE(renderer->uploadTile(0, cloud));
}

TEST_F(MetalSplatRendererTest, TheFallbackPolicyResolvesWithoutWarnings) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const DeviceCapabilities caps = renderer->deviceCapabilities();
  const RenderPolicyResolution fallback = resolveRenderPolicy(caps.policy.fallback, caps.policy);
  EXPECT_TRUE(fallback.warnings.empty());
  EXPECT_EQ(caps.policy.fallback.sortDepth, RenderPolicy{}.sortDepth);
}

// VulkanSplatPipelineTest.CpuOrderDrawDropsSplatsUnderTheCutoff is the CPU-order half of this.
TEST_F(MetalSplatRendererTest, SubpixelCutoffAppliesLiveOnBothOrderPaths) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const DeviceCapabilities caps = renderer->deviceCapabilities();
  EXPECT_TRUE(caps.policy.subpixelThreshold);
  EXPECT_EQ(caps.policy.fallback.subpixelThreshold, RenderPolicy{}.subpixelThreshold);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  // Focal 58.5 px at depth 2: source sigma 0.3 px, a 0.75 px footprint at the 2.5 sigma quad.
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {1.05e-4f, 0, 0, 1.05e-4f, 0, 1.05e-4f};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  const SplatRenderer::Range range{0, 1};
  const uint32_t order = 0;
  auto centerRed = [&](SplatRenderer::OrderSource source) {
    SplatRenderer::Frame frame;
    frame.orderSource = source;
    frame.ranges = &range;
    frame.rangeCount = 1;
    frame.order = &order;
    frame.orderCount = 1;
    frame.drawCount = 1;
    frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
    dispatch_semaphore_t captured = dispatch_semaphore_create(0);
    std::vector<uint8_t> pixels;
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      pixels = std::move(image);
      dispatch_semaphore_signal(captured);
    });
    EXPECT_TRUE(renderer->draw(frame));
    EXPECT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
              0);
    // Red at the centre over red at a corner, which only the clear colour reaches.
    if (pixels.size() != 64u * 64u * 4u) return -1;
    return pixels[(32 * 64 + 32) * 4 + 2] - pixels[2];
  };
  RenderPolicy policy = caps.policy.fallback;
  for (const auto source : {SplatRenderer::OrderSource::gpu, SplatRenderer::OrderSource::cpu}) {
    if (source == SplatRenderer::OrderSource::gpu && !caps.supportsSubgroups) continue;
    policy.subpixelThreshold = 1.0f;
    ASSERT_TRUE(renderer->applyRenderPolicy(policy, nullptr));
    const int dropped = centerRed(source);
    policy.subpixelThreshold = 0.5f;
    ASSERT_TRUE(renderer->applyRenderPolicy(policy, nullptr));
    const int kept = centerRed(source);
    printf("[ cutoff ] %s order: red over the clear colour %d at 1 px, %d at 0.5 px\n",
           source == SplatRenderer::OrderSource::gpu ? "gpu" : "cpu", dropped, kept);
    EXPECT_EQ(dropped, 0);
    EXPECT_GT(kept, 64);
  }
}

TEST_F(MetalSplatRendererTest, LodLeafCutDrawsEveryLeaf) {
  splat::LodTree tree;
  tree.leafCount = 514;
  tree.nodes.positions = {0, 0, -2};
  tree.nodes.colors = {1, 0, 0};
  tree.nodes.alphas = {1};
  tree.nodes.covariances = {0.25f, 0, 0, 0.25f, 0, 0.25f};
  tree.layout.push_back({{0, 0, -2}, 2, 1, 514});
  for (uint32_t i = 0; i < 514; ++i) {
    const float x = i == 513 ? 0.85f : 0.0f;
    tree.nodes.positions.insert(tree.nodes.positions.end(), {x, 0, -2});
    tree.nodes.colors.insert(tree.nodes.colors.end(),
                             {i == 513 ? 0.0f : 1.0f, i == 513 ? 1.0f : 0.0f, 0});
    tree.nodes.alphas.push_back(0.6f);
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.01f, 0, 0, 0.01f, 0, 0.01f});
    tree.layout.push_back({{x, 0, -2}, 0, 0, 0});
  }
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  ASSERT_TRUE(renderer->uploadLodWorld(tree, 0, 514));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  auto completed = dispatch_semaphore_create(0);
  std::vector<uint8_t> image;
  renderer->captureNextFrame([&](std::vector<uint8_t> pixels, uint32_t, uint32_t) {
    image = std::move(pixels);
    dispatch_semaphore_signal(completed);
  });
  ASSERT_TRUE(renderer->draw(frame));
  ASSERT_EQ(dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
            0);
  ASSERT_EQ(renderer->lastSelectedCount(), 514u);
  ASSERT_EQ(renderer->lastDrawCount(), 514u);
  ASSERT_EQ(image.size(), 64u * 64u * 4u);
  EXPECT_GT(image[(32 * 64 + 32) * 4 + 2], 200);
  EXPECT_GT(image[(32 * 64 + 56) * 4 + 1], 80);
}

TEST_F(MetalSplatRendererTest, LodErrorPolicyRefinesALoadedHierarchyLive) {
  splat::LodTree tree;
  tree.leafCount = 64;
  tree.nodes.positions = {0, 0, -8};
  tree.nodes.colors = {1, 1, 1};
  tree.nodes.alphas = {1};
  tree.nodes.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  tree.layout.push_back({{0, 0, -8}, 0.4f, 1, 64});
  for (uint32_t i = 0; i < 64; ++i) {
    const float x = (static_cast<float>(i % 8) - 3.5f) * 0.1f;
    const float y = (static_cast<float>(i / 8) - 3.5f) * 0.1f;
    tree.nodes.positions.insert(tree.nodes.positions.end(), {x, y, -8});
    tree.nodes.colors.insert(tree.nodes.colors.end(), {1, 1, 1});
    tree.nodes.alphas.push_back(1);
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.001f, 0, 0, 0.001f, 0, 0.001f});
    tree.layout.push_back({{x, y, -8}, 0, 0, 0});
  }
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const DeviceCapabilities caps = renderer->deviceCapabilities();
  EXPECT_TRUE(caps.policy.lodErrorPixels);
  EXPECT_EQ(caps.limits.maxLodCapacitySplats, 4'000'000u);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  ASSERT_TRUE(renderer->uploadLodWorld(tree, 0, 64));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  auto completed = dispatch_semaphore_create(0);
  auto selectedAt = [&](float pixels, uint32_t limit = 0) {
    RenderPolicy policy = caps.policy.fallback;
    policy.lodErrorPixels = pixels;
    policy.lodSplatLimit = limit;
    std::string reason;
    EXPECT_TRUE(renderer->applyRenderPolicy(policy, &reason)) << reason;
    // Selection readback trails submission, so settle over a few frames.
    for (int i = 0; i < 4; ++i) {
      renderer->captureNextFrame(
          [&](std::vector<uint8_t>, uint32_t, uint32_t) { dispatch_semaphore_signal(completed); });
      EXPECT_TRUE(renderer->draw(frame));
      EXPECT_EQ(
          dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
          0);
    }
    return renderer->lastSelectedCount();
  };
  const uint32_t coarse = selectedAt(16);
  const uint32_t fine = selectedAt(0.1f);
  printf("[ LOD ] selected %u at 16 px, %u at 0.1 px\n", coarse, fine);
  EXPECT_EQ(coarse, 1u);
  EXPECT_EQ(fine, 64u);
  EXPECT_EQ(selectedAt(16), 1u);

  // A limit below the refined cut denies the split and raises the threshold instead; lifting
  // the limit restores the policy's threshold at once.
  EXPECT_TRUE(caps.policy.lodSplatLimit);
  EXPECT_EQ(selectedAt(0.1f, 32), 1u);
  EXPECT_GT(renderer->lodPixels(), 0.1f);
  EXPECT_EQ(selectedAt(0.1f), 64u);
  EXPECT_FLOAT_EQ(renderer->lodPixels(), 0.1f);
}

TEST_F(MetalSplatRendererTest, GpuOrderedSplatContributesToThePresentedPixels) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  dispatch_semaphore_t captured = dispatch_semaphore_create(0);
  std::vector<uint8_t> pixels;
  renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
    pixels = std::move(image);
    dispatch_semaphore_signal(captured);
  });
  ASSERT_TRUE(renderer->draw(frame));
  ASSERT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
            0);
  ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
  const size_t center = (32 * 64 + 32) * 4;
  EXPECT_GT(pixels[center + 2], 200);  // red, not the background
  EXPECT_LT(pixels[center + 1], 20);
}

TEST_F(MetalSplatRendererTest, HalfScaleFrameReachesTheDrawableThroughMetalFXOrTheBlit) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  renderer->setRenderScale(0.5f);
#if SPLATKIT_HAS_METALFX
  const bool scalerExpected =
      [MTLFXSpatialScalerDescriptor supportsDevice:MTLCreateSystemDefaultDevice()];
#else
  const bool scalerExpected = false;
#endif
  EXPECT_EQ(renderer->upscalerActive(), scalerExpected);
  EXPECT_TRUE(layer.framebufferOnly);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  auto centerPixel = [&](std::vector<uint8_t>* out) {
    dispatch_semaphore_t captured = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(captured);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
              0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  std::vector<uint8_t> pixels;
  centerPixel(&pixels);
  const size_t center = (32 * 64 + 32) * 4;
  EXPECT_GT(pixels[center + 2], 200);
  EXPECT_LT(pixels[center + 1], 20);

  // Each transition rebuilds the target and the scaler together.
  renderer->setRenderScale(1.0f);
  EXPECT_FALSE(renderer->upscalerActive());
  centerPixel(&pixels);
  EXPECT_GT(pixels[center + 2], 200);
  renderer->setRenderScale(0.5f);
  EXPECT_EQ(renderer->upscalerActive(), scalerExpected);
  centerPixel(&pixels);
  EXPECT_GT(pixels[center + 2], 200);

  renderer->setSpatialUpscaling(false);
  EXPECT_FALSE(renderer->upscalerActive());
  EXPECT_TRUE(layer.framebufferOnly);
  centerPixel(&pixels);
  EXPECT_GT(pixels[center + 2], 200);
  EXPECT_LT(pixels[center + 1], 20);
}

TEST_F(MetalSplatRendererTest, IssFixtureProducesVisiblePixels) {
  const char* path = std::getenv("SPLAT_ISS_PATH");
  if (!path) GTEST_SKIP() << "Set SPLAT_ISS_PATH to the ISS SPZ fixture";
  auto file = splat::MappedFile::open(path);
  ASSERT_TRUE(file);
  splat::SpzDecodeOptions options;
  options.maxShDegree = 1;
  auto decoded = splat::decodeSpz(file.value().data(), file.value().size(), options);
  ASSERT_TRUE(decoded);
  auto& cloud = decoded.value();
  splat::reorderSpatially(cloud);
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(1206, 2622);
  renderer->setLayer(layer);
  renderer->setDrawableSize(1206, 2622);
  ASSERT_TRUE(renderer->draw({}));  // The app presents a background while loading.
  ASSERT_TRUE(renderer->uploadWorld(cloud, 1));
  WalkCamera camera;
  camera.setLookAt({0, 128, -2}, {0, -2, -2}, {1, 0, 0});
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.view = camera.viewMatrix();
  frame.cameraPosition = camera.position();
  frame.proj =
      splat::Mat4::perspective(65.0f * 3.14159265f / 180.0f, 1206.0f / 2622.0f, 0.05f, 200);
  frame.shDegree = 1;
  const SplatRenderer::Range range{0, static_cast<uint32_t>(cloud.count())};
  frame.ranges = &range;
  frame.rangeCount = 1;
  dispatch_semaphore_t captured = dispatch_semaphore_create(0);
  std::vector<uint8_t> pixels;
  for (int iteration = 0; iteration < 3; ++iteration) {
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      pixels = std::move(image);
      dispatch_semaphore_signal(captured);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(
        dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)), 0);
    uint32_t visiblePixels = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (pixels[i] > 40 || pixels[i + 1] > 40 || pixels[i + 2] > 40) ++visiblePixels;
    }
    printf("[ ISS ] %u visible pixels, %u splats, %.2f ms compute, %.2f ms raster\n", visiblePixels,
           renderer->lastDrawCount(), renderer->lastSortMillis(), renderer->lastGpuMillis());
    EXPECT_GT(visiblePixels, 10000u);
  }
}

// A smoke test: it fails only on a crash or a hang. MetalFrameComputeTest pins the handler
// lifetime this relies on.
TEST_F(MetalSplatRendererTest, DestroyingTheRendererWithFramesInFlightDoesNotCrash) {
  constexpr uint32_t kLeaves = 4096;
  constexpr int kRepeats = 50;
  splat::LodTree tree;
  tree.leafCount = kLeaves;
  tree.nodes.positions = {0, 0, -8};
  tree.nodes.colors = {1, 1, 1};
  tree.nodes.alphas = {1};
  tree.nodes.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  tree.layout.push_back({{0, 0, -8}, 0.4f, 1, kLeaves});
  for (uint32_t i = 0; i < kLeaves; ++i) {
    const float x = (static_cast<float>(i % 64) - 31.5f) * 0.02f;
    const float y = (static_cast<float>(i / 64) - 31.5f) * 0.02f;
    tree.nodes.positions.insert(tree.nodes.positions.end(), {x, y, -8});
    tree.nodes.colors.insert(tree.nodes.colors.end(), {1, 1, 1});
    tree.nodes.alphas.push_back(1);
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.001f, 0, 0, 0.001f, 0, 0.001f});
    tree.layout.push_back({{x, y, -8}, 0, 0, 0});
  }
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  for (int repeat = 0; repeat < kRepeats; ++repeat) {
    auto renderer = MetalSplatRenderer::create();
    ASSERT_NE(renderer, nullptr);
    auto layer = [CAMetalLayer layer];
    layer.drawableSize = CGSizeMake(64, 64);
    renderer->setLayer(layer);
    renderer->setDrawableSize(64, 64);
    ASSERT_TRUE(renderer->uploadLodWorld(tree, 0, kLeaves));
    // Both frame slots hold a selection, a sort and a render buffer when the renderer goes.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) ASSERT_TRUE(renderer->draw(frame));
    renderer.reset();
  }
}

}  // namespace
}  // namespace splatkit

namespace splatkit {
namespace {
TEST_F(MetalSplatRendererTest, EmptyBackdropUsesConfiguredBackground) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  RenderPolicy policy;
  policy.backgroundColor = {0.2, 0.4, 0.6};
  ASSERT_TRUE(renderer->applyRenderPolicy(policy, nullptr));
  SceneBackdrop settings;
  settings.enabled = true;
  ASSERT_TRUE(renderer->setSceneBackdrop(settings, nullptr));
  auto done = dispatch_semaphore_create(0);
  std::vector<uint8_t> pixels;
  renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
    pixels = std::move(image);
    dispatch_semaphore_signal(done);
  });
  ASSERT_TRUE(renderer->draw({}));
  ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    EXPECT_NEAR(pixels[i], 153, 2);
    EXPECT_NEAR(pixels[i + 1], 102, 2);
    EXPECT_NEAR(pixels[i + 2], 51, 2);
    EXPECT_EQ(pixels[i + 3], 255);
  }
}

TEST_F(MetalSplatRendererTest, DepthAllocationFailureCanRetryWithoutLatching) {
  const BackdropResources resources;
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  SplatRenderer::Frame frame;
  BackdropResources::gFailDepth = true;
  EXPECT_FALSE(renderer->draw(frame));
  EXPECT_FALSE(renderer->gpuFailed());
  BackdropResources::gFailDepth = false;
  EXPECT_TRUE(renderer->draw(frame));
}

TEST_F(MetalSplatRendererTest, BackdropCompileFailureRejectsEnableAndCanRetry) {
  const BackdropResources resources;
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  EXPECT_EQ(BackdropResources::gLibraries, 0);
  EXPECT_EQ(BackdropResources::gPipelines, 0);
  SceneBackdrop settings;
  settings.enabled = true;
  BackdropResources::gFailLibrary = true;
  std::string reason;
  EXPECT_FALSE(renderer->setSceneBackdrop(settings, &reason));
  EXPECT_FALSE(reason.empty());
  EXPECT_FALSE(renderer->gpuFailed());
  BackdropResources::gFailLibrary = false;
  EXPECT_TRUE(renderer->setSceneBackdrop(settings, &reason));
  EXPECT_TRUE(reason.empty());
}

}  // namespace
}  // namespace splatkit
