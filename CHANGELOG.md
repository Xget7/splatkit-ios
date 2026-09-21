# Changelog

Notable changes to the SplatKit iOS SDK and the shared C++ engine it ships.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); alphas may break APIs.

## Unreleased

## [0.1.0-alpha.5] - 2026-09-21

The XCFramework carries the same engine as alpha.4; only the Swift sources and the bundled notices changed.

### Fixed

- The copyright holder in the XCFramework's `Notices/SplatKit.txt` reads Juan Ignacio Andrade.
- The notices name splat-transform (PlayCanvas), whose collision voxel passes `splat-core`'s collider builder ports, and carry its MIT licence.

### Removed

- `SplatMetalView.motionToggleEnabled` and the double tap that toggled the gyroscope.
  A host that wants the gesture adds its own recogniser and calls `setMotionEnabled`, which keeps the touches the view claims down to the single drag it documents.

## [0.1.0-alpha.4] - 2026-09-18

### Added

- `renderPolicy.raster` selects hybrid screen tiles per view, and `SKRenderPolicySupport.rasterMask` lists the strategies Metal builds: hardware and hybrid.
  Hybrid is an experimental opt-in for scenes where many large translucent splats overlap each pixel; hardware stays the default and is faster on distant or sparse scenes.
  The tile pipelines are built on the first request, and switching back to hardware frees the tile scratch.
- `renderPolicy.lodErrorPixels` applies live on Metal: a lower threshold refines a hierarchy world further, up to the budget it loaded with.
- Shared engine: `RenderPolicySupport::rasterMask`; a raster strategy outside it falls back with a warning.
- `renderPolicy.lodSplatLimit` caps the splats a hierarchy frame selects, live, below the loaded capacity.
  When a cut would exceed the limit or the loaded capacity, Metal raises the error threshold frame by frame until it fits, so detail thins evenly instead of stopping wherever traversal ran out of room.
- Stats measure frames the display showed: Metal reports each drawable's presented time, `fps` counts shown frames, and `presentTiming`, `frameMillisP95`, `lowFps` (1% low over 5 seconds) and `droppedFrames` join `SKSplatStats` and `SplatStats`.
  The periodic log line appends the same fields.
- `SplatMetalView.walk(forward:right:)`, `look(deltaYaw:deltaPitch:)`, `setCharacter(_:)`, `character` and the `CharacterSettings` type, so the host drives walking and shapes the walker.
- `SplatMetalView.cameraPoseInterval` and the `splatView(_:cameraPoseChanged:)` delegate method, which report where the camera ended up at most that often and only while it moves.

### Changed

- A hierarchy world's LOD capacity reaches 4M selected splats, up from 2.2M; `maxLodCapacitySplats` reports it.
- Walk mode refuses steps onto a floor more than 0.35 m higher, looking 0.25 m ahead, so it climbs stairs and steps over door tracks but no longer climbs counters, chairs or tables whose top the hip probe passes over, and slides along them when walked into at an angle.

### Removed

- The `SPLATKIT_METAL_TILE_RASTER` and `SPLATKIT_METAL_LOD_QUALITY_PIXELS` environment variables; set `renderPolicy.raster` and `renderPolicy.lodErrorPixels` instead.
- `SplatMetalView.walkSensitivity` and the two-finger walk gesture it configured.
  The view now handles one drag to look and nothing else, so the host's own controls keep every touch the look drag does not.

## [0.1.0-alpha.3] - 2026-09-16

### Added

- `SplatMetalView.renderPolicy` and `deviceCapabilities`, with `SKRenderPolicy`, `SKDeviceCapabilities` and `-[SKSplatEngine applyRenderPolicy:reason:warnings:]`.
  Each request is re-validated on the render thread; Metal applies the sort depth with GPU sort and the sub-pixel threshold under tight culling, and every other field falls back with a warning.
  An invalid request or a preparation failure keeps the previous policy.
- Shared engine: `resolveRenderPolicy`, `SplatEngine::setRenderPolicy` and a capability query on `SplatRenderer`.
- Benchmarks log 30-second windows and final p99 frame and GPU times; `TimingSummary` reports p99.

### Changed

- Benchmarks reject durations outside `(0, 3600]` seconds and treat a zero GPU time as unavailable, not as a free frame.

### Removed

- The `SPLATKIT_METAL_MIN_PIXEL_RADIUS` and `SPLATKIT_METAL_DEPTH_KEY_BITS` environment variables; set `renderPolicy` instead.

## [0.1.0-alpha.2] - 2026-09-12

### Fixed

- GCC portability of the offline LOD writer and portable residency test budgets.
- Hosts without Metal raster support report skipped tests instead of failures.

## [0.1.0-alpha.1] - 2026-09-12

### Added

- Native Metal SDK for iOS 17 and A14/M1 or newer, distributed as a SwiftPM device and simulator XCFramework.
- GPU visibility and radix sorting, experimental LOD and hybrid screen tiles, asynchronous loading and first-frame readiness.

[0.1.0-alpha.5]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.5
[0.1.0-alpha.4]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.4
[0.1.0-alpha.3]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.3
[0.1.0-alpha.2]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.1
