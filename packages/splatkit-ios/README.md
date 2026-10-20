# splatkit-ios

Gaussian splat rendering for iOS: the shared SplatKit engine drawn with Metal, wrapped in a `UIView`.
Requires iOS 17+ and Apple GPU family 7+ (A14/M1+); unsupported GPUs report unavailable.
Run it on a physical device: the iOS Simulator does not report family 7, so `SplatMetalView.isAvailable` is false and the view renders black.

## Use it

Swift Package Manager: add `https://github.com/Xget7/splatkit-ios`, product `SplatKit`, then `import SplatKit`.
Choose exact version `0.1.0-beta.1`.

For source builds, build the static libraries with `scripts/build-ios.sh` from the repository root, then add to your target:

- the Swift sources in `Sources/SplatKit`,
- a bridging header importing `SplatKit/SKSplatEngine.h`, with `Sources/SplatKitCore/include` in the header search paths,
- `build/ios/lib` in the library search paths and `-lsplatkit_ios -lsplatkit_engine -lsplat_core -lspz -lzstd -lz -lc++` in the linker flags,
- the Metal, QuartzCore, CoreMotion, ImageIO, CoreGraphics and UniformTypeIdentifiers frameworks.

`scripts/package-ios.sh` builds the XCFramework; `Package.swift` pins its release checksum.
The source-built Objective-C engine exposes atomic `applyCameraRequest:reason:` and the effective `cameraState`; see [the bridge contract](Sources/SplatKitCore/include/SplatKit/SKSplatEngine.h).
These additions require a rebuilt XCFramework.

```swift
let view = SplatMetalView()
view.delegate = self
view.loadWorld(file: documents.appendingPathComponent("scene.spz"))
view.loadCollider(file: documents.appendingPathComponent("collider.glb"))
view.setMotionEnabled(true)
view.resume()
```

Forward `resume()`, `pause()` and `release()` from the host's lifecycle; the layer follows the view's window.
The display link asks for 120 Hz, which an iPhone grants only when the host's Info.plist sets `CADisableMinimumFrameDurationOnPhone` to true.
A world the app cannot hold calls `worldFailed` before it decodes, with the memory it needs and the format to prepare it as, instead of iOS ending the app; [LoadCost.cpp](../splat-core/src/loading/LoadCost.cpp) holds the estimate.
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
| `setAnchor`, `orbit`, `dolly` | Orbit a world point in temporary fly mode; angles are radians and distance is metres |
| `focus(x:y:)` | Pick an anchor through normalized view coordinates against the collider; false keeps the previous anchor |
| `animateOrbit(degrees:degreesPerSecond:easeInOut:)` | Run a finite engine-driven orbit and idle again when it finishes |
| `renderScale` | Fraction of the view's resolution the splats are drawn at, 0.1 to 2 |
| `spatialUpscaling` | Below a render scale of 1, upscale with MetalFX instead of a linear blit; on by default |
| `cullMarginDegrees` | Angular margin kept drawn around the view |
| `linearBlending` | Blend in linear light instead of the encoded colour space |
| `splatBudget`, `residencyBudget` | Most splats drawn per frame, most splats resident on the GPU; a tiled scene that fits the residency whole is fetched whole, so turning never meets a coarse stand-in |
| `shDegree`, `maxShDegree` | Harmonics drawn, harmonics kept from the file |
| `setWalkVelocity(forward:right:)`, `walk(forward:right:)` | Continuous walking in meters per second, or a single step in meters; the host draws its own control and drives these |
| `look(deltaYaw:deltaPitch:)` | Turns the camera by radians, for a look pad or mouse |
| `setCharacter(_:)`, `character` | The walker's shape in walk mode: `CharacterSettings(eyeHeight:bodyRadius:stepHeight:)` |
| `setLens(_:)`, `lens` | Vertical `fieldOfViewDegrees` (10-120) and `depthRange` (near and far planes, at most 100,000:1) in `LensSettings`; nil keeps the default 65 degrees and 0.05-200. Applies at once and reframes the default orbit; false keeps the previous lens. |
| `touchLookEnabled`, `lookSensitivity` | Whether a one-finger drag turns the camera, and radians per point dragged |
| `setMotionEnabled(_:)`, `isMotionEnabled` | Gyroscope driven camera |
| `cameraPoseInterval` | Seconds between `cameraPoseChanged` delegate calls; 0, the default, never |
| `startBenchmark(seconds:)` | A reproducible turn with the frame time distribution logged |
| `captureFrame(to:completion:)` | The next frame as a PNG |
| `renderPolicy`, `deviceCapabilities` | Per-view renderer policy, re-validated on the render thread; invalid or unpreparable requests keep the previous policy. Sort depth, the LOD error threshold and the LOD splat limit apply with GPU sort, the sub-pixel threshold is fixed and a different request resolves to it with a warning |
| `readStats()`, `gpuDescription` | Frame, GPU and sort times, splats drawn, device name |
| `delegate` | World, collider and camera pose outcomes, on the main thread |

