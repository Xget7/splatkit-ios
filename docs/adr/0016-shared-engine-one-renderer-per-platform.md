# 0016. One engine shared by the platforms, one renderer each

Status: accepted. Date: 2026-09-10.

The iPhone 17 Pro is the device the Winter Garden (15M splats) has to run on, and the engine that streams it (ADR 0015) lived inside the Android package next to Vulkan, so an iOS engine would have meant a second copy of the frame, the camera, the diagnostics and the GPU record packing.
We decided that everything of the engine that has no graphics API in it moves to `packages/splatkit-engine`, tested on the desktop like the core, and that the engine talks to the platform through one small `SplatRenderer` interface: upload a world or a slab, upload a tile into a range, draw a frame from an order.
`splatkit-android` keeps only Vulkan, the window and the JNI; `splatkit-ios` gets Metal, the layer and the Swift view, written natively rather than through MoltenVK or WebGPU, so that Apple's tile shaders and a GPU sort stay reachable and no translation layer sits in every host app.

Considered and rejected: MoltenVK (one renderer for both, but a 10 MB dependency in every app, two layers to debug and no access to what the Apple GPU has beyond Vulkan), wgpu or Dawn (one API for both, but a Rust or Chromium toolchain for React Native contributors, ADR 0002), and keeping the engine per platform (the streaming policy would have diverged within weeks).
