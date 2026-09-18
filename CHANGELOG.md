# Changelog

Notable changes to the SplatKit iOS SDK and the shared C++ engine it ships.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); alphas may break APIs.

## Unreleased

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

### Changed

- A hierarchy world's LOD capacity reaches 4M selected splats, up from 2.2M; `maxLodCapacitySplats` reports it.
- Walk mode refuses steps onto a floor more than 0.35 m higher, looking 0.25 m ahead, so it climbs stairs and steps over door tracks but no longer climbs counters, chairs or tables whose top the hip probe passes over, and slides along them when walked into at an angle.

### Removed

- The `SPLATKIT_METAL_TILE_RASTER` and `SPLATKIT_METAL_LOD_QUALITY_PIXELS` environment variables; set `renderPolicy.raster` and `renderPolicy.lodErrorPixels` instead.

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

[0.1.0-alpha.3]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.3
[0.1.0-alpha.2]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.1