## Raster strategy

Hardware rasterization is the only strategy Metal builds.
A policy that asks for `computeTile` or `hybrid` resolves to hardware with a warning, and `deviceCapabilities.supportsComputeTiles` is false.

Touch: a one-finger drag looks around.
The view ships no walking control; the host draws its own, wherever it likes, and drives it with `setWalkVelocity` or `walk`.
`lookSensitivity` tunes the drag, and `touchLookEnabled` turns it off.

## Layout

```
Sources/SplatKitCore/rendering/metal/  MetalSplatRenderer; one folder per job beside its MSL
Sources/SplatKitCore/engine/           SKSplatEngine, the Objective-C boundary over the shared engine
Sources/SplatKitCore/include/          the public header Swift imports
Sources/SplatKit/                      RenderThread, MotionInput, SplatMetalView
cmake/                                 embeds the shader source into the library
```

The shader is compiled at run time from the embedded source, so the library is a plain static archive with no metallib to ship.
The renderer keeps two frames in flight and reads GPU time from the command buffer.
GPU path: visibility → radix → indirect draw; a device where that pipeline cannot be created fails the world load with a clear error.
`sortMillis` includes visibility/radix.
That path draws front to back and stops shading a pixel once it is opaque, so the frame costs what the visible layers cost.
The sort has unit tests that run on a Mac: `cmake -S packages/splatkit-ios -B build/ios-mac && cmake --build build/ios-mac && ctest --test-dir build/ios-mac`.
Colours blend in the encoded space by default, on a `bgra8Unorm` layer; `linearBlending` switches the layer to `bgra8Unorm_srgb`.

## Experiments

Start motion after `splatView(_:worldFrameReady:)`, not upload-only `worldReady`.
Keep the view attached/resumed while loading; readiness means GPU completion, not visual acceptance.

Dev-only switches; `--quality`, `--depth-key-bits`, `--lod-error-pixels` and `--lod-splat-limit` set the view's `renderPolicy`, the rest apply before renderer creation:

| Switch | Effect |
|---|---|
| `--depth-key-bits 16` | Two radix passes; uint32 storage unchanged; ties may shimmer |
| `--budget 4000000` | LOD capacity, at most 4M; not guaranteed quality |
| `--quality ultra` | Starting level of the on-screen picker: `ultra`, `high` (default), `balanced`, `fast` |
| `--shot tour` | ISS camera, +Y up: `overview`, `orbit` (default), `detail`, `flyby`, `tour` |
| `--run-seconds 20` | Bounded run with resource monitoring |
| `--metalfx 0` | Upscales a `--scale` below 1 with the linear blit instead of MetalFX |

Defaults: 32-bit sorting, hardware raster.
Culling, LOD and depth quantization are approximations pending visual acceptance.
Private allocations still consume unified memory.
The dev memory guard cannot cancel in-flight work or prevent allocation spikes.

Build `splat_lod_build` from `splat-core/tools`; invoke `splat_lod_build input.spz output.lodsplat --sh 1`.
A cell splits until it holds at most 32 splats; `--depth` only caps the octree, at 21 subdivisions by default.
Output must be new.
Moment-matched parents remain approximate; original leaves survive.
Parents use [moment-matching initialization](https://arxiv.org/html/2406.12080v1#S4.SS1), without training/refinement.
v2 adds interior metadata and leaf packets; v1 remains readable.
Layout/validation: [LodFile.cpp](../splat-core/src/lod/LodFile.cpp).
Selection feeds visibility → radix → raster; denied refinements retain parents and report pressure.
The hierarchy stays resident, without Hi-Z or temporal transitions.

Stats: `loadedSplatCount` counts source splats; `drawnSplatCount` counts completed draw candidates.
Command timings overlap; HUD sort includes visibility/radix.
[Phone measurements](../../docs/BENCHMARKS.md) record settings, evidence and visual limitations.
