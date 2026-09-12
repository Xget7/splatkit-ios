#include "splat/formats/GlbDecoder.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace splat {
namespace {

void putU32(std::vector<std::uint8_t>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

std::vector<std::uint8_t> packGlb(std::string json, const std::vector<std::uint8_t>& bin) {
  while (json.size() % 4 != 0) json += ' ';
  std::vector<std::uint8_t> glb;
  putU32(glb, 0x46546C67);
  putU32(glb, 2);
  putU32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
  putU32(glb, static_cast<uint32_t>(json.size()));
  putU32(glb, 0x4E4F534A);
  glb.insert(glb.end(), json.begin(), json.end());
  putU32(glb, static_cast<uint32_t>(bin.size()));
  putU32(glb, 0x004E4942);
  glb.insert(glb.end(), bin.begin(), bin.end());
  return glb;
}

// One triangle at y = 2 plus uint16 indices padded to 4 bytes.
std::vector<std::uint8_t> triangleBin() {
  std::vector<std::uint8_t> bin;
  const float positions[9] = {0, 2, 0, 1, 2, 0, 0, 2, 1};
  bin.insert(bin.end(), reinterpret_cast<const std::uint8_t*>(positions),
             reinterpret_cast<const std::uint8_t*>(positions) + sizeof(positions));
  const uint16_t idx[4] = {0, 1, 2, 0};
  bin.insert(bin.end(), reinterpret_cast<const std::uint8_t*>(idx),
             reinterpret_cast<const std::uint8_t*>(idx) + 8);
  return bin;
}

// The JSON of oneTriangleGlb with the node, with the position accessor's count as given.
std::string withAccessorCount(const std::string& count) {
  return std::string("{\"asset\":{\"version\":\"2.0\"},") +
         "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"translation\":[0,0,1]}]"
         "," +
         "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
         "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" +
         count +
         ",\"type\":\"VEC3\"},"
         "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
         "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
         "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
         "\"buffers\":[{\"byteLength\":44}]}";
}

// A GLB with one triangle at y = 2 under a node translated by (0, 0, 1), in RDF.
std::vector<std::uint8_t> oneTriangleGlb(bool withNode = true, bool uint16Indices = true) {
  std::vector<std::uint8_t> bin;
  const float positions[9] = {0, 2, 0, 1, 2, 0, 0, 2, 1};
  bin.insert(bin.end(), reinterpret_cast<const std::uint8_t*>(positions),
             reinterpret_cast<const std::uint8_t*>(positions) + sizeof(positions));
  if (uint16Indices) {
    const uint16_t idx[4] = {0, 1, 2, 0};  // padded to 4 bytes
    bin.insert(bin.end(), reinterpret_cast<const std::uint8_t*>(idx),
               reinterpret_cast<const std::uint8_t*>(idx) + 8);
  } else {
    const uint32_t idx[3] = {0, 1, 2};
    bin.insert(bin.end(), reinterpret_cast<const std::uint8_t*>(idx),
               reinterpret_cast<const std::uint8_t*>(idx) + 12);
  }
  const std::string indexType = uint16Indices ? "5123" : "5125";
  const std::string indexBytes = uint16Indices ? "6" : "12";
  const std::string json =
      std::string("{\"asset\":{\"version\":\"2.0\"},") +
      (withNode ? "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"translation\":"
                  "[0,0,1]}],"
                : "") +
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
      "{\"bufferView\":1,\"componentType\":" +
      indexType +
      ",\"count\":3,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":" +
      indexBytes +
      "}],"
      "\"buffers\":[{\"byteLength\":" +
      std::to_string(bin.size()) + "}]}";
  return packGlb(json, bin);
}

TEST(GlbDecoder, RejectsAnAccessorCountThatWouldOverflowTheBoundsCheck) {
  // (count - 1) * stride wraps to about zero in 64 bits, so the old check passed and the
  // decoder then walked quintillions of elements.
  auto glb = packGlb(withAccessorCount("4611686018427387905"), triangleBin());
  auto r = decodeGlb(glb.data(), glb.size());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::corrupt);
}

TEST(GlbDecoder, RejectsAnAccessorWithoutAComponentTypeInsteadOfCrashing) {
  std::string json = withAccessorCount("3");
  const auto at = json.find("\"componentType\":5126,");
  ASSERT_NE(at, std::string::npos);
  json.erase(at, std::string("\"componentType\":5126,").size());
  auto glb = packGlb(json, triangleBin());
  auto r = decodeGlb(glb.data(), glb.size());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::corrupt);
}

TEST(GlbDecoder, RejectsAShortTranslationInsteadOfReadingPastIt) {
  std::string json = withAccessorCount("3");
  const auto at = json.find("\"translation\":[0,0,1]");
  ASSERT_NE(at, std::string::npos);
  json.replace(at, std::string("\"translation\":[0,0,1]").size(), "\"translation\":[0]");
  auto glb = packGlb(json, triangleBin());
  auto r = decodeGlb(glb.data(), glb.size());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::corrupt);
}

TEST(GlbDecoder, RejectsNonGlb) {
  const std::uint8_t junk[16] = {1, 2, 3};
  auto r = decodeGlb(junk, sizeof(junk));
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::unsupportedFormat);
}

TEST(GlbDecoder, AppliesNodeTransformAndFrameConversion) {
  auto glb = oneTriangleGlb();
  auto r = decodeGlb(glb.data(), glb.size());
  ASSERT_TRUE(r.ok()) << r.error().message;
  const TriangleMesh& m = r.value();
  ASSERT_EQ(m.triangleCount(), 1u);
  ASSERT_EQ(m.vertexCount(), 3u);
  // Vertex 0: (0, 2, 0) translated to (0, 2, 1) in RDF, then RDF -> RUB flips y and z.
  EXPECT_FLOAT_EQ(m.positions[0], 0);
  EXPECT_FLOAT_EQ(m.positions[1], -2);
  EXPECT_FLOAT_EQ(m.positions[2], -1);
  EXPECT_EQ(m.indices[2], 2u);
}

TEST(GlbDecoder, ReadsUint32IndicesAndMeshesWithoutScene) {
  auto glb = oneTriangleGlb(false, false);
  auto r = decodeGlb(glb.data(), glb.size(), {CoordinateFrame::rub});
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_FLOAT_EQ(r.value().positions[1], 2);  // no conversion, no node
  EXPECT_EQ(r.value().indices[1], 1u);
}

TEST(GlbDecoder, RejectsTruncatedBin) {
  auto glb = oneTriangleGlb();
  glb.resize(glb.size() - 20);
  auto r = decodeGlb(glb.data(), glb.size());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::corrupt);
}

// Opt-in: the real World Labs collider. Set SPLAT_FIXTURES_DIR to a folder holding
// house_collider.glb.
TEST(GlbDecoder, DecodesWorldLabsCollider) {
  const char* dir = std::getenv("SPLAT_FIXTURES_DIR");
  if (dir == nullptr) GTEST_SKIP() << "SPLAT_FIXTURES_DIR not set";
  std::ifstream file(std::string(dir) + "/house_collider.glb", std::ios::binary);
  if (!file.is_open()) GTEST_SKIP() << "house_collider.glb not present";
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  auto r = decodeGlb(bytes.data(), bytes.size());
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_GT(r.value().triangleCount(), 100000u);
  // Floor below the origin once Y points up.
  float minY = 1e9f;
  for (std::size_t i = 1; i < r.value().positions.size(); i += 3)
    minY = std::min(minY, r.value().positions[i]);
  EXPECT_LT(minY, 0.0f);
}

}  // namespace
}  // namespace splat
