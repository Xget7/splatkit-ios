#import "SplatKit/SKSplatEngine.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics/SKPngWriter.h"
#include "engine/SKTypeConversions.h"
#include "rendering/metal/MetalSplatRenderer.h"
#include "splat/core/CoordinateFrame.h"
#include "splatkit/Log.h"
#include "splatkit/engine/SplatEngine.h"

namespace {

// NSLog reaches both the system log and the console of `devicectl --console`.
void nslogSink(splatkit::LogLevel level, const char* message) {
  const char* tag = level == splatkit::LogLevel::error  ? "E"
                    : level == splatkit::LogLevel::warn ? "W"
                                                        : "I";
  NSLog(@"SplatKit %s: %s", tag, message);
}

// The whole string, so a NUL inside it cannot cut it to a valid prefix.
std::optional<splat::CoordinateFrame> parseFrame(NSString* letters) {
  return splat::CoordinateFrame::parse(std::string_view(
      letters.UTF8String, [letters lengthOfBytesUsingEncoding:NSUTF8StringEncoding]));
}

}  // namespace

@implementation SKSplatEngine {
  splatkit::MetalSplatRenderer* _renderer;  // owned by the engine
  std::unique_ptr<splatkit::SplatEngine> _engine;
  bool _awaitingWorldFrame;
  uint32_t _worldFrameSplatCount;
}

+ (nullable instancetype)create {
  splatkit::setLogSink(&nslogSink);
  auto renderer = splatkit::MetalSplatRenderer::create();
  if (!renderer) return nil;
  return [[self alloc] initWithRenderer:std::move(renderer)];
}

- (instancetype)initWithRenderer:(std::unique_ptr<splatkit::MetalSplatRenderer>)renderer {
  self = [super init];
  if (self == nil) return nil;
  _renderer = renderer.get();
  _engine = std::make_unique<splatkit::SplatEngine>(std::move(renderer));
  __weak SKSplatEngine* weakSelf = self;
  _engine->setEventSink([weakSelf](splatkit::SplatEngine::Event event, const std::string& message,
                                   uint32_t splatCount) {
    SKSplatEngine* strongSelf = weakSelf;
    if (strongSelf == nil) return;
    if (event == splatkit::SplatEngine::Event::worldReady) {
      strongSelf->_awaitingWorldFrame = true;
      strongSelf->_worldFrameSplatCount = splatCount;
    }
    if (strongSelf.eventHandler == nil) return;
    strongSelf.eventHandler(static_cast<SKSplatEvent>(event), @(message.c_str()), splatCount);
  });
  return self;
}

- (void)setLayer:(nullable CAMetalLayer*)layer {
  _renderer->setLayer(layer);
}

- (void)setDrawableSize:(CGSize)size {
  _renderer->setDrawableSize(static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height));
}

- (void)render:(int64_t)frameTimeNanos {
  _engine->render(frameTimeNanos);
  // Read an atomic completion flag, never wait for the GPU on the render thread.
  if (_awaitingWorldFrame && _renderer->hasCompletedWorldFrame()) {
    _awaitingWorldFrame = false;
    if (self.eventHandler)
      self.eventHandler(SKSplatEventWorldFrameReady, @"", _worldFrameSplatCount);
  }
}

- (void)loadWorldFile:(NSString*)path {
  _engine->loadWorldFile(path.UTF8String);
}

- (void)loadTiledWorldFile:(NSString*)path {
  _engine->loadTiledWorldFile(path.UTF8String);
}

- (void)loadColliderFile:(NSString*)path {
  _engine->loadColliderFile(path.UTF8String);
}

- (BOOL)setSourceFrame:(nullable NSString*)letters {
  if (letters == nil) {
    _engine->setSourceFrame(std::nullopt);
    return YES;
  }
  const auto frame = parseFrame(letters);
  if (!frame) return NO;
  _engine->setSourceFrame(frame);
  return YES;
}

+ (nullable NSString*)canonicalSourceFrame:(NSString*)letters {
  const auto frame = parseFrame(letters);
  return frame ? @(frame->name().c_str()) : nil;
}

- (SKCameraPose)cameraPose {
  const splatkit::CameraPose p = _engine->cameraPose();
  return {p.x, p.y, p.z, p.yaw, p.pitch};
}

- (void)setCameraPose:(SKCameraPose)pose {
  _engine->setCameraPose({pose.x, pose.y, pose.z, pose.yaw, pose.pitch});
}

- (void)lookAtFrom:(SKVec3)position target:(SKVec3)target up:(SKVec3)up {
  _engine->setCameraLookAt({position.x, position.y, position.z}, {target.x, target.y, target.z},
                           {up.x, up.y, up.z});
}

- (void)setAnchor:(SKVec3)point {
  _engine->setCameraAnchor({point.x, point.y, point.z});
}

- (BOOL)orbitWithDeltaAzimuth:(float)deltaAzimuth deltaElevation:(float)deltaElevation {
  return _engine->orbit(deltaAzimuth, deltaElevation) ? YES : NO;
}

- (BOOL)dolly:(float)deltaRadius {
  return _engine->dolly(deltaRadius) ? YES : NO;
}

- (BOOL)focusX:(float)x y:(float)y {
  return _engine->focus(x, y) ? YES : NO;
}

