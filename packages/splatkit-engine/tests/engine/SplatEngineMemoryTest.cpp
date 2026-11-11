#include "splatkit/engine/SplatEngine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "load-spz.h"
#include "splat/formats/PartLabels.h"
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

std::vector<uint8_t> worldBytes(int count = 1) {
  spz::GaussianCloud cloud;
  cloud.numPoints = count;
  for (int i = 0; i < count; ++i) {
    cloud.positions.insert(cloud.positions.end(), {0, 0, 2});
    cloud.scales.insert(cloud.scales.end(), {0, 0, 0});
    cloud.rotations.insert(cloud.rotations.end(), {0, 0, 0, 1});
    cloud.alphas.push_back(0);
    cloud.colors.insert(cloud.colors.end(), {0, 0, 0});
  }
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, spz::PackOptions{}, &bytes));
  return bytes;
}

TEST(SplatEngineEvents, UploadAndGpuCompletionHaveDistinctOneShotEvents) {
  class CompletingRenderer : public StubRenderer {
   public:
    bool complete = false;
    bool hasCompletedWorldFrame() override { return complete; }
  };
  auto renderer = std::make_unique<CompletingRenderer>();
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  std::vector<SplatEngine::Event> events;
  engine.setEventSink(
      [&](SplatEngine::Event event, const std::string&, uint32_t) { events.push_back(event); });
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  EXPECT_TRUE(events.empty());
  engine.render(1);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], SplatEngine::Event::worldReady);
  observed->complete = true;
  engine.render(2);
  engine.render(3);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[1], SplatEngine::Event::worldFrameReady);
}

// Pauses only the first admission, making late completion independent of machine speed.
class PausingRenderer final : public StubRenderer {
 public:
  mutable std::atomic<bool> first{true};
  mutable std::promise<void> entered;
  std::shared_future<void> resume;
  bool fail = false;
  std::optional<std::uint64_t> availableMemory() const override {
    if (!first.exchange(false)) return kPlenty;
    entered.set_value();
    resume.wait();
    return fail ? kTooLittle : kPlenty;
  }
};

void checkSupersededCompletion(bool fail) {
  auto renderer = std::make_unique<PausingRenderer>();
  auto* observed = renderer.get();
  std::promise<void> resume;
  observed->resume = resume.get_future().share();
  observed->fail = fail;
  auto entered = observed->entered.get_future();
  SplatEngine engine(std::move(renderer));
  std::vector<SplatEngine::Event> events;
  engine.setEventSink([&](auto event, const std::string&, uint32_t) { events.push_back(event); });
  const auto bytes = worldBytes();
  const auto old = engine.beginLoad();
  auto late = std::async(std::launch::async,
                         [&] { return engine.loadWorld(old, bytes.data(), bytes.size()); });
  entered.wait();
  const auto newest = engine.beginLoad();
  const auto replacement = worldBytes(2);
  EXPECT_TRUE(engine.loadWorld(newest, replacement.data(), replacement.size()));
  engine.render(1);
  EXPECT_EQ(events.size(), 1u);
  resume.set_value();
  EXPECT_FALSE(late.get());
  engine.render(2);
  EXPECT_EQ(events.size(), 1u);
  EXPECT_FALSE(engine.loadWorldFile(old, "missing.spz"));
  EXPECT_EQ(events.size(), 1u);
}

TEST(SplatEngineReservations, SupersededSuccessCannotEmit) {
  checkSupersededCompletion(false);
}
TEST(SplatEngineReservations, SupersededFailureCannotEmit) {
  checkSupersededCompletion(true);
}

TEST(SplatEngineReservations, ReservationSuppressesCompletionEvents) {
  class CompletingRenderer : public StubRenderer {
   public:
    bool hasCompletedWorldFrame() override { return true; }
  };
  SplatEngine engine(std::make_unique<CompletingRenderer>());
  std::vector<SplatEngine::Event> events;
  engine.setEventSink([&](auto event, const std::string&, uint32_t) { events.push_back(event); });
  const auto bytes = worldBytes();
  auto request = engine.beginLoad();
  ASSERT_TRUE(engine.loadWorld(request, bytes.data(), bytes.size()));
  engine.render(2);
  ASSERT_EQ(events.size(), 1u);
  request = engine.beginLoad();
  EXPECT_FALSE(engine.loadWorldFile(request, "missing.spz"));
  engine.render(3);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events.back(), SplatEngine::Event::worldFailed);
  EXPECT_EQ(engine.renderer().world()->count, 1u);
  EXPECT_FALSE(engine.loadWorldFile("missing.spz", {}));
}

TEST(SplatEngineEvents, SlowFailureCallbackAllowsReservationsAndRendering) {
  SplatEngine engine(std::make_unique<StubRenderer>());
  const auto bytes = worldBytes();
  ASSERT_TRUE(engine.loadWorld(bytes.data(), bytes.size()));
  engine.render(1);
  std::promise<void> entered;
  auto started = entered.get_future();
  std::promise<void> resume;
  auto released = resume.get_future().share();
  engine.setEventSink([&](auto event, const std::string&, uint32_t) {
    if (event != SplatEngine::Event::worldFailed) return;
    entered.set_value();
    released.wait();
  });
  auto failed = std::async(std::launch::async, [&] { return engine.loadWorldFile("missing.spz"); });
  started.wait();
  auto rendered = std::async(std::launch::async, [&] {
    const auto request = engine.beginLoad();
    const bool accepted = engine.loadWorld(request, bytes.data(), bytes.size());
    engine.render(2);
    return accepted;
  });
  const auto status = rendered.wait_for(std::chrono::seconds(5));
  // Release even on failure so a regression reports a failed assertion rather than hanging.
  resume.set_value();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_FALSE(failed.get());
  EXPECT_TRUE(rendered.get());
}

TEST(SplatEngineEvents, ReadyCallbackCanReserveAnotherLoad) {
  SplatEngine engine(std::make_unique<StubRenderer>());
  uint64_t next = 0;
  engine.setEventSink([&](auto event, const std::string&, uint32_t) {
    if (event == SplatEngine::Event::worldReady) next = engine.beginLoad();
  });
  const auto bytes = worldBytes();
  ASSERT_TRUE(engine.loadWorld(bytes.data(), bytes.size()));
  engine.render(1);
  EXPECT_NE(next, 0u);
  EXPECT_TRUE(engine.loadWorld(next, bytes.data(), bytes.size()));
}

TEST(SplatEngineEvents, LabelFailuresHaveTheirOwnEventAndPreserveTheWorld) {
  SplatEngine engine(std::make_unique<StubRenderer>());
  std::vector<SplatEngine::Event> events;
  engine.setEventSink([&](auto event, const std::string&, uint32_t) { events.push_back(event); });
  const auto bytes = worldBytes();
  ASSERT_TRUE(engine.loadWorld(bytes.data(), bytes.size()));
  engine.render(1);
  auto labels = splat::encodePartLabels({7, 8});
  EXPECT_FALSE(engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size()));
  EXPECT_EQ(events.back(), SplatEngine::Event::labelsMismatch);
  labels[0] = 0;
  EXPECT_FALSE(engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size()));
  EXPECT_EQ(events.back(), SplatEngine::Event::labelsMismatch);
  labels = splat::encodePartLabels({7});
  SplatEngine::LoadOptions options;
  options.expectedLabelsSha256 = std::array<std::uint8_t, 32>{};
  EXPECT_FALSE(engine.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size(), options));
  EXPECT_EQ(events.back(), SplatEngine::Event::labelsMismatch);
  engine.render(2);
  EXPECT_EQ(engine.renderer().world()->count, 1u);
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
