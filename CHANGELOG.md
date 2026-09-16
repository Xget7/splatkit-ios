# Changelog

Notable changes to the SplatKit iOS SDK and the shared C++ engine it ships.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); alphas may break APIs.

## Unreleased

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

[0.1.0-alpha.2]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/Xget7/splatkit-ios/releases/tag/v0.1.0-alpha.1