- (BOOL)startOrbitAnimationDegrees:(float)degrees
                  degreesPerSecond:(float)degreesPerSecond
                         easeInOut:(BOOL)easeInOut {
  return _engine->startOrbitAnimation(degrees, degreesPerSecond, easeInOut == YES) ? YES : NO;
}

- (SKSplatStats)stats {
  return splatkit::fromCppStats(_engine->stats());
}

- (NSString*)gpuDescription {
  return @(_engine->gpuDescription().c_str());
}

- (SKRenderPolicy)renderPolicy {
  return splatkit::fromCppPolicy(_engine->renderPolicy());
}

- (SKDeviceCapabilities)deviceCapabilities {
  return splatkit::fromCppCapabilities(_engine->deviceCapabilities());
}

- (SKRenderPolicyOutcome)applyRenderPolicy:(SKRenderPolicy)policy
                                    reason:(NSString**)reason
                                  warnings:(NSArray<NSString*>**)warnings {
  splatkit::RenderPolicy requested;
  if (!splatkit::toCppPolicy(policy, &requested)) {
    if (reason != nullptr) *reason = @"sortDepth must be 16 or 32";
    return SKRenderPolicyOutcomeInvalid;
  }
  const splatkit::RenderPolicyResolution resolution = _engine->setRenderPolicy(requested);
  if (!resolution.accepted) {
    if (reason != nullptr) *reason = @(resolution.error.c_str());
    return resolution.preparationFailed ? SKRenderPolicyOutcomePreparationFailed
                                        : SKRenderPolicyOutcomeInvalid;
  }
  if (warnings != nullptr) {
    NSMutableArray<NSString*>* list = [NSMutableArray arrayWithCapacity:resolution.warnings.size()];
    for (const splatkit::RenderPolicyWarning& warning : resolution.warnings) {
      [list addObject:@(warning.message.c_str())];
    }
    *warnings = list;
  }
  return SKRenderPolicyOutcomeApplied;
}

- (BOOL)applyCameraRequest:(SKCameraRequest)request reason:(NSString**)reason {
  const auto resolution = _engine->applyCameraRequest(splatkit::toCppCameraRequest(request));
  if (reason != nullptr) *reason = @(resolution.error.c_str());
  return resolution.accepted;
}

- (SKCameraState)cameraState {
  return splatkit::fromCppCameraState(_engine->cameraState());
}

- (void)lookWithDeltaYaw:(float)deltaYaw deltaPitch:(float)deltaPitch {
  _engine->look(deltaYaw, deltaPitch);
}

- (void)walkForward:(float)forward right:(float)right {
  _engine->walk(forward, right);
}

- (void)setVelocityForward:(float)forward right:(float)right {
  _engine->setVelocity(forward, right);
}

- (BOOL)setCharacter:(SKCharacterSettings)settings {
  splat::CharacterSettings character = _engine->character();
  character.eyeHeight = settings.eyeHeight;
  character.bodyRadius = settings.bodyRadius;
  character.stepHeight = settings.stepHeight;
  return _engine->setCharacter(character) ? YES : NO;
}

- (SKCharacterSettings)character {
  const splat::CharacterSettings& character = _engine->character();
  return SKCharacterSettings{character.eyeHeight, character.bodyRadius, character.stepHeight};
}

- (BOOL)setLens:(SKLensSettings)settings {
  return _engine->setLens(splatkit::lensSettings(settings)) ? YES : NO;
}

+ (BOOL)acceptsLens:(SKLensSettings)settings {
  return splatkit::Lens::with(splatkit::lensSettings(settings)).has_value() ? YES : NO;
}

- (void)setAttitude:(const float*)rowMajor {
  _engine->setAttitude(rowMajor);
}

- (void)setMotionEnabled:(BOOL)enabled {
  _engine->setMotionEnabled(enabled == YES);
}

- (void)setRenderScale:(float)scale {
  _engine->setRenderScale(scale);
}

- (void)setSpatialUpscaling:(BOOL)enabled {
  _engine->setSpatialUpscaling(enabled);
}

- (void)setCullMargin:(float)degrees {
  _engine->setCullMargin(degrees);
}

- (void)setLinearBlending:(BOOL)linear {
  _engine->setLinearBlending(linear == YES);
}

- (void)setSplatBudget:(int)budget {
  _engine->setSplatBudget(budget);
}

- (void)setResidencyBudget:(int)splats {
  _engine->setResidencyBudget(splats);
}

- (void)setMaxShDegree:(int)degree {
  _engine->setMaxShDegree(degree);
}

- (void)setShDegree:(int)degree {
  _engine->setShDegree(degree);
}

- (void)startBenchmark:(float)seconds {
  _engine->startBenchmark(seconds);
}

- (void)captureFrameToFile:(NSString*)path completion:(void (^)(BOOL ok))completion {
  _renderer->captureNextFrame(
      [path, completion](std::vector<uint8_t> bgra, uint32_t width, uint32_t height) {
        const bool ok = splatkit::writePng(path, bgra, width, height);
        if (ok) {
          LOGI("captured %ux%u to %s", width, height, path.UTF8String);
        } else {
          LOGE("capture to %s failed", path.UTF8String);
        }
        completion(ok);
      });
  _engine->requestRedraw();
}

@end
