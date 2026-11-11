#include "splatkit/highlight/Highlight.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace splatkit {
namespace {

constexpr float kTolerance = 1e-6f;
const HighlightLook kLook;

// A host's own look: navy blue, a strong tint and undimmed surroundings.
HighlightLook hostLook() {
  HighlightLook look;
  look.tint[0] = 0x2A / 255.0f;
  look.tint[1] = 0x5B / 255.0f;
  look.tint[2] = 0xD7 / 255.0f;
  look.tintAmount = 0.5f;
  look.partBrightness = 1.2f;
  look.otherBrightness = 1;
  return look;
}

void expectLook(const LabelStyles& styles, std::uint16_t part, std::uint16_t other,
                const HighlightLook& look) {
  for (int c = 0; c < 3; ++c) EXPECT_FLOAT_EQ(styles[part].tint[c], look.tint[c]);
  EXPECT_FLOAT_EQ(styles[part].tintAmount, look.tintAmount);
  EXPECT_FLOAT_EQ(styles[part].brightness, look.partBrightness);
  EXPECT_FLOAT_EQ(styles[other].tintAmount, 0.0f);
  EXPECT_FLOAT_EQ(styles[other].brightness, look.otherBrightness);
}

void finishFade(Highlight& highlight) {
  while (highlight.update(0.05f)) {
  }
}

void expectAsCaptured(const LabelStyle& style) {
  EXPECT_EQ(style.tintAmount, 0.0f);
  EXPECT_EQ(style.brightness, 1.0f);
  EXPECT_EQ(style.opacity, 1.0f);
}

TEST(Highlight, StartsWithEverySplatAsCaptured) {
  Highlight highlight(65536);
  EXPECT_FALSE(highlight.fading());
  EXPECT_FALSE(highlight.update(1));
  for (std::size_t i = 0; i < highlight.styles().states().size(); ++i)
    expectAsCaptured(highlight.styles()[i]);
}

TEST(Highlight, EmphasisesItsPartsAndDimsEverythingElse) {
  Highlight highlight(65536);
  const std::vector<std::uint16_t> labels = {256, 65535};
  highlight.set(labels.data(), labels.size());
  finishFade(highlight);
  const LabelStyles& styles = highlight.styles();
  for (const std::uint16_t label : labels) {
    EXPECT_EQ(styles[label].tintAmount, kLook.tintAmount);
    EXPECT_EQ(styles[label].tint[2], kLook.tint[2]);
    EXPECT_EQ(styles[label].brightness, kLook.partBrightness);
  }
  EXPECT_EQ(styles[0].brightness, kLook.otherBrightness);
  EXPECT_EQ(styles[4].brightness, kLook.otherBrightness);
  EXPECT_EQ(styles[255].tintAmount, 0.0f);
  EXPECT_EQ(styles[4].opacity, 1.0f);
}

TEST(Highlight, FadesOverAQuarterSecondThenRests) {
  Highlight highlight(65536);
  const std::uint16_t label = 5;
  highlight.set(&label, 1);
  EXPECT_TRUE(highlight.fading());
  ASSERT_TRUE(highlight.update(Highlight::kFadeSeconds / 2));
  EXPECT_NEAR(highlight.styles()[label].tintAmount, kLook.tintAmount / 2, kTolerance);
  EXPECT_NEAR(highlight.styles()[1].brightness, (1 + kLook.otherBrightness) / 2, kTolerance);
  ASSERT_TRUE(highlight.update(Highlight::kFadeSeconds / 2));
  EXPECT_FALSE(highlight.fading());
  EXPECT_FALSE(highlight.update(1));
  EXPECT_EQ(highlight.styles()[label].tintAmount, kLook.tintAmount);
}

TEST(Highlight, AnEmptySetFadesBackToTheCapture) {
  Highlight highlight(65536);
  const std::uint16_t label = 5;
  highlight.set(&label, 1);
  finishFade(highlight);
  highlight.set(nullptr, 0);
  ASSERT_TRUE(highlight.update(Highlight::kFadeSeconds / 2));
  EXPECT_EQ(highlight.styles()[label].tint[0], kLook.tint[0]);
  finishFade(highlight);
  for (std::size_t i = 0; i < highlight.styles().states().size(); ++i)
    expectAsCaptured(highlight.styles()[i]);
}

TEST(Highlight, RetargetStartsFromTheCurrentAppearance) {
  Highlight highlight(10);
  const uint16_t first = 5;
  const uint16_t second = 9;
  highlight.set(&first, 1);
  highlight.update(Highlight::kFadeSeconds / 2);
  highlight.set(&second, 1);
  EXPECT_FLOAT_EQ(highlight.styles()[first].brightness, (1 + kLook.partBrightness) / 2);
  finishFade(highlight);
  EXPECT_FLOAT_EQ(highlight.styles()[first].brightness, kLook.otherBrightness);
  EXPECT_FLOAT_EQ(highlight.styles()[second].brightness, kLook.partBrightness);
}

TEST(Highlight, InvalidLabelsWarnOnce) {
  Highlight highlight(45);
  const uint16_t invalid = 45;
  testing::internal::CaptureStderr();
  highlight.set(&invalid, 1);
  highlight.set(&invalid, 1);
  const auto warning = testing::internal::GetCapturedStderr();
  const auto first = warning.find("Highlight label outside world label count");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(warning.find("Highlight label outside world label count", first + 1),
            std::string::npos);
}

TEST(Highlight, RepeatedRetargetingKeepsPaletteBounded) {
  Highlight highlight(45);
  for (uint16_t label = 1; label < 45; ++label) {
    highlight.set(&label, 1);
    highlight.update(0.01f);
    EXPECT_TRUE(highlight.styles().validFor(45));
  }
  finishFade(highlight);
  EXPECT_FLOAT_EQ(highlight.styles()[44].brightness, kLook.partBrightness);
}

TEST(Highlight, TheSetItHeadsForAgainChangesNothing) {
  Highlight highlight(65536);
  const std::vector<std::uint16_t> labels = {256, 65535};
  const std::vector<std::uint16_t> sameParts = {65535, 256, 65535};
  highlight.set(labels.data(), labels.size());
  ASSERT_TRUE(highlight.update(Highlight::kFadeSeconds / 2));
  highlight.set(sameParts.data(), sameParts.size());  // a re-render sends the parts again
  ASSERT_TRUE(highlight.update(Highlight::kFadeSeconds / 2));
  EXPECT_FALSE(highlight.fading());
  highlight.set(sameParts.data(), sameParts.size());
  EXPECT_FALSE(highlight.fading());
  EXPECT_EQ(highlight.styles()[256].tintAmount, kLook.tintAmount);
}

TEST(Highlight, DefaultLookStaysCloseToTheCapture) {
  EXPECT_TRUE(kLook.valid());
  EXPECT_FLOAT_EQ(kLook.tintAmount, 0.12f);
  EXPECT_FLOAT_EQ(kLook.partBrightness, 1.15f);
  EXPECT_FLOAT_EQ(kLook.otherBrightness, 0.55f);
}

TEST(Highlight, AHostLookStylesPartsAndEverythingElse) {
  Highlight highlight(10, hostLook());
  const std::uint16_t part = 5;
  highlight.set(&part, 1);
  finishFade(highlight);
  expectLook(highlight.styles(), part, 1, hostLook());
}

TEST(Highlight, ANewLookFadesTheCurrentPartsToIt) {
  Highlight highlight(10);
  const std::uint16_t part = 5;
  highlight.set(&part, 1);
  finishFade(highlight);
  highlight.setLook(hostLook());
  ASSERT_TRUE(highlight.fading());
  ASSERT_TRUE(highlight.update(Highlight::kFadeSeconds / 2));
  EXPECT_NEAR(highlight.styles()[part].tintAmount, (kLook.tintAmount + 0.5f) / 2, kTolerance);
  finishFade(highlight);
  expectLook(highlight.styles(), part, 1, hostLook());
}

TEST(Highlight, ANewLookWithNothingHighlightedWaitsForParts) {
  Highlight highlight(10);
  highlight.setLook(hostLook());
  EXPECT_FALSE(highlight.fading());
  const std::uint16_t part = 5;
  highlight.set(&part, 1);
  finishFade(highlight);
  expectLook(highlight.styles(), part, 1, hostLook());
}

TEST(Highlight, CompletingFadesKeepsUntintedLooks) {
  // An untinted part darker than captured once read as dimmed when a fade was cut short.
  HighlightLook look;
  look.tintAmount = 0;
  look.partBrightness = 0.8f;
  look.otherBrightness = 0.5f;
  Highlight highlight(45, look);
  for (uint16_t label = 1; label < 45; ++label) {
    highlight.set(&label, 1);
    highlight.update(0.01f);
  }
  finishFade(highlight);
  EXPECT_FLOAT_EQ(highlight.styles()[44].brightness, 0.8f);
  EXPECT_FLOAT_EQ(highlight.styles()[1].brightness, 0.5f);
}

TEST(Highlight, UndimmedSurroundingsKeepTheCapturedStyleThroughFades) {
  Highlight highlight(10, hostLook());
  const std::uint16_t part = 5;
  highlight.set(&part, 1);
  EXPECT_TRUE(hostLook().valid());
  while (highlight.update(0.05f)) {
    expectAsCaptured(highlight.styles()[1]);
  }
  const std::uint16_t next = 7;
  highlight.set(&next, 1);
  finishFade(highlight);
  expectAsCaptured(highlight.styles()[part]);
  expectAsCaptured(highlight.styles()[1]);
}

TEST(Highlight, LooksOutsideTheBoundsAreInvalid) {
  auto look = hostLook();
  look.tintAmount = 1.5f;
  EXPECT_FALSE(look.valid());
  look = hostLook();
  look.tint[1] = -0.1f;
  EXPECT_FALSE(look.valid());
  look = hostLook();
  look.otherBrightness = std::nanf("");
  EXPECT_FALSE(look.valid());
  look = hostLook();
  look.partBrightness = HighlightLook::kMaxBrightness + 0.5f;
  EXPECT_FALSE(look.valid());
}

TEST(Highlight, ClearingWhatIsNotHighlightedChangesNothing) {
  Highlight highlight(65536);
  highlight.set(nullptr, 0);
  EXPECT_FALSE(highlight.fading());
}

}  // namespace
}  // namespace splatkit
