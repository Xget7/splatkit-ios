#include "splatkit/loading/LoadReportLog.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "splatkit/Log.h"

namespace splatkit {
namespace {

std::vector<std::string>* gLines = nullptr;

void capture(LogLevel, const char* message) {
  gLines->emplace_back(message);
}

struct CapturedLog {
  std::vector<std::string> lines;
  CapturedLog() {
    gLines = &lines;
    setLogSink(&capture);
  }
  ~CapturedLog() {
    setLogSink(nullptr);
    gLines = nullptr;
  }
  CapturedLog(const CapturedLog&) = delete;
  CapturedLog& operator=(const CapturedLog&) = delete;
  CapturedLog(CapturedLog&&) = delete;
  CapturedLog& operator=(CapturedLog&&) = delete;
  bool mention(const char* text) const {
    for (const auto& line : lines) {
      if (line.find(text) != std::string::npos) return true;
    }
    return false;
  }
};

TEST(LoadReportLog, AWorldReportLogsOnlyWhatTheWorldHas) {
  CapturedLog log;
  splat::SplatWorldLoader::WorldReport plain;
  plain.splatCount = 1234;
  plain.shDegree = 2;
  logWorldReport(plain);
  EXPECT_TRUE(log.mention("decoded 1234 splats"));
  EXPECT_FALSE(log.mention("level of detail"));
  EXPECT_FALSE(log.mention("tiled world"));
  EXPECT_FALSE(log.mention("memory"));
  log.lines.clear();

  splat::SplatWorldLoader::WorldReport rich = plain;
  rich.nodeCount = 7;
  rich.tileCount = 3;
  rich.neededBytes = 512ull << 20;
  rich.availableBytes = 2048ull << 20;
  logWorldReport(rich);
  EXPECT_TRUE(log.mention("level of detail tree: 7 nodes"));
  EXPECT_TRUE(log.mention("tiled world: 3 tiles"));
  EXPECT_TRUE(log.mention("512 MiB with headroom, 2048 MiB free"));
}

TEST(LoadReportLog, AColliderReportLogsItsTriangles) {
  const CapturedLog log;
  splat::SplatWorldLoader::ColliderReport report;
  report.triangleCount = 42;
  logColliderReport(report);
  EXPECT_TRUE(log.mention("collider: 42 triangles"));
}

}  // namespace
}  // namespace splatkit
