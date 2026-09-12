#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "splat/formats/SplatCloud.h"
#include "splat/lod/LodTree.h"
#include "splat/math/Mat4.h"
#include "splat/math/Vec3.h"

namespace splatkit {

struct Extent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// The world the renderer holds: `count` records, harmonics up to `shDegree`. A single
// file world has exactly its splats; a slab has its capacity, filled by tiles.
struct GpuWorldInfo {
  uint32_t count = 0;
  int shDegree = 0;
};

// Screen-tile ownership from the last completed hybrid frame. All zero when
// unavailable. Compute includes background tiles; nonemptyCompute excludes them.
struct ScreenTileStats {
  uint32_t compute = 0;
  uint32_t nonemptyCompute = 0;
  uint32_t hardware = 0;
};

// What the engine needs from a platform's graphics API: a surface it can draw the
// world on, a world it can upload whole or by tiles into a slab, and a frame drawn from
// an order the sorter wrote. Vulkan on Android, Metal on iOS. Render thread only.
//
// Attaching and resizing the surface are platform calls made by the platform's own
// view code, so they are not part of this interface.
class SplatRenderer {
 public:
  virtual ~SplatRenderer() = default;

  // Fraction of the surface resolution the splats are drawn at, [0.1, 2]. Away from one
  // the frame is drawn offscreen and rescaled with a linear blit.
  virtual void setRenderScale(float scale) = 0;
  virtual float renderScale() const = 0;
  // Blend in linear light instead of the encoded space.
  virtual void setLinearBlending(bool linear) = 0;
  virtual bool linearBlending() const = 0;
  // Off, frame times stop being multiples of the vsync, which benchmarks need.
  virtual void setVsync(bool vsync) = 0;

  // True when a surface is up: frames can be drawn and worlds uploaded.
  virtual bool ready() const = 0;
  // Where the splats are drawn: the surface's size with the render scale applied.
  virtual Extent drawExtent() const = 0;
  // Counts the rebuilds of the surface's images. A frame drawn before one is gone.
  virtual uint32_t generation() const = 0;

  // Uploads a world and draws it from now on. Fails, keeping the previous world, when
  // the upload does.
  virtual bool uploadWorld(const splat::SplatCloud& cloud, int maxShDegree) = 0;
  // Optional native GPU hierarchy selection. Unsupported renderers retain CPU LOD.
  virtual bool selectsLodOnGpu() const { return false; }
  virtual bool uploadLodWorld(const splat::LodTree&, int, uint32_t) { return false; }
  // Replaces the world with an empty slab of `capacity` records for a tiled world;
  // tiles land in it through `uploadTile`.
  virtual bool createSlab(uint32_t capacity, int shDegree) = 0;
  // Uploads a tile into records [offset, offset + count) of the slab (blocking).
  virtual bool uploadTile(uint32_t offset, const splat::SplatCloud& cloud) = 0;
  virtual std::optional<GpuWorldInfo> world() const = 0;

  // A run of the world's records: what a tile occupies in a slab.
  struct Range {
    uint32_t offset = 0;
    uint32_t count = 0;
  };
  // True when the renderer culls and sorts on the GPU: the engine then hands it the
  // ranges to draw in every frame instead of an order.
  virtual bool sortsOnGpu() const { return false; }

  enum class OrderSource { cpu, gpu };

  struct Frame {
    // Explicit even for an empty frame: a null range pointer is not a CPU fallback.
    OrderSource orderSource = OrderSource::cpu;
    // A new draw order for the world, copied in before the draw; nullptr keeps the last.
    const uint32_t* order = nullptr;
    uint32_t orderCount = 0;
    uint32_t drawCount = 0;  // entries of the order buffer to draw
    // For a renderer that sorts on the GPU: the ranges to draw, every frame. The order
    // fields are unused then.
    const Range* ranges = nullptr;
    uint32_t rangeCount = 0;
    int shDegree = 0;  // capped by what the world carries
    splat::Mat4 view = splat::Mat4::identity();
    splat::Mat4 proj = splat::Mat4::identity();
    splat::Vec3 cameraPosition;
  };
  // Records and presents one frame. Returns false when nothing was presented, e.g. the
  // surface was rebuilt instead; the caller keeps the order for the next frame.
  virtual bool draw(const Frame& frame) = 0;

  // GPU time of the most recently completed frame, from timestamps at both ends of it.
  // Zero until the first frame completes or if unsupported.
  virtual double lastGpuMillis() const = 0;
  // GPU time of the last visibility pass, when the renderer sorts on the GPU.
  virtual double lastSortMillis() const { return 0; }
  // Splats the last frame drew, when the renderer sorts on the GPU.
  virtual uint32_t lastDrawCount() const { return 0; }
  virtual uint32_t lastSelectedCount() const { return 0; }
  virtual double lastSelectMillis() const { return 0; }
  virtual ScreenTileStats lastScreenTileStats() const { return {}; }
  // GPU name and API version, for a HUD.
  virtual const std::string& deviceDescription() const = 0;
};

}  // namespace splatkit
