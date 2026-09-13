# 0003. Shared C++ core for Formats and Navigation

Status: accepted. Date: 2026-09-03.

## Context

Both platforms need to decode `.spz` and `.glb` and to raycast a collider mesh.
Two implementations (Swift and Kotlin) would diverge.
`nianticlabs/spz` is the reference decoder, has a CI-tested NDK build, and reads SPZ v1 to v4.

## Decision

`packages/splat-core` is a C++17 library with no graphics or platform dependency.
It owns the types `SplatCloud` and `TriangleMesh`, the SPZ and GLB decoders, and the collider grid and raycast.
It converts input data into the internal frame once, at decode time.
Rendering, camera, input, scene and view stay native per platform.

## Consequences

Android calls the core through JNI; iOS through a C++ interop target.
The core is developed and tested on the desktop with `ctest`; no device is needed for it.
