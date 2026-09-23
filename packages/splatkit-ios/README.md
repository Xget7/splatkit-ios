# splatkit-ios

Gaussian splat rendering for iOS: the shared SplatKit engine drawn with Metal, wrapped in a `UIView`.
Requires iOS 17+ and Apple GPU family 7+ (A14/M1+); unsupported GPUs report unavailable.
Run it on a physical device: the iOS Simulator does not report family 7, so `SplatMetalView.isAvailable` is false and the view renders black.

## Use it

Swift Package Manager: add `https://github.com/Xget7/splatkit-ios`, product `SplatKit`, then `import SplatKit`.
Choose exact version `0.1.0-alpha.5`.

For source builds, build the static libraries with `scripts/build-ios.sh` from the repository root, then add to your target:

- the Swift sources in `Sources/SplatKit`,
- a bridging header importing `SplatKit/SKSplatEngine.h`, with `Sources/SplatKitCore/include` in the header search paths,
- `build/ios/lib` in the library search paths and `-lsplatkit_ios -lsplatkit_engine -lsplat_core -lspz -lzstd -lz -lc++` in the linker flags,
- the Metal, QuartzCore, CoreMotion, ImageIO, CoreGraphics and UniformTypeIdentifiers frameworks.

`scripts/package-ios.sh` builds the XCFramework; `Package.swift` pins its release checksum.

```swift
let view = SplatMetalView()
view.delegate = self
view.loadWorld(file: documents.appendingPathComponent("scene.spz"))
view.loadCollider(file: documents.appendingPathComponent("collider.glb"))
view.setMotionEnabled(true)
view.resume()
```

