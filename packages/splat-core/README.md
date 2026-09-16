# splat-core

C++17 library shared by the Android and iOS engines.
No graphics, no platform APIs, no React Native.

## Domains

| Domain | Responsibility | Public headers |
|---|---|---|
| Formats | Decode `.spz` worlds and `.glb` colliders into library-owned types in the internal frame | `splat/formats/*.h`, `splat/core/*.h` |
| Loading | Map files, decode and prepare worlds for a render thread | `splat/io/*.h`, `splat/loading/*.h` |
| Level of detail | Load-time trees, offline `.lodsplat` files and budgeted selection | `splat/lod/*.h` |
| Tiles | Tilesets, tile loading and residency-bounded streaming | `splat/tiles/*.h` |
| Sorting | Distance order on a background thread, spatial reorder and visibility planning | `splat/sorting/*.h` |
| Math | Column major matrices, vectors and frusta shared by every renderer | `splat/math/*.h` |
| Navigation | Collider grid, raycast, character controller | `splat/navigation/*.h` |
| Diagnostics | Timing summaries with percentiles | `splat/diagnostics/*.h` |

## Build and test

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Set `SPLAT_FIXTURES_DIR` to a folder with World Labs example files to run the integration tests.

## Converting a PLY

The engine reads SPZ.
A scene from SuperSplat, Polycam or the Mip-NeRF 360 set comes as a Gaussian splat PLY; `tools/ply2spz` (built with the tests) packs it:

```
build/tools/ply2spz bicycle.ply bicycle.spz --sh 1 --keep 2
```

`--sh N` keeps harmonics up to degree N (degree 3 costs 92 bytes per splat on the GPU) and `--keep N` keeps every Nth splat, for scenes too big for a phone.
`--prune-alpha T` drops the splats whose stored opacity is below T: at `1/255` (0.0039) only what draws nothing goes, so the picture is unchanged, and any higher value trades the faintest layers for speed, a choice to make with captures side by side.
Neither tool prunes unless asked; the default output is the whole scene.
`tools/splat-tile` takes the same `--sh` and `--prune-alpha` before partitioning a scene into streamed tiles.
`tools/splat_lod_build` writes an offline `.lodsplat` hierarchy; the [iOS README](../splatkit-ios/README.md) shows its options.
The coordinates are written as they are; the reference 3DGS frame is what the decoder assumes for a file without a frame tag, so the scene stands upright.
