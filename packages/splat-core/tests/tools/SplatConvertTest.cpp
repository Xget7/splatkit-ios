#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "load-spz.h"
#include "splat/formats/SpzDecoder.h"
#include "splat/formats/SpzFrame.h"
#include "support/TempPath.h"

namespace splat {
namespace {

// Shared pose with scripts/spz-interop/test.mjs: asymmetric axes, rotation and SH degree 3
// expose frame changes that an isotropic, SH-0 room would hide.
spz::GaussianCloud orientedCloud() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.shDegree = 3;
  cloud.positions = {1.125f, -2.375f, 3.625f};
  cloud.scales = {-1.0f, -2.0f, -3.0f};
  cloud.rotations = {0.2f, 0.3f, 0.4f, std::sqrt(0.71f)};
  cloud.alphas = {std::log(3.0f)};
  cloud.colors = {0.1f, 0.2f, 0.3f};
  for (int i = 0; i < 45; ++i) cloud.sh.push_back(0.1f + static_cast<float>(i % 9) * 0.025f);
  return cloud;
}

std::string shellQuote(const std::string& value) {
  std::string result = "'";
  for (const char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
}

class SplatConvert : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(spz::saveSplatToPly(orientedCloud(), {}, base_ + ".ply")); }
  int convert(const std::string& input, const std::string& options = "",
              const std::string& executable = SPLAT_CONVERT_PATH) const {
    const auto command = shellQuote(executable) + " " + shellQuote(input) + " " +
                         shellQuote(base_ + ".spz") + " " + options;
    return std::system(command.c_str());
  }
  // Checks the values stored in the file, then that decoding it, by its tag or SPZ's default
  // unless `sourceFrame` overrides both, gives the pose in the internal frame.
  void expectPose(bool rub, const std::string& path = "",
                  std::optional<CoordinateFrame> sourceFrame = std::nullopt) const {
    const auto file = path.empty() ? base_ + ".spz" : path;
    const auto cloud = spz::loadSpz(file, {});
    ASSERT_EQ(cloud.numPoints, 1);
    ASSERT_EQ(cloud.shDegree, 3);
    const auto expected = orientedCloud();
    const std::array<float, 3> axes{1.0f, rub ? -1.0f : 1.0f, rub ? -1.0f : 1.0f};
    const std::array<float, 15> signs{-1, -1, 1, -1, 1, 1, -1, 1, -1, 1, -1, -1, 1, -1, 1};
    for (std::size_t i = 0; i < 3; ++i) {
      EXPECT_NEAR(cloud.positions[i], expected.positions[i] * axes[i], 1.0f / 4096.0f);
      EXPECT_NEAR(cloud.scales[i], expected.scales[i], 1.0f / 16.0f);
    }
    // Compare rotations up to the equivalent quaternion sign. v2 has lower precision.
    float dot = 0.0f;
    for (std::size_t i = 0; i < 4; ++i)
      dot += cloud.rotations[i] * expected.rotations[i] * (i < 3 ? axes[i] : 1.0f);
    EXPECT_GT(std::abs(dot), 0.999f);
    ASSERT_EQ(cloud.sh.size(), expected.sh.size());
    for (std::size_t i = 0; i < cloud.sh.size(); ++i)
      EXPECT_NEAR(cloud.sh[i], expected.sh[i] * (rub ? signs[i / 3] : 1.0f), 0.07f);
    EXPECT_NEAR(1.0f / (1.0f + std::exp(-cloud.alphas[0])), 0.75f, 1.0f / 255.0f);
    std::ifstream input(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const auto decoded = decodeSpz(bytes.data(), bytes.size(), {sourceFrame});
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_FLOAT_EQ(decoded.value().positions[1], 2.375f);
    EXPECT_FLOAT_EQ(decoded.value().positions[2], -3.625f);
    EXPECT_EQ(decoded.value().shDegree, 3);
    // All six covariance entries should match a raw RUB export after SDK normalization.
    auto reference = orientedCloud();
    reference.convertCoordinates(spz::CoordinateSystem::RDF, spz::CoordinateSystem::RUB);
    spz::PackOptions pack;
    pack.version = 2;
    std::vector<std::uint8_t> referenceBytes;
    ASSERT_TRUE(spz::saveSpz(reference, pack, &referenceBytes));
    const auto referenceDecoded =
        decodeSpz(referenceBytes.data(), referenceBytes.size(), {CoordinateFrame::kRub});
    ASSERT_TRUE(referenceDecoded.ok());
    for (std::size_t i = 0; i < 6; ++i)
      EXPECT_NEAR(decoded.value().covariances[i], referenceDecoded.value().covariances[i], 0.003f);
  }
  const TempPath dir_{"splat-convert", TempKind::directory};
  // A space and a quote in every path the tool is handed, for the shell quoting.
  const std::string base_ = (dir_.path() / "splat convert 'fixture").string();
};

TEST_F(SplatConvert, LegacyPlyCommandDefaultsToVersion2AndRub) {
  ASSERT_EQ(convert(base_ + ".ply", "", PLY2SPZ_PATH), 0);
  EXPECT_EQ(spz::loadSpzPacked(base_ + ".spz").version, 2u);
  expectPose(true);
}

class SplatConvertVersion : public SplatConvert, public testing::WithParamInterface<int> {};

// A frame other than rub is declared by a tag, which spz reads back only from version 4.
std::vector<std::string> targetFrames(int version) {
  if (version < 4) return {"rub"};
  return {"rdf", "rub"};
}

TEST_P(SplatConvertVersion, ExportsOrientedPlyInEveryFrameItCanDeclare) {
  for (const auto& frame : targetFrames(GetParam())) {
    SCOPED_TRACE(frame);
    ASSERT_EQ(convert(base_ + ".ply",
                      "--spz-version " + std::to_string(GetParam()) + " --target-frame " + frame),
              0);
    EXPECT_EQ(spz::loadSpzPacked(base_ + ".spz").version, static_cast<std::uint32_t>(GetParam()));
    expectPose(frame == "rub");
  }
}

TEST_P(SplatConvertVersion, ReadsAnUntaggedSpzInSpzsDefaultFrame) {
  spz::PackOptions pack;
  pack.version = static_cast<std::uint32_t>(GetParam());
  pack.from = spz::CoordinateSystem::RDF;  // stores RUB, SPZ's default
  ASSERT_TRUE(spz::saveSpz(orientedCloud(), pack, base_ + "-input.spz"));
  ASSERT_EQ(convert(base_ + "-input.spz", "--spz-version " + std::to_string(GetParam())), 0);
  expectPose(true);
}

TEST_P(SplatConvertVersion, InterpretsRubPlyWithoutAnImplicitFrameFlip) {
  auto cloud = orientedCloud();
  cloud.convertCoordinates(spz::CoordinateSystem::RDF, spz::CoordinateSystem::RUB);
  ASSERT_TRUE(spz::saveSplatToPly(cloud, {}, base_ + ".ply"));
  for (const auto& frame : targetFrames(GetParam())) {
    SCOPED_TRACE(frame);
    ASSERT_EQ(convert(base_ + ".ply", "--source-frame rub --spz-version " +
                                          std::to_string(GetParam()) + " --target-frame " + frame),
              0);
    expectPose(frame == "rub");
  }
}

INSTANTIATE_TEST_SUITE_P(Versions, SplatConvertVersion, testing::Values(2, 3, 4));

TEST_F(SplatConvert, ImportsExternalOrReferenceSpz) {
  // The interop CI job substitutes SplatTransform's actual v4 output for this reference.
  const char* external = std::getenv("SPLAT_INTEROP_FIXTURE_PATH");
  const char* frame = std::getenv("SPLAT_INTEROP_SOURCE_FRAME");
  const std::string sourceFrame = frame != nullptr ? frame : "rub";
  ASSERT_TRUE(sourceFrame == "rdf" || sourceFrame == "rub");
  spz::PackOptions pack;
  pack.version = 4;
  pack.from = spz::CoordinateSystem::RDF;
  ASSERT_TRUE(spz::saveSpz(orientedCloud(), pack, base_ + "-input.spz"));
  if (external != nullptr) {
    expectPose(sourceFrame == "rub", external, CoordinateFrame::parse(sourceFrame));
  }
  ASSERT_EQ(convert(external != nullptr ? external : base_ + "-input.spz",
                    "--source-frame " + sourceFrame + " --spz-version 4"),
            0);
  expectPose(true);
}

TEST_F(SplatConvert, RejectsInvalidOptionsBeforeCreatingAnOutput) {
  for (const auto* options :
       {"--spz-version 1", "--spz-version 5", "--spz-version 4junk", "--spz-version nan",
        "--spz-version", "--source-frame xyz", "--target-frame auto", "--sh -1", "--sh 4",
        "--sh junk", "--keep 0", "--prune-alpha nan", "--prune-alpha 1.1", "--drop-over -1",
        "--drop-over inf"}) {
    SCOPED_TRACE(options);
    EXPECT_NE(convert(base_ + ".ply", options), 0);
    EXPECT_FALSE(std::ifstream(base_ + ".spz").good());
  }
}

TEST_F(SplatConvert, DeclaresATargetFrameOtherThanRub) {
  ASSERT_EQ(convert(base_ + ".ply", "--spz-version 4 --target-frame rdf"), 0);
  EXPECT_EQ(taggedFrame(spz::loadSpz(base_ + ".spz", {})), CoordinateFrame::kRdf);
}

TEST_F(SplatConvert, RefusesToDeclareAFrameBeforeVersion4) {
  EXPECT_NE(convert(base_ + ".ply", "--spz-version 3 --target-frame rdf"), 0);
  EXPECT_FALSE(std::ifstream(base_ + ".spz").good());
}

// spz writes the tag into gzip containers as well, but its own reader drops it there.
TEST_F(SplatConvert, HonorsTheFrameAnInputDeclaresInAnyVersion) {
  auto cloud = orientedCloud();
  ASSERT_TRUE(convertFrame(cloud, CoordinateFrame::kRdf, CoordinateFrame::kRdf));  // tags it rdf
  for (const std::uint32_t version : {2u, 4u}) {
    SCOPED_TRACE(version);
    spz::PackOptions pack;
    pack.version = version;
    ASSERT_TRUE(spz::saveSpz(cloud, pack, base_ + "-input.spz"));
    ASSERT_EQ(convert(base_ + "-input.spz"), 0);
    expectPose(true);
    EXPECT_FALSE(taggedFrame(spz::loadSpz(base_ + ".spz", {})).has_value());
  }
}

}  // namespace
}  // namespace splat
