#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// A renderer that is always ready, keeps nothing and draws nothing. Tests override what
// they observe.
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
  bool uploadWorld(const splat::SplatCloud&, int) override { return true; }
  bool createSlab(uint32_t, int) override { return false; }
  bool uploadTile(uint32_t, const splat::SplatCloud&) override { return false; }
  std::optional<GpuWorldInfo> world() const override { return std::nullopt; }
  bool draw(const Frame&) override { return false; }
  double lastGpuMillis() const override { return 0; }
  const std::string& deviceDescription() const override { return description_; }

 private:
  std::string description_ = "stub";
};

}  // namespace splatkit
