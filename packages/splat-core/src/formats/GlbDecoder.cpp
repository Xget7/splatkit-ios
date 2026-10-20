#include "splat/formats/GlbDecoder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "splat/math/Mat4.h"

namespace splat {
namespace {

using Json = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;

// glTF 2.0 enumerations.
constexpr int kComponentUnsignedShort = 5123;
constexpr int kComponentUnsignedInt = 5125;
constexpr int kComponentFloat = 5126;
constexpr int kModeTriangles = 4;

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
  const std::size_t componentSize = out.componentType == kComponentFloat           ? 4
                                    : out.componentType == kComponentUnsignedInt   ? 4
                                    : out.componentType == kComponentUnsignedShort ? 2
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
    r = Mat4::rotation(
        {q.at(0).get<float>(), q.at(1).get<float>(), q.at(2).get<float>(), q.at(3).get<float>()});
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

struct GlbChunks {
  const std::uint8_t* json = nullptr;
  std::size_t jsonSize = 0;
  const std::uint8_t* bin = nullptr;
  std::size_t binSize = 0;
};

Result<GlbChunks> splitGlbChunks(const std::uint8_t* data, std::size_t size) {
  if (size < 12 || readU32(data) != kGlbMagic) {
    return Error{ErrorCode::unsupportedFormat, "not a GLB file"};
  }
  if (readU32(data + 4) != 2) {
    return Error{ErrorCode::unsupportedFormat, "GLB version other than 2"};
  }
  const std::size_t total = std::min<std::size_t>(readU32(data + 8), size);

  GlbChunks chunks;
  std::size_t offset = 12;
  while (offset + 8 <= total) {
    const uint32_t chunkLength = readU32(data + offset);
    const uint32_t chunkType = readU32(data + offset + 4);
    offset += 8;
    if (offset + chunkLength > total) return Error{ErrorCode::corrupt, "GLB chunk exceeds file"};
    if (chunkType == kChunkJson) {
      chunks.json = data + offset;
      chunks.jsonSize = chunkLength;
    } else if (chunkType == kChunkBin) {
      chunks.bin = data + offset;
      chunks.binSize = chunkLength;
    }
    offset += chunkLength;
  }
  if (chunks.json == nullptr) return Error{ErrorCode::corrupt, "GLB without JSON chunk"};
  return chunks;
}

Result<Ok> parseGlbDocument(const GlbChunks& chunks, Json& doc) {
  doc = Json::parse(chunks.json, chunks.json + chunks.jsonSize, nullptr, false);
  if (doc.is_discarded()) return Error{ErrorCode::corrupt, "GLB JSON does not parse"};
  if (doc.contains("extensionsRequired") && !doc["extensionsRequired"].empty()) {
    return Error{ErrorCode::unsupportedFormat, "GLB requires extensions"};
  }
  if (!doc.contains("meshes") || !doc.contains("accessors") || !doc.contains("bufferViews")) {
    return Error{ErrorCode::corrupt, "GLB without meshes"};
  }
  return Ok{};
}

// Frame conversion: RDF -> RUB negates Y and Z, applied on top of every node transform.
Mat4 sourceFrameTransform(CoordinateFrame sourceFrame) {
  Mat4 frame = Mat4::identity();
  if (sourceFrame == CoordinateFrame::rdf) {
    frame.at(1, 1) = -1;
    frame.at(2, 2) = -1;
  }
  return frame;
}

uint32_t readIndex(const Accessor& indices, std::size_t i) {
  const std::uint8_t* p = indices.data + i * indices.stride;
  if (indices.componentType == kComponentUnsignedInt) {
    uint32_t value = 0;
    std::memcpy(&value, p, 4);
    return value;
  }
  if (indices.componentType == kComponentUnsignedShort) {
    uint16_t value = 0;
    std::memcpy(&value, p, 2);
    return value;
  }
  return *p;
}

// Builds the mesh from the document's meshes. A bad reference marks the assembly corrupt
// but the walk goes on: the caller reports it once, after every reference was visited.
struct MeshAssembler {
  Json& doc;  // non-const: operator[] on a missing "nodes" yields null, which reads as empty
  GlbChunks chunks;
  TriangleMesh mesh;
  bool corrupt = false;

