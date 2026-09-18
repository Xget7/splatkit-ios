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
  for (const CoordinateFrame frame : {CoordinateFrame::rdf, CoordinateFrame::rub}) {
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

TEST(GlbEncoder, WritesWorldLabsFrameByDefault) {
  const auto glb = encodeGlb(twoTriangles());
  GlbDecodeOptions asIs;
  asIs.sourceFrame = CoordinateFrame::rub;
  auto raw = decodeGlb(glb.data(), glb.size(), asIs);
  ASSERT_TRUE(raw.ok()) << raw.error().message;
  // Y and Z are negated on disk.
  EXPECT_FLOAT_EQ(raw.value().positions[7], -2);
  EXPECT_FLOAT_EQ(raw.value().positions[8], 3);
}

}  // namespace
}  // namespace splat
