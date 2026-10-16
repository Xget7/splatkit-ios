#pragma once

#include "splat/loading/SplatWorldLoader.h"

namespace splatkit {

// Writes a successful load's report to the engine log, one line per fact the world has:
// the decode, then its level of detail tree, its tiles and the memory check when present.
void logWorldReport(const splat::SplatWorldLoader::WorldReport& report);
void logColliderReport(const splat::SplatWorldLoader::ColliderReport& report);

}  // namespace splatkit