Forward `resume()`, `pause()` and `release()` from the host's lifecycle; the layer follows the view's window.
A PLY is converted offline by [`scripts/prepare-world.sh`](https://github.com/Xget7/splatkit/blob/main/scripts/prepare-world.sh), which also builds `.lodsplat` trees and colliders.

## API

`SplatMetalView` mirrors `SplatSurfaceView` on Android:

| Member | What it does |
| --- | --- |
| `loadWorld(file:)` | Decodes and shows a `.spz` or `.lodsplat` world; the file is mapped, not copied |
| `loadTiledWorld(tileset:)` | Streams a tiled world made by `splat-tile` within `residencyBudget` |
| `loadCollider(file:)` | Decodes a GLB mesh and enables walk mode |
| `cameraPose` | Position, yaw and pitch; set it to teleport |
| `lookAt(from:target:up:)` | Scripted camera: teleports facing a target with an explicit up vector; the next touch or motion update takes over |
| `renderScale` | Fraction of the view's resolution the splats are drawn at, 0.1 to 2 |
| `cullMarginDegrees` | Angular margin kept drawn around the view |
| `linearBlending` | Blend in linear light instead of the encoded colour space |
| `splatBudget`, `residencyBudget` | Most splats drawn per frame, most splats resident on the GPU; a tiled scene that fits the residency whole is fetched whole, so turning never meets a coarse stand-in |
| `shDegree`, `maxShDegree` | Harmonics drawn, harmonics kept from the file |
| `setWalkVelocity(forward:right:)`, `walk(forward:right:)` | Continuous walking in meters per second, or a single step in meters; the host draws its own control and drives these |
| `look(deltaYaw:deltaPitch:)` | Turns the camera by radians, for a look pad or mouse |
| `setCharacter(_:)`, `character` | The walker's shape in walk mode: `CharacterSettings(eyeHeight:bodyRadius:stepHeight:)` |
| `touchLookEnabled`, `lookSensitivity` | Whether a one-finger drag turns the camera, and radians per point dragged |
| `setMotionEnabled(_:)`, `isMotionEnabled` | Gyroscope driven camera |
| `cameraPoseInterval` | Seconds between `cameraPoseChanged` delegate calls; 0, the default, never |
| `startBenchmark(seconds:)` | A reproducible turn with the frame time distribution logged |
| `captureFrame(to:completion:)` | The next frame as a PNG |
| `renderPolicy`, `deviceCapabilities` | Per-view renderer policy, re-validated on the render thread; invalid or unpreparable requests keep the previous policy. Sort depth, the raster strategy, the LOD error threshold and the LOD splat limit apply with GPU sort, the sub-pixel threshold only under the tight-culling experiment |
| `readStats()`, `gpuDescription` | Frame, GPU and sort times, splats drawn, device name |
| `delegate` | World, collider and camera pose outcomes, on the main thread |

## Raster strategy

Hardware rasterization is the default and the fastest choice for most scenes.
Hybrid screen tiles are an experimental opt-in for scenes where many large translucent splats overlap each pixel, such as close-up interiors:

```swift
var policy = splatView.renderPolicy
policy.raster = 2  // hybrid; 0 restores hardware
splatView.renderPolicy = policy
```

They composite 16×16 tiles in compute on top of the GPU's own tiling, so on distant or sparse scenes they only add work.
ISS at a 45 m orbit on an iPhone 17 Pro drew the same 1.98M splats at 18.9 FPS with tiles and 23.1 FPS without.
The first request builds the tile pipelines; switching back to hardware frees the tile scratch.
`computeTile` is not built and falls back to hardware with a warning.

Touch: a one-finger drag looks around.
The view ships no walking control; the host draws its own, wherever it likes, and drives it with `setWalkVelocity` or `walk`.
`lookSensitivity` tunes the drag, and `touchLookEnabled` turns it off.

## Layout

```
Sources/SplatKitCore/rendering/    renderer, world, visibility, radix, LOD, tiles; shaders/ contains MSL
Sources/SplatKitCore/engine/       SKSplatEngine, the Objective-C boundary over the shared engine
Sources/SplatKitCore/include/      the public header Swift imports
Sources/SplatKit/                  RenderThread, MotionInput, SplatMetalView
cmake/                             embeds the shader source into the library
```

The shader is compiled at run time from the embedded source, so the library is a plain static archive with no metallib to ship.
The renderer keeps two frames in flight and reads GPU time from the command buffer.
GPU path: visibility → radix → indirect draw; compatibility frames may use CPU order.
`sortMillis` includes visibility/radix.
That path draws front to back and stops shading a pixel once it is opaque, so the frame costs what the visible layers cost.
The sort has unit tests that run on a Mac: `cmake -S packages/splatkit-ios -B build/ios-mac && cmake --build build/ios-mac && ctest --test-dir build/ios-mac`.
Colours blend in the encoded space by default, on a `bgra8Unorm` layer; `linearBlending` switches the layer to `bgra8Unorm_srgb`.

## Experiments

Start motion after `splatView(_:worldFrameReady:)`, not upload-only `worldReady`.
Keep the view attached/resumed while loading; readiness means GPU completion, not visual acceptance.

Dev-only switches; `--quality`, `--min-pixel-radius`, `--depth-key-bits`, `--lod-error-pixels`, `--lod-splat-limit` and `--tile-raster` set the view's `renderPolicy`, the rest apply before renderer creation:

| Switch | Effect |
|---|---|
| `--metal-culling 1 --min-pixel-radius 1` | Covariance bounds, opacity/subpixel rejection |
| `--depth-key-bits 16` | Two radix passes; uint32 storage unchanged; ties may shimmer |
| `--tile-raster 1` | Hybrid raster: 16×16 tiles, compute ≤512 candidates, dense/large-footprint tiles use hardware |
| `--budget 4000000` | LOD capacity, at most 4M; not guaranteed quality |
| `--quality ultra` | Starting level of the on-screen picker: `ultra`, `high` (default), `balanced`, `fast` |
| `--shot tour` | ISS camera, +Y up: `overview`, `orbit` (default), `detail`, `flyby`, `tour` |
| `--run-seconds 20` | Bounded run with resource monitoring |

Defaults: 32-bit sorting, hardware raster.
Culling, LOD and depth quantization are approximations pending visual acceptance.
Tile termination uses transmittance ≤0.0001; hardware geometry submission remains.
Private allocations still consume unified memory.
The dev memory guard cannot cancel in-flight work or prevent allocation spikes.

Build `splat_lod_build` from `splat-core/tools`; invoke `splat_lod_build input.spz output.lodsplat --depth 10 --sh 1`.
Output must be new.
Moment-matched parents remain approximate; original leaves survive.
Parents use [moment-matching initialization](https://arxiv.org/html/2406.12080v1#S4.SS1), without training/refinement.
v2 adds interior metadata and leaf packets; v1 remains readable.
Layout/validation: [LodFile.cpp](../splat-core/src/lod/LodFile.cpp).
Selection feeds visibility → radix → raster; denied refinements retain parents and report pressure.
The hierarchy stays resident, without Hi-Z or temporal transitions.

Stats: `loadedSplatCount` counts source splats; `drawnSplatCount` counts completed draw candidates.
Tile counts distinguish compute, nonempty compute and hardware screen tiles.
Command timings overlap; HUD sort includes visibility/radix.
[Measurements](../../docs/BENCHMARKS.md) separate visual rejection, Mac checks and phone evidence.
