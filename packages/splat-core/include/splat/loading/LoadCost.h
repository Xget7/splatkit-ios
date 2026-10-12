#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/core/Result.h"
#include "splat/formats/SpzDecoder.h"

namespace splat {

// What loading a world takes, read from its header before anything is decoded, so a world
// too large for the device fails instead of getting the app ended for memory. The CPU side
// is the loader's own allocations, modelled stage by stage after the code that makes them;
// the GPU side is described by what is uploaded, for the renderer to price in its layout.
struct LoadCost {
  // The most the loader holds at once while it decodes and prepares the world.
  std::uint64_t peakBytes = 0;
  // What stays on the CPU while the world is uploaded: the world, plus the selection data a
  // renderer builds for a tree that does not carry it.
  std::uint64_t residentBytes = 0;
  // The upload: `records` splats with harmonics to `shDegree`, of which a frame draws at most
  // `drawCapacity`, and a tree's `clusters` and `leafRefs` of selection data.
  std::uint64_t records = 0;
  int shDegree = 0;
  std::uint64_t drawCapacity = 0;
  std::uint64_t clusters = 0;
  std::uint64_t leafRefs = 0;
};

// An SPZ world decoded with harmonics up to `maxShDegree`, reordered, and with a `budget`
// turned into a tree that draws that many splats a frame.
Result<LoadCost> splatFileLoadCost(const std::uint8_t* data, std::size_t size, int maxShDegree,
                                   int budget);
// A LODSPLAT world: its tree as stored, drawing `budget` splats a frame.
Result<LoadCost> lodSplatLoadCost(const std::uint8_t* data, std::size_t size, int maxShDegree,
                                  int budget);
// A tiled world: a slab of `residency` splats, filled by tiles as they stream in.
LoadCost slabLoadCost(std::uint32_t residency, int shDegree);

// The model's stages, for the tests that hold them to the code. Bytes allocated by:
// decodeSpz of an SPZ with this header, keeping harmonics up to `degree`.
std::uint64_t spzDecodePeak(const SpzHeader& header, int degree);
// reorderSpatially of a cloud of `splats`.
std::uint64_t reorderPeak(std::uint64_t splats, int degree);
// buildLodTree when its `leaves` make a tree of `nodes`.
std::uint64_t treeBuildPeak(std::uint64_t leaves, std::uint64_t nodes, int degree);
// A cloud of `splats`, and a tree of `nodes` with its selection data.
std::uint64_t cloudBytes(std::uint64_t splats, int degree);
std::uint64_t treeBytes(std::uint64_t nodes, std::uint64_t clusters, std::uint64_t leafRefs,
                        int degree);

}  // namespace splat
