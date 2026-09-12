// One library, with each stage maintained in its own source file.
#include "SplatRaster.metal"
#include "SplatVisibility.metal"
#include "PrepareIndirect.metalh"
#include "SplatRadixSort.metal"
#include "SplatTileRaster.metal"
#include "SplatLOD.metal"
