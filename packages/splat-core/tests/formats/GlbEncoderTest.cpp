#include "splat/formats/GlbDecoder.h"
#include "splat/formats/GlbEncoder.h"

#include <gtest/gtest.h>

namespace splat {
namespace {

TriangleMesh twoTriangles() {
  TriangleMesh mesh;
  mesh.positions = {0, 0, 0, 1, 0, 0, 1, 2, -3, 0, 2, -3};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

TEST(GlbEncoder, RoundTripsThroughTheDecoderInEitherFrame) {
  for (const CoordinateFrame frame : {CoordinateFrame::kRdf, CoordinateFrame::kRub}) {
    const auto glb = encodeGlb(twoTriangles(), {frame});
    EXPECT_EQ(glb.size() % 4, 0u);
    GlbDecodeOptions decode;
    decode.sourceFrame = frame;
    auto decoded = decodeGlb(glb.data(), glb.size(), decode);
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value().positions, twoTriangles().positions);
    EXPECT_EQ(decoded.value().indices, twoTriangles().indices);
  }
}

TEST(GlbEncoder, WritesTheGltfFrameByDefault) {
  const auto glb = encodeGlb(twoTriangles());
  auto decoded = decodeGlb(glb.data(), glb.size());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().positions, twoTriangles().positions);
}

TEST(GlbEncoder, WritesTheTargetFramesAxes) {
  const auto glb = encodeGlb(twoTriangles(), {CoordinateFrame::kRdf});
  auto raw = decodeGlb(glb.data(), glb.size());
  ASSERT_TRUE(raw.ok()) << raw.error().message;
  // rdf points Y down and Z forward, so both are negated on disk.
  EXPECT_FLOAT_EQ(raw.value().positions[7], -2);
  EXPECT_FLOAT_EQ(raw.value().positions[8], 3);
}

}  // namespace
}  // namespace splat
