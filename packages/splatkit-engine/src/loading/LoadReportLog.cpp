#include "splatkit/loading/LoadReportLog.h"

#include "splatkit/Log.h"

namespace splatkit {

void logWorldReport(const splat::SplatWorldLoader::WorldReport& r) {
  LOGI("decoded %zu splats in %.0f ms, sh degree %d, bounds y [%.2f, %.2f], reordered in %.0f ms",
       r.splatCount, r.decodeMillis, r.shDegree, r.bounds.min[1], r.bounds.max[1], r.reorderMillis);
  if (r.nodeCount > 0) {
    LOGI("level of detail tree: %zu nodes over %zu splats, built in %.0f ms", r.nodeCount,
         r.splatCount, r.treeMillis);
  }
  if (r.tileCount > 0) LOGI("tiled world: %zu tiles", r.tileCount);
  if (r.availableBytes > 0) {
    LOGI("memory: the load needs about %llu MiB with headroom, %llu MiB free",
         static_cast<unsigned long long>(r.neededBytes >> 20),
         static_cast<unsigned long long>(r.availableBytes >> 20));
  }
}

void logColliderReport(const splat::SplatWorldLoader::ColliderReport& report) {
  LOGI("collider: %zu triangles, grid built in %.0f ms", report.triangleCount, report.millis);
}

}  // namespace splatkit
