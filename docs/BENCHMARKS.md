# Phone benchmarks

Historical measurements on physical phones, for the builds and settings listed below.
Short runs do not establish sustained performance; hour-long thermal behaviour remains unmeasured.
See [validation](VALIDATION.md) for reproduction and acceptance checks.

## iPhone 17 Pro: Metal

Physical iPhone 17 Pro, A19 Pro; ISS experiments ran September 11-16, 2026.
Settings differ between runs; exact configurations and limitations are in the linked JSON.
ISS image quality remains unapproved.

| ISS run | Mean FPS | Evidence |
|---|---:|---|
| Original hybrid, partial run | 11.06 | [JSON](benchmarks/2026-09-11-iss-metal-512.json) |
| Initial LOD, 1.2M capacity; quality rejected | 35.75 | [JSON](benchmarks/2026-09-12-iss-metal-lod-phone.json) |
| Guarded LOD, 2.2M capacity; full orbit | 21.63 | [JSON](benchmarks/2026-09-12-iss-metal-lod-guarded.json) |
| LOD hardware / hybrid | 23.11 / 18.92 | [JSON](benchmarks/2026-09-12-iss-lod-hybrid.json) |
| 16-bit radix | 23.36 | [JSON](benchmarks/2026-09-12-iss-radix16.json) |
| Per-view policy, 16-bit; two 30-second runs | 22.29 / 23.32 | [JSON](benchmarks/2026-09-16-iss-render-policy-phone.json) |
| Per-view policy, 32-bit; 30 seconds | 19.18 | [JSON](benchmarks/2026-09-16-iss-render-policy-phone.json) |

These are periodic stats samples, not all-frame statistics.
Metal sort intervals include visibility and radix; they are not isolated sort timings.
The guarded run observed a ~3.54 GB process limit, not a device memory guarantee.
[Startup readiness](benchmarks/2026-09-12-iss-prepared-start.json) means GPU completion, not visual acceptance.

Lioness: 1.86M splats, iOS 27.0, SH3, render scale 1.0, 30-second engine orbit at 1.41 m radius.

| Date | Mean FPS | Mean GPU ms | Evidence |
|---|---:|---:|---|
| 2026-09-22 | 37.57 | 25.44 | [JSON](benchmarks/2026-09-22-lioness-ios-orbit.json), [log](benchmarks/2026-09-22-lioness-ios-orbit.log), [capture](benchmarks/2026-09-22-lioness-ios-orbit.png) |
| 2026-09-24, committed-engine rerun | 37.62 | 25.41 | [log](benchmarks/2026-09-24-lioness-ios-orbit-rerun.log) |

