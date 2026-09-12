# SplatKit iOS

Native Metal Gaussian splatting for iOS 17+, A14/M1+ GPUs: asynchronous loading, GPU visibility/radix sorting, SH0–3, walk/fly input and diagnostics.
MIT-licensed sources include the shared C++ engine/core.

## Install

Add `https://github.com/Xget7/splatkit-ios` in Xcode Package Dependencies and select product `SplatKit`.
Choose exact version `0.1.0-alpha.2`.
The package downloads the release XCFramework for arm64 devices and arm64/x86_64 simulators.

```swift
import SplatKit

let view = SplatMetalView()
view.loadWorld(file: worldURL)
view.resume()
```

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
RN GPU controls remain pending.
