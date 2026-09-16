#pragma once

#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

/// Where the camera is and where it looks: position in the world's frame in meters, yaw
/// about the up axis and pitch, both in radians. Pitch is clamped to 85 degrees.
typedef struct {
  float x;
  float y;
  float z;
  float yaw;
  float pitch;
} SKCameraPose;

typedef struct {
  float x;
  float y;
  float z;
} SKVec3;

/// A snapshot of what the engine is doing, refreshed twice a second.
typedef struct {
  float fps;
  float frameMillis;
  float gpuMillis;
  float sortMillis;
  uint32_t splatCount;
  BOOL walking;
  BOOL motion;
  uint32_t drawnSplatCount;
  uint32_t computeTileCount;
  uint32_t nonemptyComputeTileCount;
  uint32_t hardwareTileCount;
} SKSplatStats;

/// The renderer-applicable policy. Mirror of the host contract: raster 0 hardware,
/// 1 computeTile, 2 hybrid; tileSize 8/16/32; sortDepth 16/32.
typedef struct {
  uint32_t raster;
  uint32_t tileSize;
  float lodErrorPixels;
  float alphaThreshold;
  float subpixelThreshold;
  BOOL enableFrustumCulling;
  BOOL enableHiZOcclusion;
  BOOL enableEarlyTermination;
  uint32_t sortDepth;
} SKRenderPolicy;

/// Which policy fields this backend can apply. A false field resolves to `fallback`.
typedef struct {
  BOOL raster;
  BOOL tileSize;
  BOOL lodErrorPixels;
  BOOL alphaThreshold;
  BOOL subpixelThreshold;
  BOOL enableFrustumCulling;
  BOOL enableHiZOcclusion;
  BOOL enableEarlyTermination;
  BOOL sortDepth;
  uint32_t tileSizeMask;
  float minLodErrorPixels;
  float maxLodErrorPixels;
  float minSubpixelThreshold;
  float maxSubpixelThreshold;
} SKRenderPolicySupport;

/// Resource ceilings and device features. Values come from the native adapter.
typedef struct {
  uint32_t maxLodCapacitySplats;
  uint32_t minResidencyCapacitySplats;
  uint32_t maxResidencyCapacitySplats;
  BOOL supportsComputeTiles;
  BOOL supportsHiZOcclusion;
  BOOL supportsSubgroups;
  uint32_t maxTextureDimension;
  SKRenderPolicySupport policy;
  SKRenderPolicy fallback;
} SKDeviceCapabilities;

/// How `applyRenderPolicy:` ended. Anything but applied keeps the previous policy.
typedef NS_ENUM(NSInteger, SKRenderPolicyOutcome) {
  /// Applied; `warnings` lists each unsupported choice that fell back.
  SKRenderPolicyOutcomeApplied = 0,
  /// The request was invalid.
  SKRenderPolicyOutcomeInvalid = 1,
  /// The request was valid but the backend could not prepare it.
  SKRenderPolicyOutcomePreparationFailed = 2,
};

typedef NS_ENUM(NSInteger, SKSplatEvent) {
  SKSplatEventWorldReady = 0,
  SKSplatEventWorldFailed = 1,
  SKSplatEventColliderReady = 2,
  SKSplatEventColliderFailed = 3,
  /// First successful GPU frame after this world's upload, not merely upload completion.
  SKSplatEventWorldFrameReady = 4,
};

/// The native engine behind one view: the shared C++ engine over the Metal renderer.
///
/// Rendering, input and settings belong to one thread, the view's render thread. The
/// load methods may be called from any thread: they decode there and the next frame
/// uploads. Stats and the camera pose are readable from any thread.
@interface SKSplatEngine : NSObject

/// Nil when the device has no Metal.
+ (nullable instancetype)create;
- (instancetype)init NS_UNAVAILABLE;

/// The layer to draw on, or nil when the view is going away. Render thread.
- (void)setLayer:(nullable CAMetalLayer*)layer;
/// The layer's size in pixels. Render thread.
- (void)setDrawableSize:(CGSize)size;
/// One frame, at the display link's timestamp. Render thread.
- (void)render:(int64_t)frameTimeNanos;

/// Loading outcomes, on whichever thread found them.
@property(nonatomic, copy, nullable) void (^eventHandler)
    (SKSplatEvent event, NSString* message, uint32_t splatCount);

- (void)loadWorldFile:(NSString*)path;
- (void)loadTiledWorldFile:(NSString*)path;
- (void)loadColliderFile:(NSString*)path;

@property(nonatomic) SKCameraPose cameraPose;
@property(nonatomic, readonly) SKSplatStats stats;
@property(nonatomic, readonly) NSString* gpuDescription;

/// The policy currently applied to this instance.
@property(nonatomic, readonly) SKRenderPolicy renderPolicy;
/// One capability query for this adapter: limits, features and accepted policy.
@property(nonatomic, readonly) SKDeviceCapabilities deviceCapabilities;

/// Re-validates and applies a policy to this instance only. An invalid request, or one
/// the backend cannot prepare, keeps the previous policy and sets `reason`; otherwise
/// `warnings` lists any unsupported choice that fell back. Render thread.
- (SKRenderPolicyOutcome)applyRenderPolicy:(SKRenderPolicy)policy
                                    reason:(NSString* _Nullable* _Nullable)reason
                                  warnings:(NSArray<NSString*>* _Nullable* _Nullable)warnings;

- (void)lookWithDeltaYaw:(float)deltaYaw deltaPitch:(float)deltaPitch;
/// Scripted camera: from `position` looking at `target` with `up` at the top of the frame.
- (void)lookAtFrom:(SKVec3)position target:(SKVec3)target up:(SKVec3)up;
- (void)walkForward:(float)forward right:(float)right;
- (void)setVelocityForward:(float)forward right:(float)right;
/// Device to reference rotation, row major 3x3, device axes x right, y up, z out of the
/// screen, reference z up.
- (void)setAttitude:(const float*)rowMajor;
- (void)setMotionEnabled:(BOOL)enabled;

- (void)setRenderScale:(float)scale;
- (void)setCullMargin:(float)degrees;
- (void)setLinearBlending:(BOOL)linear;
- (void)setSplatBudget:(int)budget;
- (void)setResidencyBudget:(int)splats;
- (void)setMaxShDegree:(int)degree;
- (void)setShDegree:(int)degree;
- (void)startBenchmark:(float)seconds;

/// Writes the next presented frame to `path` as a PNG, at the layer's resolution.
/// `completion` runs on an arbitrary thread with the outcome. Render thread.
- (void)captureFrameToFile:(NSString*)path completion:(void (^)(BOOL ok))completion;

@end

NS_ASSUME_NONNULL_END
