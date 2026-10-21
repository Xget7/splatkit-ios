#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// A renderer that is always ready and sorts and selects level of detail on the GPU, the
// production shape every engine test runs against. It keeps the world it was given and
// records what it was asked to draw; tests override what else they observe.
class StubRenderer : public SplatRenderer {
 public:
  void setRenderScale(float) override {}
  float renderScale() const override { return 1; }
  void setLinearBlending(bool) override {}
  bool linearBlending() const override { return false; }
  void setVsync(bool) override {}
  bool ready() const override { return true; }
  Extent drawExtent() const override { return {100, 100}; }
  uint32_t generation() const override { return 0; }
  bool sortsOnGpu() const override { return true; }
  bool selectsLodOnGpu() const override { return true; }
  bool uploadWorld(const splat::SplatCloud& cloud, int shDegree) override {
    world_ = GpuWorldInfo{static_cast<uint32_t>(cloud.count()), shDegree};
    return true;
  }
  bool uploadLodWorld(const splat::LodTree& tree, int shDegree, uint32_t budget) override {
    lodBudget = budget;
    return uploadWorld(tree.nodes, shDegree);
  }
  bool createSlab(uint32_t capacity, int shDegree) override {
    world_ = GpuWorldInfo{capacity, shDegree};
    return true;
  }
  bool uploadTile(uint32_t, const splat::SplatCloud&) override { return true; }
  std::optional<GpuWorldInfo> world() const override { return world_; }
  bool draw(const Frame& frame) override {
    ranges.clear();
    for (uint32_t i = 0; i < frame.rangeCount; ++i) ranges.push_back(frame.ranges[i]);
    proj = frame.proj;
    ++frames;
    return true;
  }
  // Each frame finishes as soon as it is submitted.
  uint64_t submittedFrames() const override { return frames; }
  uint64_t completedFrames() override { return frames; }
  double lastGpuMillis() const override { return 0; }
  const std::string& deviceDescription() const override { return description_; }

  std::vector<Range> ranges;  // what the last draw was given
  uint32_t frames = 0;        // draws accepted so far
  uint32_t lodBudget = 0;     // the budget the last uploadLodWorld carried
  // The projection the last draw was given.
  splat::Mat4 proj = splat::Mat4::identity();

 private:
  std::optional<GpuWorldInfo> world_;
  std::string description_ = "stub";
};

}  // namespace splatkit
