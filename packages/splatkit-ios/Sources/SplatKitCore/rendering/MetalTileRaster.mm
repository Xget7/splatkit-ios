#include "rendering/MetalTileRaster.h"
#include "rendering/MetalCompute.h"
#include "rendering/MetalShaderTypes.h"

namespace splatkit {

bool MetalTileRaster::create(id<MTLDevice> device, id<MTLLibrary> library) {
  device_ = device;
  bin_ = metal::pipeline(device, library, "binSplatTiles");
  scanRows_ = metal::pipeline(device, library, "scanLargeSplatRows");
  prepareFallback_ = metal::pipeline(device, library, "prepareSplatTileFallback");
  raster_ = metal::pipeline(device, library, "rasterSplatTiles");
  summarize_ = metal::pipeline(device, library, "summarizeSplatTiles");
  for (auto& buffer : diagnostics_) buffer = metal::buffer(device, 4 * sizeof(uint32_t));
  return bin_ != nil && scanRows_ != nil && prepareFallback_ != nil && raster_ != nil &&
         summarize_ != nil && diagnostics_[0] != nil && diagnostics_[1] != nil &&
         bin_.threadExecutionWidth == 32 && bin_.maxTotalThreadsPerThreadgroup >= 256 &&
         raster_.threadExecutionWidth == 32 && scanRows_.maxTotalThreadsPerThreadgroup >= 32 &&
         prepareFallback_.maxTotalThreadsPerThreadgroup >= 32 &&
         raster_.maxTotalThreadsPerThreadgroup >= 256 &&
         raster_.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength;
}

bool MetalTileRaster::encode(id<MTLCommandBuffer> cmd, id<MTLBuffer> uniforms,
                             id<MTLBuffer> projected, id<MTLBuffer> order, id<MTLBuffer> count,
                             uint32_t capacity, id<MTLTexture> target, uint32_t slot) {
  // Signed rectangle prefix sums must be able to represent every input splat.
  if (capacity > 0x7fffffffu || slot >= diagnostics_.size() || bin_ == nil || scanRows_ == nil ||
      prepareFallback_ == nil || raster_ == nil || target == nil ||
      target.pixelFormat != MTLPixelFormatRGBA16Float || uniforms.length < sizeof(CameraUniform) ||
      projected.length < size_t{capacity} * sizeof(ProjectedSplat) ||
      order.length < size_t{capacity} * sizeof(uint32_t) || count.length < sizeof(uint32_t))
    return false;
  struct Config {
    uint32_t tilesX, tilesY, capacity, candidates;
  };
  static_assert(sizeof(Config) == 16);
  const Config config{static_cast<uint32_t>((target.width + 15) / 16),
                      static_cast<uint32_t>((target.height + 15) / 16), capacity, 512};
  const size_t tiles = size_t{config.tilesX} * config.tilesY;
  const size_t bytes = tiles * config.candidates * sizeof(uint32_t);
  const size_t rectangleBytes = size_t{config.tilesX + 1} * (config.tilesY + 1) * sizeof(int32_t);
  if (bytes + tiles * sizeof(uint32_t) + rectangleBytes + 16 > 128u * 1024u * 1024u) return false;
  if (bins_ == nil || bins_.length < std::max<size_t>(bytes, 16) ||
      rectangles_.length < rectangleBytes) {
    id<MTLBuffer> bins = metal::buffer(device_, bytes, MTLResourceStorageModePrivate);
    id<MTLBuffer> counts =
        metal::buffer(device_, tiles * sizeof(uint32_t), MTLResourceStorageModePrivate);
    id<MTLBuffer> fallback =
        metal::buffer(device_, sizeof(uint32_t), MTLResourceStorageModePrivate);
    id<MTLBuffer> rectangles =
        metal::buffer(device_, rectangleBytes, MTLResourceStorageModePrivate);
    if (bins == nil || counts == nil || fallback == nil || rectangles == nil) return false;
    bins_ = bins;
    counts_ = counts;
    fallback_ = fallback;
    rectangles_ = rectangles;
    bins_.label = @"Bounded exact tile candidates";
  }
  id<MTLBlitCommandEncoder> reset = [cmd blitCommandEncoder];
  [reset fillBuffer:counts_ range:NSMakeRange(0, counts_.length) value:0];
  [reset fillBuffer:fallback_ range:NSMakeRange(0, fallback_.length) value:0];
  [reset fillBuffer:rectangles_ range:NSMakeRange(0, rectangles_.length) value:0];
  [reset endEncoding];
  id<MTLComputeCommandEncoder> bin = [cmd computeCommandEncoder];
  bin.label = @"Bounded exact tile binning";
  [bin setComputePipelineState:bin_];
  [bin setBuffer:uniforms offset:0 atIndex:0];
  [bin setBuffer:projected offset:0 atIndex:1];
  [bin setBuffer:order offset:0 atIndex:2];
  [bin setBuffer:count offset:0 atIndex:3];
  [bin setBuffer:counts_ offset:0 atIndex:4];
  [bin setBuffer:bins_ offset:0 atIndex:5];
  [bin setBuffer:fallback_ offset:0 atIndex:6];
  [bin setBytes:&config length:sizeof(config) atIndex:7];
  [bin setBuffer:rectangles_ offset:0 atIndex:8];
  [bin dispatchThreadgroups:MTLSizeMake((size_t{std::max(capacity, 1u)} + 255) / 256, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [bin endEncoding];
  // Encoder boundaries order the rectangle prefix passes before the raster
  // reads counts, without CPU readback or per-large-splat screen-sized loops.
  id<MTLComputeCommandEncoder> rows = [cmd computeCommandEncoder];
  rows.label = @"Large splat rectangle row prefixes";
  [rows setComputePipelineState:scanRows_];
  [rows setBuffer:rectangles_ offset:0 atIndex:0];
  [rows setBytes:&config length:sizeof(config) atIndex:1];
  [rows dispatchThreadgroups:MTLSizeMake((config.tilesY + 1 + 31) / 32, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  [rows endEncoding];
  id<MTLComputeCommandEncoder> prepare = [cmd computeCommandEncoder];
  prepare.label = @"Per-tile hardware ownership";
  [prepare setComputePipelineState:prepareFallback_];
  [prepare setBuffer:rectangles_ offset:0 atIndex:0];
  [prepare setBuffer:counts_ offset:0 atIndex:1];
  [prepare setBuffer:fallback_ offset:0 atIndex:2];
  [prepare setBytes:&config length:sizeof(config) atIndex:3];
  [prepare dispatchThreadgroups:MTLSizeMake((config.tilesX + 31) / 32, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  [prepare endEncoding];
  id<MTLComputeCommandEncoder> raster = [cmd computeCommandEncoder];
  raster.label = @"Experimental 16x16 tile compositing";
  [raster setComputePipelineState:raster_];
  [raster setBuffer:uniforms offset:0 atIndex:0];
  [raster setBuffer:projected offset:0 atIndex:1];
  [raster setBuffer:order offset:0 atIndex:2];
  [raster setBuffer:count offset:0 atIndex:3];
  [raster setBuffer:counts_ offset:0 atIndex:4];
  [raster setBuffer:bins_ offset:0 atIndex:5];
  [raster setBuffer:fallback_ offset:0 atIndex:6];
  [raster setBytes:&config length:sizeof(config) atIndex:7];
  [raster setTexture:target atIndex:0];
  [raster dispatchThreadgroups:MTLSizeMake(config.tilesX, config.tilesY, 1)
         threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
  [raster endEncoding];
  id<MTLComputeCommandEncoder> stats = [cmd computeCommandEncoder];
  stats.label = @"Hybrid tile diagnostics";
  [stats setComputePipelineState:summarize_];
  [stats setBuffer:counts_ offset:0 atIndex:0];
  [stats setBuffer:fallback_ offset:0 atIndex:1];
  [stats setBytes:&config length:sizeof(config) atIndex:2];
  [stats setBuffer:diagnostics_[slot] offset:0 atIndex:3];
  [stats dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [stats endEncoding];
  return true;
}

}  // namespace splatkit
