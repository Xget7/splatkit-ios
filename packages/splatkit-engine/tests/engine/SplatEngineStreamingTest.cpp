#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "load-spz.h"
#include "splat/tiles/TileBuilder.h"
#include "splatkit/engine/SplatEngine.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

namespace fs = std::filesystem;

constexpr int kClusterSplats = 60000;  // the leaves of both clusters overflow the residency
constexpr float kClusterGap = 40.0f;
constexpr int kResidency = 100000;
constexpr int64_t kFrameNanos = 16'666'667;
constexpr int kRoundsPerStop = 600;
constexpr float kWobble = 0.001f;  // moves the camera a little each vsync, so each one draws

// Round splats in two clusters `kClusterGap` apart along x.
spz::GaussianCloud clusters() {
  std::mt19937 rng(11);
  std::normal_distribution<float> spread(0.0f, 1.0f);
  spz::GaussianCloud c;
  c.shDegree = 0;
  for (int cluster = 0; cluster < 2; ++cluster) {
    for (int i = 0; i < kClusterSplats; ++i) {
      c.positions.insert(c.positions.end(),
                         {spread(rng) + cluster * kClusterGap, spread(rng), spread(rng)});
      const float s = std::log(0.02f);
      c.scales.insert(c.scales.end(), {s, s, s});
      c.rotations.insert(c.rotations.end(), {0, 0, 0, 1});
      c.alphas.push_back(4.0f);
      c.colors.insert(c.colors.end(), {0.5f, 0.0f, 0.0f});
      ++c.numPoints;
    }
  }
  return c;
}

// A GPU that finishes each frame `kLag` vsyncs after it was submitted, longer than a tile
// takes to decode. It counts tile uploads into a range a frame still running drew.
class LaggingRenderer final : public StubRenderer {
 public:
  static constexpr uint64_t kLag = 200;

  void tick() { ++now_; }
  bool draw(const Frame& frame) override {
    StubRenderer::draw(frame);
    submittedAt_.push_back(now_);
    drawnBy_.push_back(ranges);
    return true;
  }
  uint64_t completedFrames() override {
    uint64_t done = 0;
    while (done < submittedAt_.size() && submittedAt_[done] + kLag <= now_) ++done;
    return done;
  }
  bool uploadTile(uint32_t offset, const splat::SplatCloud& cloud) override {
    const uint64_t end = uint64_t{offset} + cloud.count();
    const uint64_t completed = completedFrames();
    bool drawnBefore = false;
    for (uint64_t frame = 0; frame < drawnBy_.size(); ++frame) {
      for (const Range& r : drawnBy_[frame]) {
        if (offset >= uint64_t{r.offset} + r.count || r.offset >= end) continue;
        drawnBefore = true;
        if (frame >= completed) ++overwritesInFlight;
      }
    }
    if (drawnBefore) ++reuses;
    return true;
  }

  int reuses = 0;              // uploads into a range some frame drew
  int overwritesInFlight = 0;  // overlaps with a range a running frame drew

 private:
  uint64_t now_ = 0;
  std::vector<uint64_t> submittedAt_;
  std::vector<std::vector<Range>> drawnBy_;
};

// A tile is rewritten only once the frames that drew its range finished. The camera draws
// every vsync and jumps between the clusters, so the far one evicts the set the frames
// still running drew.
TEST(SplatEngineStreaming, ATileRangeIsRewrittenOnlyAfterTheFramesThatDrewItFinished) {
  const fs::path dir =
      fs::temp_directory_path() / ("splatkit-stream-" + std::to_string(std::random_device{}()));
  ASSERT_TRUE(fs::create_directories(dir));
  splat::TileBuildOptions options;
  options.chunks.chunkSplats = 8192;
  const auto built = splat::buildTiles(clusters(), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;

  auto owned = std::make_unique<LaggingRenderer>();
  LaggingRenderer& gpu = *owned;
  SplatEngine engine(std::move(owned));
  engine.setResidencyBudget(kResidency);
  engine.loadTiledWorldFile((dir / "tileset.json").string());

  int64_t now = 0;
  for (const float x : {0.0f, kClusterGap, 0.0f, kClusterGap}) {
    for (int round = 0; round < kRoundsPerStop; ++round) {
      const float wobble = kWobble * static_cast<float>(round % 2);
      engine.setCameraLookAt({x + wobble, 0, 4}, {x, 0, 0}, {0, 1, 0});
      gpu.tick();
      engine.render(now += kFrameNanos);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  fs::remove_all(dir);

  EXPECT_GT(gpu.reuses, 0) << "the walk never reused a range, so it proves nothing";
  EXPECT_EQ(gpu.overwritesInFlight, 0);
}

}  // namespace
}  // namespace splatkit