Lioness by Spenser Dickerson: [CC BY 4.0](https://superspl.at/scene/7e4e9bcb).

## Xiaomi Mi 9: Vulkan GPU pipeline

Physical Mi 9, Adreno 640, driver `0x801f6000`; minified Release, validation off, SH0, 1080x2261, 30-second turns.
LOD, visibility, radix and indirect draw run on the GPU.

| Date | World / settings | Mean FPS | Mean GPU ms | Evidence |
|---|---|---:|---:|---|
| 2026-09-12 | House 2M, full source | 19.2 | 51.9 | [JSON](benchmarks/2026-09-12-mi9-vulkan.json) |
| 2026-09-12 | House, load-time LOD, 1.2M capacity | 24.0 | 41.6 | [JSON](benchmarks/2026-09-12-mi9-vulkan.json) |
| 2026-09-12 | Same LOD, empty groups skipped | 41.4 | 23.9 | [JSON](benchmarks/2026-09-12-mi9-vulkan.json) |
| 2026-09-14 | House, load-time LOD, 1.2M capacity | 39.2 | 24.9 | [JSON](benchmarks/2026-09-14-mi9-sdk-smoke.json) |
| 2026-09-14 | Kitchen 500k, no LOD | 42.6 | 22.6 | [JSON](benchmarks/2026-09-14-mi9-sdk-smoke.json) |

Timings and drawn counts are periodic samples.
USB disconnected before the September 14 lifecycle cycles; sustained thermal and memory checks remain pending.

## Mi 9: earlier CPU ordering pipeline

September 4-9, 2026: Android 11, Vulkan 1.1.128, Release, portrait 1080x2261 unless stated otherwise.
GPU timings use timestamp queries; phone cooled below 48 C before runs, below 40 C for the September 9 harmonics tests.
These measurements precede the GPU pipeline above.
Same-build variation reached ~2 ms between days; small changes require an A/B in the same session.

### Presets and larger worlds

Historical presets: HIGH scale 1.0/SH3; MEDIUM 0.7/SH1; LOW 0.5/SH0 with a 500k budget; ULTRA 1.5/SH3.
House HIGH/MEDIUM/LOW used a 10-degree cull margin; ULTRA used 20 degrees.
House presets: September 4; Pool and Bicycle: September 8, commits `9732897` and `11daaf7`.

| World | Preset / change | GPU ms mean / p50 / p95 |
|---|---|---|
| House 2M | HIGH | 19.8 / 19.3 / 26.2 |
| House 2M | MEDIUM | 13.4 / 13.4 / 17.5 |
| House 2M | LOW | 12.1 / 12.4 / 14.7 |
| House 2M | ULTRA | 39.4 / 39.1 / 50.0 |
| Pool 3.57M | HIGH | 73.5 / 65.8 / 123.3 |
| Pool 3.57M | MEDIUM | 41.4 / 36.6 / 63.3 |
| Pool 3.57M | LOW | 20.2 / 19.2 / 32.5 |
| Pool 1.79M | Every second splat, MEDIUM | 20.8 / 18.1 / 31.1 |
| Bicycle 2.6M | HIGH | 19.4 / 13.7 / 45.3 |

House MEDIUM reached 60 FPS; ULTRA reached ~25 FPS and ended at GPU 61 C.
Pool HIGH reached ~14 FPS; halving the splats with MEDIUM reached ~55 FPS.
Removing its 342 largest splats did not improve p50: 65.8 ms either way.
Budgeted LOD can blur nearby detail; the house's 500k-budget test was rejected for quality.

Pool: Apartment Pool by paul/nesterdigital, superspl.at, CC BY; SH0, no collider, pose (-0.3, 2.3, 4.0).
Bicycle: Mip-NeRF 360 by Seeget3D, superspl.at, CC BY; SH3, no collider, pose (0, 0.7, 1.6), pitch -0.2.

### Optimisations and rejected experiments

Earlier house 2M measurements, GPU milliseconds.

| Change | Render scale | Mean / p50 / p95 |
|---|---:|---|
| As decoded | 1.0 | 144 / 121 / 279 |
| Morton spatial order | 1.0 | 48.4 / 44.9 / 77.1 |
| Frustum culling | 1.0 | 34.7 / 34.2 / 43.2 |
| Same culling | 0.7 | 18.1 / 17.5 / 24.4 |
| Encoded-space blending | 1.0 | 19.8 / 19.4 / 26.3 |
| Same blending | 0.7 | 13.7 / 13.6 / 17.8 |

Spatial ordering improved fetch locality; kitchen was already coherent.
Reordering 2M splats took 470 ms on the phone's loader thread.
CPU sort took 41-46 ms on movement; turning only culled, ~10 ms for 2M splats on four threads.
The cull margin accounts for turn rate and measured latency; a fixed margin left empty edges during fast turns.
The 32-byte splat record saved 4-6 ms per frame; kitchen p50 fell from 27-29 ms to 23 ms before the blending change.

House experiments against a 44.9 ms p50 baseline:

| Rejected change | GPU p50 ms |
|---|---:|
| One triangle per splat | 48.1 |
| Triangle only below 1.5 px | 48.9 |
| Draw in 256k-instance chunks | 181 |

A compute projection prepass was 4 ms slower and was removed.
Gathering into draw order showed no improvement: 46.3 vs 46.9 ms in its A/B.
CPU priority/affinity changes showed no gain: house GPU p50 31.9 vs 31.8 ms.
With culling, octagons worsened house p50 (34.2 to 38.9 ms) but improved kitchen (21.8 to 19.4 ms).
These results apply to this phone and workload.

### Spherical harmonics and image quality

Raccoon 932k: SH3 vs SH0 p50 was 12.5 vs 12.4 ms, no measurable cost in that test.
Bicycle 2.6M, September 9: fresh-load SH3/SH0 means were 18.9/17.2 ms portrait and 40.9/28.5 ms landscape (2265x1080).
Landscape includes more canopy; harmonics cost depends on the view.
Changing the degree per frame matched the fixed-degree build's timing and avoided decoding the world again.

Pool image comparison, September 8: Mi 9 HIGH, 1080x2261, vertical FOV 65 degrees, pose (-0.3, 2.3, 4.0), yaw -0.75, pitch -0.15.
PlayCanvas reference used the same pose, FOV and resolution, sRGB, no tone mapping or antialiasing.
Sharpness is Laplacian variance over a 1080x1200 crop, not perceptual acceptance.

| Render / data | Sharpness |
|---|---:|
| PlayCanvas / original PLY | 71.9 |
| PlayCanvas / half-float covariance | 71.7 |
| PlayCanvas / phone SPZ converted to PLY | 69.3 |
| Mi 9 HIGH / same SPZ | 53.0 |
| Mi 9 ULTRA / same SPZ | 49.7 |
| Mi 9 MEDIUM / same SPZ | 31.6 |

Half-float covariance and radial ordering showed negligible differences in this comparison.
Residual edge softness remains unresolved; frame settings must match before comparing against a web viewer.
