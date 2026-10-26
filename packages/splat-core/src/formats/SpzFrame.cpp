#include "splat/formats/SpzFrame.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>

#include "coordinate-system-adobe.h"
#include "load-spz.h"
#include "splat-extensions.h"
#include "splat/formats/FrameTransform.h"

namespace splat {
namespace {

// spz::CoordinateSystem in enum order from 1, each named by its frame's three letters.
constexpr const char* kSpzNames[] = {"ldb", "rdb", "lub", "rub", "ldf", "rdf", "luf", "ruf",
                                     "lfd", "rfd", "lfu", "rfu", "lbd", "rbd", "lbu", "rbu"};

constexpr int kMaxShDegree = 3;

}  // namespace

std::optional<CoordinateFrame> frameOfSpz(spz::CoordinateSystem system) {
  const auto index = static_cast<std::size_t>(system);
  if (index < 1 || index > std::size(kSpzNames)) return std::nullopt;
  return CoordinateFrame::parse(kSpzNames[index - 1]);
}

std::optional<spz::CoordinateSystem> spzSystemOf(CoordinateFrame frame) {
  const std::string name = frame.name();
  const auto* const found = std::find(std::begin(kSpzNames), std::end(kSpzNames), name);
  if (found == std::end(kSpzNames)) return std::nullopt;
  return static_cast<spz::CoordinateSystem>(std::distance(std::begin(kSpzNames), found) + 1);
}

std::optional<CoordinateFrame> taggedFrame(const spz::GaussianCloud& cloud) {
  const auto tag =
      spz::findExtensionByType<spz::SpzExtensionCoordinateSystemAdobe>(cloud.extensions);
  if (!tag) return std::nullopt;
  return frameOfSpz(tag->resolve());
}

bool convertFrame(spz::GaussianCloud& cloud, CoordinateFrame from, CoordinateFrame to) {
  const auto system = spzSystemOf(to);
  if (!system || cloud.shDegree > kMaxShDegree) return false;
  const FrameTransform transform(from, to);
  const auto n = static_cast<std::size_t>(cloud.numPoints);
  const std::size_t shStride = n == 0 ? 0 : cloud.sh.size() / n;
  for (std::size_t i = 0; !transform.isIdentity() && i < n; ++i) {
    transform.applyToPoint(&cloud.positions[i * 3]);
    transform.applyToRotationAndScale(&cloud.rotations[i * 4], &cloud.scales[i * 3]);
    if (shStride > 0) transform.applyToSh(&cloud.sh[i * shStride], cloud.shDegree);
  }
  auto& extensions = cloud.extensions;
  extensions.erase(std::remove_if(extensions.begin(), extensions.end(),
                                  [](const spz::SpzExtensionBasePtr& e) {
                                    return e->extensionType ==
                                           spz::SpzExtensionType::SPZ_ADOBE_coordinate_system;
                                  }),
                   extensions.end());
  if (to != CoordinateFrame::kRub) {
    auto tag = std::make_shared<spz::SpzExtensionCoordinateSystemAdobe>();
    tag->coordinateSystem = *system;
    extensions.push_back(std::move(tag));
  }
  return true;
}

}  // namespace splat
