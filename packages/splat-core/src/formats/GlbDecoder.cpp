#include "splat/formats/GlbDecoder.h"

#include <cstring>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "splat/math/Mat4.h"

namespace splat {
namespace {

using Json = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;

uint32_t readU32(const std::uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}

struct Accessor {
  const std::uint8_t* data = nullptr;
  std::size_t count = 0;
  std::size_t stride = 0;
  int componentType = 0;
  std::string type;
};

// Resolves accessor -> bufferView -> BIN chunk. Returns false when anything is out of bounds.
bool resolve(const Json& doc, int accessorIndex, const std::uint8_t* bin, std::size_t binSize,
             Accessor& out) {
  const Json& accessors = doc["accessors"];
  if (accessorIndex < 0 || accessorIndex >= static_cast<int>(accessors.size())) return false;
  const Json& a = accessors[static_cast<std::size_t>(accessorIndex)];
  if (!a.contains("bufferView")) return false;
  const int viewIndex = a["bufferView"].get<int>();
  const Json& views = doc["bufferViews"];
  if (viewIndex < 0 || viewIndex >= static_cast<int>(views.size())) return false;
  const Json& view = views[static_cast<std::size_t>(viewIndex)];
  if (view.value("buffer", 0) != 0) return false;  // only the embedded BIN chunk

  // `at` throws on a missing key or a wrong type where operator[] would read past the
  // end; decodeGlb turns the exception into a corrupt result.
  out.componentType = a.at("componentType").get<int>();
  out.type = a.at("type").get<std::string>();
  const auto count = a.at("count").get<std::int64_t>();
  if (count < 0) return false;
  out.count = static_cast<std::size_t>(count);
  const int components = out.type == "SCALAR" ? 1
                         : out.type == "VEC2" ? 2
                         : out.type == "VEC3" ? 3
                                              : 4;
  const std::size_t componentSize = out.componentType == 5126   ? 4
                                    : out.componentType == 5125 ? 4
                                    : out.componentType == 5123 ? 2
                                                                : 1;
  const std::size_t elementSize = static_cast<std::size_t>(components) * componentSize;
  out.stride = view.value("byteStride", 0u);
  if (out.stride == 0) out.stride = elementSize;
  // glTF bounds byteStride to 252; anything past that is a file trying to overflow the
  // check below. Elements may not overlap either.
  if (out.stride < elementSize || out.stride > 252) return false;
  // The count alone must fit the chunk, so the 64 bit product below cannot wrap.
  if (out.count > binSize / elementSize) return false;

  const std::uint64_t offset =
      static_cast<std::uint64_t>(view.value("byteOffset", 0u)) + a.value("byteOffset", 0u);
  const std::uint64_t needed =
      out.count == 0 ? 0 : offset + (out.count - 1) * out.stride + elementSize;
  if (needed > binSize) return false;
  out.data = bin + offset;
  return true;
}

Mat4 nodeTransform(const Json& node) {
  if (node.contains("matrix")) {
    Mat4 m;
    for (std::size_t i = 0; i < 16; ++i) m.m[i] = node.at("matrix").at(i).get<float>();
    return m;  // glTF matrices are column major, like ours
  }
  Mat4 t = Mat4::identity();
  Mat4 r = Mat4::identity();
  Mat4 s = Mat4::identity();
  if (node.contains("translation")) {
    const Json& v = node["translation"];
    t = Mat4::translation({v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()});
  }
  if (node.contains("rotation")) {
    const Json& q = node["rotation"];
    const float x = q.at(0).get<float>();
    const float y = q.at(1).get<float>();
    const float z = q.at(2).get<float>();
    const float w = q.at(3).get<float>();
    r.at(0, 0) = 1 - 2 * (y * y + z * z);
    r.at(0, 1) = 2 * (x * y - w * z);
    r.at(0, 2) = 2 * (x * z + w * y);
    r.at(1, 0) = 2 * (x * y + w * z);
    r.at(1, 1) = 1 - 2 * (x * x + z * z);
    r.at(1, 2) = 2 * (y * z - w * x);
    r.at(2, 0) = 2 * (x * z - w * y);
    r.at(2, 1) = 2 * (y * z + w * x);
    r.at(2, 2) = 1 - 2 * (x * x + y * y);
  }
  if (node.contains("scale")) {
    const Json& v = node["scale"];
    s.at(0, 0) = v.at(0).get<float>();
    s.at(1, 1) = v.at(1).get<float>();
    s.at(2, 2) = v.at(2).get<float>();
  }
  return t * r * s;
}

Result<TriangleMesh> decodeGlbOrThrow(const std::uint8_t* data, std::size_t size,
                                      const GlbDecodeOptions& options);

}  // namespace

// The JSON accessors throw on a field that is missing or of the wrong type; a file from
// the network can do both, and the core never lets an exception out.
Result<TriangleMesh> decodeGlb(const std::uint8_t* data, std::size_t size,
                               const GlbDecodeOptions& options) {
  try {
    return decodeGlbOrThrow(data, size, options);
  } catch (const Json::exception& e) {
    return Error{ErrorCode::corrupt, std::string("GLB JSON is malformed: ") + e.what()};
  }
}

namespace {

Result<TriangleMesh> decodeGlbOrThrow(const std::uint8_t* data, std::size_t size,
                                      const GlbDecodeOptions& options) {
  if (size < 12 || readU32(data) != kGlbMagic) {
    return Error{ErrorCode::unsupportedFormat, "not a GLB file"};
  }
  if (readU32(data + 4) != 2) {
    return Error{ErrorCode::unsupportedFormat, "GLB version other than 2"};
  }
  const std::size_t total = std::min<std::size_t>(readU32(data + 8), size);

  const std::uint8_t* jsonChunk = nullptr;
  std::size_t jsonSize = 0;
  const std::uint8_t* bin = nullptr;
  std::size_t binSize = 0;
  std::size_t offset = 12;
  while (offset + 8 <= total) {
    const uint32_t chunkLength = readU32(data + offset);
    const uint32_t chunkType = readU32(data + offset + 4);
    offset += 8;
    if (offset + chunkLength > total) return Error{ErrorCode::corrupt, "GLB chunk exceeds file"};
    if (chunkType == kChunkJson) {
      jsonChunk = data + offset;
      jsonSize = chunkLength;
    } else if (chunkType == kChunkBin) {
      bin = data + offset;
      binSize = chunkLength;
    }
    offset += chunkLength;
  }
  if (jsonChunk == nullptr) return Error{ErrorCode::corrupt, "GLB without JSON chunk"};

  Json doc = Json::parse(jsonChunk, jsonChunk + jsonSize, nullptr, false);
  if (doc.is_discarded()) return Error{ErrorCode::corrupt, "GLB JSON does not parse"};
  if (doc.contains("extensionsRequired") && !doc["extensionsRequired"].empty()) {
    return Error{ErrorCode::unsupportedFormat, "GLB requires extensions"};
  }
  if (!doc.contains("meshes") || !doc.contains("accessors") || !doc.contains("bufferViews")) {
    return Error{ErrorCode::corrupt, "GLB without meshes"};
  }

  // Frame conversion: RDF -> RUB negates Y and Z, applied on top of every node transform.
  Mat4 frame = Mat4::identity();
  if (options.sourceFrame == CoordinateFrame::rdf) {
    frame.at(1, 1) = -1;
    frame.at(2, 2) = -1;
  }

  TriangleMesh mesh;
  bool corrupt = false;
  auto appendMesh = [&](int meshIndex, const Mat4& transform) {
    const Json& meshes = doc["meshes"];
    if (meshIndex < 0 || meshIndex >= static_cast<int>(meshes.size())) {
      corrupt = true;
      return;
    }
    for (const Json& prim :
         meshes[static_cast<std::size_t>(meshIndex)].value("primitives", Json::array())) {
      if (prim.value("mode", 4) != 4) continue;  // triangles only
      if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
      Accessor pos;
      if (!resolve(doc, prim["attributes"]["POSITION"].get<int>(), bin, binSize, pos) ||
          pos.componentType != 5126 || pos.type != "VEC3") {
        corrupt = true;
        return;
      }

      const auto base = static_cast<uint32_t>(mesh.vertexCount());
      for (std::size_t i = 0; i < pos.count; ++i) {
        float v[3];
        std::memcpy(v, pos.data + i * pos.stride, sizeof(v));
        const Vec3 p = transform.transformPoint({v[0], v[1], v[2]});
        mesh.positions.push_back(p.x);
        mesh.positions.push_back(p.y);
        mesh.positions.push_back(p.z);
      }
      if (prim.contains("indices")) {
        Accessor idx;
        if (!resolve(doc, prim["indices"].get<int>(), bin, binSize, idx) || idx.type != "SCALAR") {
          corrupt = true;
          return;
        }
        for (std::size_t i = 0; i < idx.count; ++i) {
          const std::uint8_t* p = idx.data + i * idx.stride;
          uint32_t value = 0;
          if (idx.componentType == 5125)
            std::memcpy(&value, p, 4);
          else if (idx.componentType == 5123) {
            uint16_t v16 = 0;
            std::memcpy(&v16, p, 2);
            value = v16;
          } else
            value = *p;
          if (value >= pos.count) {
            corrupt = true;
            return;
          }
          mesh.indices.push_back(base + value);
        }
      } else {
        for (uint32_t i = 0; i < pos.count; ++i) mesh.indices.push_back(base + i);
      }
    }
  };

  // Walk the scene graph so node transforms apply; fall back to every mesh when there is none.
  std::function<void(int, const Mat4&)> visit = [&](int nodeIndex, const Mat4& parent) {
    const Json& nodes = doc["nodes"];
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
      corrupt = true;
      return;
    }
    const Json& node = nodes[static_cast<std::size_t>(nodeIndex)];
    const Mat4 world = parent * nodeTransform(node);
    if (node.contains("mesh")) appendMesh(node["mesh"].get<int>(), world);
    for (const Json& child : node.value("children", Json::array())) visit(child.get<int>(), world);
  };
  if (doc.contains("scenes") && !doc["scenes"].empty()) {
    const std::size_t sceneIndex = doc.value("scene", 0u);
    for (const Json& root : doc["scenes"][sceneIndex].value("nodes", Json::array()))
      visit(root.get<int>(), frame);
  } else {
    for (int i = 0; i < static_cast<int>(doc["meshes"].size()); ++i) appendMesh(i, frame);
  }

  if (corrupt) return Error{ErrorCode::corrupt, "GLB geometry references are invalid"};
  if (mesh.triangleCount() == 0) return Error{ErrorCode::corrupt, "GLB has no triangles"};
  return mesh;
}

}  // namespace

}  // namespace splat
