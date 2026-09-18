#include "splat/formats/GlbEncoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace splat {
namespace {

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;

void putU32(std::vector<std::uint8_t>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

void putBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  out.insert(out.end(), p, p + size);
}

}  // namespace

std::vector<std::uint8_t> encodeGlb(const TriangleMesh& mesh, const GlbEncodeOptions& options) {
  // RUB -> RDF negates Y and Z: a rotation, so triangle winding is unchanged.
  const float sign[3] = {1, options.targetFrame == CoordinateFrame::rdf ? -1.0f : 1.0f,
                         options.targetFrame == CoordinateFrame::rdf ? -1.0f : 1.0f};
  std::vector<float> positions(mesh.positions.size());
  float lo[3];
  float hi[3];
  std::fill(lo, lo + 3, std::numeric_limits<float>::max());
  std::fill(hi, hi + 3, std::numeric_limits<float>::lowest());
  for (std::size_t i = 0; i < positions.size(); ++i) {
    const std::size_t axis = i % 3;
    positions[i] = mesh.positions[i] * sign[axis];
    lo[axis] = std::min(lo[axis], positions[i]);
    hi[axis] = std::max(hi[axis], positions[i]);
  }
  const std::size_t vertices = mesh.vertexCount();
  if (vertices == 0) {
    std::fill(lo, lo + 3, 0.0f);
    std::fill(hi, hi + 3, 0.0f);
  }

  const std::size_t positionBytes = positions.size() * sizeof(float);
  const std::size_t indexBytes = mesh.indices.size() * sizeof(uint32_t);
  using Json = nlohmann::json;
  const Json doc = {
      {"asset", {{"version", "2.0"}, {"generator", "SplatKit collider builder"}}},
      {"scene", 0},
      {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
      {"nodes", Json::array({{{"mesh", 0}}})},
      {"meshes",
       Json::array(
           {{{"primitives",
              Json::array({{{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"mode", 4}}})}}})},
      {"buffers", Json::array({{{"byteLength", positionBytes + indexBytes}}})},
      {"bufferViews",
       Json::array(
           {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", positionBytes}, {"target", 34962}},
            {{"buffer", 0},
             {"byteOffset", positionBytes},
             {"byteLength", indexBytes},
             {"target", 34963}}})},
      {"accessors", Json::array({{{"bufferView", 0},
                                  {"componentType", 5126},
                                  {"count", vertices},
                                  {"type", "VEC3"},
                                  {"min", {lo[0], lo[1], lo[2]}},
                                  {"max", {hi[0], hi[1], hi[2]}}},
                                 {{"bufferView", 1},
                                  {"componentType", 5125},
                                  {"count", mesh.indices.size()},
                                  {"type", "SCALAR"}}})},
  };
  std::string json = doc.dump();
  while (json.size() % 4 != 0) json += ' ';
  // Floats and uint32 indices keep the BIN chunk four-byte aligned, as GLB requires.
  const std::size_t binBytes = positionBytes + indexBytes;

  std::vector<std::uint8_t> glb;
  glb.reserve(12 + 8 + json.size() + 8 + binBytes);
  putU32(glb, kGlbMagic);
  putU32(glb, 2);
  putU32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + binBytes));
  putU32(glb, static_cast<uint32_t>(json.size()));
  putU32(glb, kChunkJson);
  putBytes(glb, json.data(), json.size());
  putU32(glb, static_cast<uint32_t>(binBytes));
  putU32(glb, kChunkBin);
  putBytes(glb, positions.data(), positionBytes);
  putBytes(glb, mesh.indices.data(), indexBytes);
  return glb;
}

}  // namespace splat
