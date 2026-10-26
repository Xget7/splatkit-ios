# splat-core

C++17 library shared by the Android and iOS engines.
No graphics, no platform APIs, no React Native.

## Domains

| Domain | Responsibility | Public headers |
|---|---|---|
| Formats | Decode `.spz` worlds and `.glb` colliders into library-owned types in the internal frame, and encode `.glb` colliders | `splat/formats/*.h`, `splat/core/*.h` |
| Loading | Map files, decode and prepare worlds for a render thread | `splat/io/*.h`, `splat/loading/*.h` |
| Level of detail | Load-time trees, offline `.lodsplat` files and budgeted selection | `splat/lod/*.h` |
| Tiles | Tilesets of tree chunks, tile loading and residency-bounded streaming | `splat/tiles/*.h` |
| Sorting | Spatial reorder for GPU fetch locality and the tile streaming prefetch margin; splats are sorted and culled on the GPU | `splat/sorting/*.h` |
| Math | Column major matrices, vectors and frusta shared by every renderer | `splat/math/*.h` |
| Navigation | Collider grid, raycast, character controller, colliders built from splats | `splat/navigation/*.h` |
| Diagnostics | Timing summaries with percentiles | `splat/diagnostics/*.h` |

## Build and test

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Set `SPLAT_FIXTURES_DIR` to a folder with World Labs example files to run the integration tests.

## Converting a PLY

The engine reads `.spz`, `.lodsplat` and tilesets.
SPZ trees are built locally on the loader CPU; `.lodsplat` files carry a prepared tree ([load pipeline](../../docs/ARCHITECTURE.md#load-shared-cpu)).
[`scripts/prepare-world.sh`](../../scripts/prepare-world.sh) builds these tools and runs `splat-convert`, `splat_lod_build` and `splat_collider` in order; the [React Native guide](https://github.com/Xget7/splatkit/tree/main/packages/react-native-splatkit#preparing-a-world) explains its flags.
Call the tools directly for the options below.
A scene from SuperSplat, Polycam or the Mip-NeRF 360 set comes as a Gaussian splat PLY; `tools/ply2spz` (built with `SPLAT_CORE_BUILD_TOOLS`) packs it:

```
build/tools/ply2spz bicycle.ply bicycle.spz --sh 1 --keep 2
```

`--sh N` keeps harmonics up to degree N (degree 3 costs 92 bytes per splat on the GPU) and `--keep N` keeps every Nth splat, for scenes too big for a phone.
`--prune-alpha T` drops the splats whose stored opacity is below T: at `1/255` (0.0039) only what draws nothing goes, so the picture is unchanged, and any higher value trades the faintest layers for speed, a choice to make with captures side by side.
Neither tool prunes unless asked; the default output is the whole scene.
`tools/splat-tile` takes the same `--sh` and `--prune-alpha` before partitioning a scene into streamed tiles of at most `--tile` splats (65536 by default), each with at most `--fan-out` children (4 by default).
`tools/splat_lod_build` writes an offline `.lodsplat` hierarchy; the [iOS README](../splatkit-ios/README.md) shows its options.
A coordinate frame is three letters from `r l u d b f` saying where a file's +X, +Y and +Z point: `rub` is glTF, OpenGL and three.js, `rdf` OpenCV and COLMAP, `rfu` Blender; [CoordinateFrame.h](include/splat/core/CoordinateFrame.h) takes all 48.
Decoders move every input into the engine's `rub` once, with [FrameTransform.h](include/splat/formats/FrameTransform.h), so positions, shapes and SH up to degree 3 stay exact and nothing after decode knows about frames.
An SPZ is read in the frame its `SPZ_ADOBE_coordinate_system` tag declares, else in the spec default `rub`; a PLY in `rdf`; a GLB in glTF's frame.
`SpzDecodeOptions::sourceFrame`, `SplatWorldLoader::setSourceFrame` and each SDK's source frame override that for a file that stores another frame without saying so, such as `rdf` for World Labs Marble exports and SplatTransform 3.10.0 SPZ.

`ply2spz` and `splat-convert` accept `--spz-version 2|3|4`, `--source-frame F` and `--target-frame F`.
The source defaults to the frame the file declares, else its format's; the target defaults to `rub`, written untagged, which every SPZ reader takes as it is.
Another target is tagged and needs `--spz-version 4`, since spz reads the tag back only from v4 containers.
Conversions transform positions, rotations and SH together.
`splat-tile`, `splat_lod_build` and `splat_collider` take `--source-frame F` too; `splat_collider` then writes the GLB in that frame, so a host loading both with that frame gets them aligned.
`scripts/prepare-world.sh` passes `--source-frame` to the converter and writes `rub`, so its output loads with no frame set:

```sh
scripts/prepare-world.sh marble.spz out/ --source-frame rdf --collider
```

[PlayCanvas's v4-only SPZ parser](https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/spz/) reads the default output when it is v4:

```sh
build/tools/splat-convert scene.ply playcanvas.spz --spz-version 4
```

spz's own frame conversion is wrong for its eight frames whose Y is front or back (pinned commit affd0ec), so SplatKit reads the stored values and converts them itself.

SPZ is lossy: positions have 1/4096-unit steps, log scales 1/16 steps, opacity 1/255 steps and SH uses 5 bits for degree 1 and 4 bits above it.
SPZ v2 uses 8-bit quaternion components; v3/v4 use smallest-three quaternion encoding.
Repacking an SPZ can add quantization error.
The pinned [interop check](../../scripts/spz-interop/test.mjs) verifies an oriented SH3 fixture against PlayCanvas's CPU parser and SplatTransform; it does not establish visual or device acceptance.

```sh
npm ci --prefix ../../scripts/spz-interop --ignore-scripts
node ../../scripts/spz-interop/test.mjs build/tools/splat-convert build/tests/splat_core_tests ../../build/spz-interop
```

## Generating a collider

Walk mode needs a collider.
For a world shipped without one, `buildCollider` (`splat/navigation/ColliderBuilder.h`) makes it from the splats: a C++ port of the voxel collision passes of [PlayCanvas splat-transform](https://github.com/playcanvas/splat-transform).
Splat opacity is summed into 5 cm voxels, the space outside an enclosed scene is filled, and a 1.6 m walker box is flood-filled from the origin; what it cannot reach becomes solid, so floaters and the far side of walls are gone.
A surface net meshes the result, with vertices moved to where the splats are.
`tools/splat_collider` writes it as a `.glb` in glTF's frame:

```
build/tools/splat_collider kitchen.spz kitchen_collider.glb
```

`--voxel`, `--solid-opacity`, `--exterior-fill`, `--floor-fill` (outdoor ground), `--capsule-height`, `--capsule-radius` and `--seed x,y,z` match the options in the header; `--capsule-height 0` keeps every surface.
`--compare reference.glb` reports floor, wall and walk-reach differences against another collider.
The World Labs kitchen (500k splats) builds in about a second on an M-series Mac: its floor is within 1 cm of the collider shipped with it, and the walker reaches no space that collider does not have.
