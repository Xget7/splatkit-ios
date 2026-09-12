#include "splat/tiles/Tileset.h"

#include <nlohmann/json.hpp>

namespace splat {
namespace {

constexpr int kVersion = 1;

Error corrupt(const std::string& what) {
  return Error{ErrorCode::corrupt, "tileset: " + what};
}

}  // namespace

std::string writeTileset(const Tileset& tileset) {
  nlohmann::json out;
  out["version"] = kVersion;
  out["shDegree"] = tileset.shDegree;
  out["splatCount"] = tileset.splatCount;
  out["root"] = tileset.root;
  nlohmann::json tiles = nlohmann::json::array();
  for (const Tile& t : tileset.tiles) {
    nlohmann::json j;
    j["file"] = t.file;
    j["level"] = t.level;
    j["min"] = t.bounds.min;
    j["max"] = t.bounds.max;
    j["count"] = t.count;
    j["error"] = t.error;
    j["children"] = t.children;
    tiles.push_back(std::move(j));
  }
  out["tiles"] = std::move(tiles);
  return out.dump(1);
}

Result<Tileset> readTileset(const std::string& json) {
  nlohmann::json in = nlohmann::json::parse(json, nullptr, false);
  if (in.is_discarded() || !in.is_object()) return corrupt("not JSON");
  if (in.value("version", 0) != kVersion) return corrupt("unknown version");
  Tileset set;
  try {
    set.shDegree = in.at("shDegree").get<int>();
    set.splatCount = in.at("splatCount").get<std::size_t>();
    set.root = in.at("root").get<std::uint32_t>();
    for (const auto& j : in.at("tiles")) {
      Tile t;
      t.file = j.at("file").get<std::string>();
      t.level = j.at("level").get<int>();
      t.bounds.min = j.at("min").get<std::array<float, 3>>();
      t.bounds.max = j.at("max").get<std::array<float, 3>>();
      t.count = j.at("count").get<std::uint32_t>();
      t.error = j.at("error").get<float>();
      t.children = j.at("children").get<std::vector<std::uint32_t>>();
      set.tiles.push_back(std::move(t));
    }
  } catch (const nlohmann::json::exception& e) {
    return corrupt(e.what());
  }
  if (set.root >= set.tiles.size()) return corrupt("root out of range");
  for (const Tile& t : set.tiles) {
    for (const std::uint32_t c : t.children) {
      if (c >= set.tiles.size()) return corrupt("child out of range");
      if (set.tiles[c].level >= t.level) return corrupt("a child is not below its parent");
    }
  }
  return set;
}

}  // namespace splat
