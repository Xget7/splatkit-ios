#import "SplatKit/SKSplatEngine.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdio>
#include <memory>
#include <string>

#include "rendering/MetalSplatRenderer.h"
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

// BGRA rows, top down, to a PNG file. Colours are written as they were presented.
bool writePng(NSString* path, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || bgra.size() != size_t{width} * height * 4) return false;
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, bgra.data(), bgra.size(), nullptr);
  const CGBitmapInfo info = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
  CGImageRef image = CGImageCreate(width, height, 8, 32, size_t{width} * 4, space, info, provider,
                                   nullptr, false, kCGRenderingIntentDefault);
  bool ok = false;
  if (image != nullptr) {
    NSURL* url = [NSURL fileURLWithPath:path];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    if (dest != nullptr) {
      CGImageDestinationAddImage(dest, image, nullptr);
      ok = CGImageDestinationFinalize(dest);
      CFRelease(dest);
    }
    CGImageRelease(image);
  }
  CGDataProviderRelease(provider);
  CGColorSpaceRelease(space);
  return ok;
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

- (SKSplatStats)stats {
  const splatkit::Stats s = _engine->stats();
  return {s.fps,
          s.frameMillis,
          s.gpuMillis,
          s.sortMillis,
          s.splatCount,
          s.walking,
          s.motion,
          s.drawnSplatCount,
          s.computeTileCount,
          s.nonemptyComputeTileCount,
          s.hardwareTileCount};
}

- (NSString*)gpuDescription {
  return @(_engine->gpuDescription().c_str());
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

- (void)setAttitude:(const float*)rowMajor {
  _engine->setAttitude(rowMajor);
}

- (void)setMotionEnabled:(BOOL)enabled {
  _engine->setMotionEnabled(enabled == YES);
}

- (void)setRenderScale:(float)scale {
  _engine->setRenderScale(scale);
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
        const bool ok = writePng(path, bgra, width, height);
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
