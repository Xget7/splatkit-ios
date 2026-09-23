# SplatKit iOS

Native Metal Gaussian splatting for iOS 17+, A14/M1+ GPUs: asynchronous loading, GPU visibility/radix sorting, SH0–3, walk/fly input and diagnostics.
MIT-licensed sources include the shared C++ engine/core.
Run it on a physical device: the iOS Simulator does not report GPU family 7, so `SplatMetalView.isAvailable` is false and the view renders black.

[![SwiftPM](https://img.shields.io/github/v/release/Xget7/splatkit-ios?include_prereleases&label=SwiftPM)](https://github.com/Xget7/splatkit-ios/releases)
[![license: MIT](https://img.shields.io/github/license/Xget7/splatkit-ios)](LICENSE)
[![platform: iOS 17+](https://img.shields.io/badge/platform-iOS%2017%2B-lightgrey.svg)](#install)

## Install

Add `https://github.com/Xget7/splatkit-ios` in Xcode Package Dependencies and select product `SplatKit`.
Choose exact version `0.1.0-alpha.5`; `main` may be ahead of that release, as the [changelog](CHANGELOG.md) lists.
The package downloads the release XCFramework for arm64 devices and arm64/x86_64 simulators.

```swift
import SplatKit

let view = SplatMetalView()
view.loadWorld(file: worldURL)
view.resume()
```

`loadWorld` reads `.spz` and `.lodsplat`; `scripts/prepare-world.sh scene.ply out/ --collider` turns any Gaussian splat PLY into a world and a walk collider, and `--lod` adds an offline tree for scenes of several million splats.
Attach the view to your hierarchy; forward `resume()`, `pause()` and `release()` from lifecycle events.
Start motion after `splatView(_:worldFrameReady:)`.
[API and experiments](packages/splatkit-ios/README.md).

## Verify

```sh
python3 scripts/sdk_harness.py check engine
python3 scripts/sdk_harness.py check metal
bash scripts/package-ios.sh
```

Requires Xcode, CMake and Python 3.9+; builds fetch pinned dependencies.
No signing credentials or scene downloads are needed for synthetic native tests.
LOD, hybrid tiles and 16-bit sorting remain experimental; quality acceptance and sustained 30/60 FPS are not guaranteed.
Simulator/Mac checks are not phone benchmarks.
[react-native-splatkit](https://github.com/Xget7/react-native-splatkit) drives `renderPolicy` from its `policy` prop; iOS device validation is pending.