  void appendPositions(const Accessor& pos, const Mat4& transform) {
    for (std::size_t i = 0; i < pos.count; ++i) {
      float v[3];
      std::memcpy(v, pos.data + i * pos.stride, sizeof(v));
      const Vec3 p = transform.transformPoint({v[0], v[1], v[2]});
      mesh.positions.push_back(p.x);
      mesh.positions.push_back(p.y);
      mesh.positions.push_back(p.z);
    }
  }

  // False when the index accessor is invalid or an index points past the positions.
  bool appendIndices(const Json& prim, uint32_t base, std::size_t vertexCount) {
    if (!prim.contains("indices")) {
      for (uint32_t i = 0; i < vertexCount; ++i) mesh.indices.push_back(base + i);
      return true;
    }
    Accessor idx;
    if (!resolve(doc, prim["indices"].get<int>(), chunks.bin, chunks.binSize, idx) ||
        idx.type != "SCALAR")
      return false;
    for (std::size_t i = 0; i < idx.count; ++i) {
      const uint32_t value = readIndex(idx, i);
      if (value >= vertexCount) return false;
      mesh.indices.push_back(base + value);
    }
    return true;
  }

  // False when the primitive's accessors are invalid.
  bool appendPrimitive(const Json& prim, const Mat4& transform) {
    Accessor pos;
    if (!resolve(doc, prim["attributes"]["POSITION"].get<int>(), chunks.bin, chunks.binSize, pos) ||
        pos.componentType != kComponentFloat || pos.type != "VEC3")
      return false;
    const auto base = static_cast<uint32_t>(mesh.vertexCount());
    appendPositions(pos, transform);
    return appendIndices(prim, base, pos.count);
  }

  void appendMesh(int meshIndex, const Mat4& transform) {
    const Json& meshes = doc["meshes"];
    if (meshIndex < 0 || meshIndex >= static_cast<int>(meshes.size())) {
      corrupt = true;
      return;
    }
    for (const Json& prim :
         meshes[static_cast<std::size_t>(meshIndex)].value("primitives", Json::array())) {
      if (prim.value("mode", kModeTriangles) != kModeTriangles) continue;  // triangles only
      if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
      if (!appendPrimitive(prim, transform)) {
        corrupt = true;
        return;
      }
    }
  }

  void visit(int nodeIndex, const Mat4& parent) {
    const Json& nodes = doc["nodes"];
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
      corrupt = true;
      return;
    }
    const Json& node = nodes[static_cast<std::size_t>(nodeIndex)];
    const Mat4 world = parent * nodeTransform(node);
    if (node.contains("mesh")) appendMesh(node["mesh"].get<int>(), world);
    for (const Json& child : node.value("children", Json::array())) visit(child.get<int>(), world);
  }

  // Walks the scene graph so node transforms apply; falls back to every mesh when there is none.
  Result<Ok> appendScene(const Mat4& frame) {
    if (!doc.contains("scenes") || doc["scenes"].empty()) {
      for (int i = 0; i < static_cast<int>(doc["meshes"].size()); ++i) appendMesh(i, frame);
      return Ok{};
    }
    // A non-const operator[] past the end grows the array, to 2^64 entries for a scene of -1.
    const Json& scenes = doc["scenes"];
    const std::int64_t sceneIndex = doc.value("scene", std::int64_t{0});
    if (sceneIndex < 0 || static_cast<std::uint64_t>(sceneIndex) >= scenes.size())
      return Error{ErrorCode::corrupt, "GLB default scene is out of range"};
    for (const Json& root :
         scenes.at(static_cast<std::size_t>(sceneIndex)).value("nodes", Json::array()))
      visit(root.get<int>(), frame);
    return Ok{};
  }
};

Result<TriangleMesh> decodeGlbOrThrow(const std::uint8_t* data, std::size_t size,
                                      const GlbDecodeOptions& options) {
  const auto chunks = splitGlbChunks(data, size);
  if (!chunks) return chunks.error();
  Json doc;
  const auto parsed = parseGlbDocument(chunks.value(), doc);
  if (!parsed) return parsed.error();

  MeshAssembler assembler{doc, chunks.value(), {}};
  const auto appended = assembler.appendScene(sourceFrameTransform(options.sourceFrame));
  if (!appended) return appended.error();

  if (assembler.corrupt) return Error{ErrorCode::corrupt, "GLB geometry references are invalid"};
  if (assembler.mesh.triangleCount() == 0) return Error{ErrorCode::corrupt, "GLB has no triangles"};
  return std::move(assembler.mesh);
}

}  // namespace

}  // namespace splat
