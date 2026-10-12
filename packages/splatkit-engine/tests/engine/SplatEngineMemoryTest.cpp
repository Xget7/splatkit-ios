#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "load-spz.h"
#include "splatkit/engine/SplatEngine.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

constexpr std::uint64_t kMebibyte = 1024 * 1024;
// Less than the loader keeps in reserve for everything else, so no world fits in it.
constexpr std::uint64_t kTooLittle = 64 * kMebibyte;
constexpr std::uint64_t kPlenty = 4096 * kMebibyte;

// A platform that says how much memory the app has left before it is ended.
class MemoryReportingRenderer final : public StubRenderer {
 public:
  explicit MemoryReportingRenderer(std::optional<std::uint64_t> available)
      : available_(available) {}
  std::optional<std::uint64_t> availableMemory() const override { return available_; }

 private:
  std::optional<std::uint64_t> available_;
};

std::vector<uint8_t> worldBytes() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {0, 0, 2};
  cloud.scales = {0, 0, 0};
  cloud.rotations = {0, 0, 0, 1};
  cloud.alphas = {0};
  cloud.colors = {0, 0, 0};
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, spz::PackOptions{}, &bytes));
  return bytes;
}

// The message of the world failure a load reports, if it fails.
std::optional<std::string> failureLoading(std::optional<std::uint64_t> available) {
  SplatEngine engine(std::make_unique<MemoryReportingRenderer>(available));
  std::optional<std::string> failure;
  engine.setEventSink([&](SplatEngine::Event event, const std::string& message, uint32_t) {
    if (event == SplatEngine::Event::worldFailed) failure = message;
  });
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  return failure;
}

TEST(SplatEngineMemory, AWorldTheAppCannotHoldFailsSayingWhatItNeeds) {
  const auto failure = failureLoading(kTooLittle);
  ASSERT_TRUE(failure.has_value());
  EXPECT_NE(failure->find("GB of memory to load"), std::string::npos) << *failure;
}

TEST(SplatEngineMemory, AWorldThatFitsLoads) {
  EXPECT_EQ(failureLoading(kPlenty), std::nullopt);
}

TEST(SplatEngineMemory, WhereThePlatformDoesNotSayLoadsAreNotGuarded) {
  EXPECT_EQ(failureLoading(std::nullopt), std::nullopt);
}

}  // namespace
}  // namespace splatkit
