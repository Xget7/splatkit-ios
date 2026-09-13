# Validation

No publication until artifact integration, matched-image review and sustained physical-device runs pass.
Mac Metal execution and Android cross-compilation do not establish phone performance.

## Local checks

```sh
python3 scripts/sdk_harness.py check engine
python3 scripts/sdk_harness.py check metal
python3 scripts/sdk_harness.py check android
python3 -m pip install -r scripts/requirements-validation.txt
python3 -m unittest discover -s scripts/tests
```

Capture comparisons need Python 3.10+ and Pillow; their tests explicitly skip if Pillow is absent.
Set `SPLAT_FIXTURES_DIR` for external core fixtures; report skips, never count them as validation.

## Matched images

```sh
python3 scripts/compare_captures.py reference.json lod.json
```

Each manifest records `image` (relative path), `image_sha256`, original `world_sha256`, `environment`, `camera` and `settings`.
`camera` contains column-major `view`/`projection` matrices (16 numbers each) and integer `viewport: [width, height]`.
`settings` includes `render_scale`, `sh_degree`, `linear_blending`, `splat_budget` and every other active rendering override.
Only `splat_budget` may differ by default; list other deliberate differences with `--vary`.
Use HUD-free captures; `--roi X Y WIDTH HEIGHT` explicitly restricts measurement to a matching region.
The tool rejects mismatched cameras, dimensions, hashes and colour tags; it never aligns or resizes images.
It reports RGB MAE, PSNR, p99 error and worst 32-pixel region; `--max-mae` is an explicit numeric gate, not perceptual acceptance.
Review silhouettes, thin geometry, transparency and temporal LOD transitions separately.

The Mac integration test `MetalLODTest.OfflineFixtureRendersWithBoundedDrawCount` accepts `SPLAT_LOD_PATH`, optional `SPLAT_LOD_REFERENCE_SPZ`, `SPLAT_LOD_BUDGET`, `SPLAT_LOD_POSE=x,y,z,yaw,pitch` and `SPLAT_LOD_CAPTURE`.
Its `[ LOD CAMERA ]` line records the actual capture matrices; default framing remains the historical ISS view.
An offline hierarchy is not interchangeable evidence for the default load-time hierarchy.

## Sustained runs

```sh
python3 scripts/benchmark_report.py capture.log --environment physical --pid 123 --minimum-seconds 600
```

The shared benchmark accepts finite durations in `(0, 3600]`, preserves wall-time stalls, emits 30-second windows and final p99.
It still makes one full turn per run: a 600-second turn is not the same trajectory speed as a 30-second turn.
For constant-speed thermal comparisons, repeat identical 30-second turns for 10-15 minutes and keep every run plus the continuous thermal/memory trace; use `--minimum-seconds 30` per run.
Pin world/hierarchy hashes, build, driver, pose, viewport, SH, depth mode and quality settings; warm up before capturing, record charging state and lifecycle interruptions.
Sample PSS/RSS and thermal status independently; stop the device test on severe thermal status, repeated GPU failures or an unresponsive UI.
GPU timings are latest-completed-query samples, may repeat and are not guaranteed to match the current host frame; zero/unavailable is not zero cost.
Historical logs cannot supply missing p99 or memory/thermal evidence.
Host callbacks may remain display-paced even when presentation vsync is disabled.

## Remaining parity gates

| Contract | Metal / iOS | Vulkan / Android |
|---|---|---|
| Native LOD, visibility, radix, indirect draw | Implemented | Implemented |
| Source and completed draw counts | Public stats | Public stats |
| First successful GPU world-frame callback | Public delegate | Missing |
| Scripted look-at with explicit up vector | Public API | Shared engine only |
| PNG capture | Public API | No SDK capture API |
| Quality presets | Individual properties | `RenderQuality` presets |
| Experimental hybrid screen tiles | Implemented | Missing |
| GPU sort timing | Visibility + radix command interval | Isolated radix interval |

Shared functionality does not imply identical internal algorithms or timing semantics.
Tile parity, missing host contracts and consistent stage diagnostics remain open; no equivalent-feature or universal 30/60 FPS claim.
