# splat-core

C++17 library shared by the Android and iOS engines.
No graphics, no platform APIs, no React Native.

## Domains

| Domain | Responsibility | Public headers |
|---|---|---|
| Formats | Decode `.spz` and `.glb` into library-owned types in the internal frame | `splat/formats/*.h` |
| Sorting | Back to front order by distance, on a background thread | `splat/sorting/*.h` |
| Math | Column major matrices and vectors shared by every renderer | `splat/math/*.h` |
| Navigation | Collider grid, raycast, character controller | `splat/navigation/*.h` |

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
The coordinates are written as they are; the reference 3DGS frame is what the decoder assumes for a file without a frame tag, so the scene stands upright.
