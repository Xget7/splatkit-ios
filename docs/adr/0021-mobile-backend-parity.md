# 0021 - Mobile backend parity

Decision: share host contracts and LOD data through [SplatRenderer](../../packages/splatkit-engine/include/splatkit/rendering/SplatRenderer.h), keeping GPU implementations native.

Both backends integrate GPU LOD → visibility/compaction → radix → indirect drawing.
Metal additionally offers experimental hybrid screen tiles; Vulkan does not.
[Vulkan contracts and evidence](../../packages/splatkit-android/docs/VULKAN.md) distinguish implemented functionality from device acceptance.

Gate [subgroups](https://docs.vulkan.org/guide/latest/subgroups.html), memory and dispatch limits at runtime. Retain bounded CPU fallback, never unsafe raw multi-million-splat draws.
World streaming tiles and screen raster tiles are separate concepts.

Tradeoff: shared moment-matched parents and covering cuts preserve coverage, not exact images.
Quantized depth, subpixel rejection and transmittance termination are approximations.
Transparent splats are not reliable opaque Hi-Z occluders.
[Optimized hierarchies](https://repo-sam.inria.fr/fungraph/hierarchical-3d-gaussians/) and [Mobile-GS](https://xiaobiaodu.github.io/mobile-gs-project/) require further representation/quality work.

Merge gate: builds, tests, lint and validation-clean integration evidence via the [harness](../AGENT_HARNESS.md).
Production quality/performance acceptance additionally requires reference images, lifecycle stress and sustained physical-device timings with world/settings/driver provenance.
The emulator verifies functionality, not Android speed. No lossless, universal 30/60 FPS or state-of-art claim follows.
