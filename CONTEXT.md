# SplatKit

An engine that walks Gaussian splat worlds on phones: a shared C++ core (formats, sorting, navigation, level of detail) and one renderer per platform.
This glossary is the vocabulary of the code, the docs and the conversation; it names concepts, not implementations.

## Language

### The world

**Splat**:
One Gaussian: a position, a covariance, a colour and an opacity.
_Avoid_: point, gaussian, particle

**Cloud**:
The splats of a world decoded into memory, structure of arrays, in the engine's own frame.
_Avoid_: point cloud, dataset, buffer

**World**:
What a host asks the engine to show: a source of splats plus, optionally, a collider.
A world is one file or a tileset streamed in tiles.
_Avoid_: scene, model, asset, map

**Collider**:
The triangle mesh the walk camera collides with; the world's floor and walls for navigation only, never drawn.
_Avoid_: mesh, geometry, nav mesh

**Source**:
Where a world's bytes come from: a file, an asset, a content provider or a URL.
Fetching a source puts its bytes on disk; it says nothing about how they are drawn.
_Avoid_: URI (that is the wire format of a source), download

### Drawing

**Frame**:
One presented image.
The engine draws a frame only when something changed.

**Anchor**:
A point in the world that an orbit camera keeps centred.
A picked anchor is a collider ray hit; without a collider the default is the centre of the world's bounds.

**Orbit**:
Camera movement around an anchor by azimuth, elevation and radius.
Orbit is fly mode; first-person input resumes walking from the same pose.

**Sort**:
Ordering splats by camera depth in the direction required by compositing.
_Avoid_: depth sort, z-order

**Cull**:
Rejecting splats outside the view or below the configured visibility threshold.

**Render scale**:
The size of the render target relative to the surface, 0.1 to 2.
Below 1 the frame is upscaled, above 1 supersampled.
_Avoid_: resolution, resolution mode, quality (that is the preset)

**Preset**:
A named set of starting values for the render scale, the harmonics degree and the budgets: `RenderQuality` on Android, the builder presets in React Native.
Individual settings override it.
_Avoid_: mode, profile, level

**Splat budget**:
Selection capacity; zero disables reduction.
_Avoid_: limit, cap, max splats

**Render policy**:
Per-view renderer choices, such as sort depth and the sub-pixel threshold, resolved against the device capabilities.
A supported choice applies, an unsupported one falls back with a warning, and an invalid request is rejected while the previous policy stays.
_Avoid_: settings, config, quality (that is the preset)

**Capabilities**:
What one backend on one device can apply from a render policy, and the limits it accepts.

### Level of detail

**Node**:
One entry of the hierarchy over a cloud: a leaf is a splat of the file, an interior node is one splat standing in for its children.

**Tree**:
The hierarchy of nodes, built at load time or read from an offline `.lodsplat` file.
_Avoid_: LOD, octree (a tree of nodes is not an octree of tiles)

**Selection**:
The nodes drawn this frame: a cut that covers the world within the splat budget.
A larger budget is not a quality guarantee.

### Scale

**Tile**:
A cube of the world at one level, stored as its own spz file, with its splats in spatial order.
_Avoid_: chunk, cell, block, node (a node is inside a cloud, a tile is a cloud)

**Screen tile**:
A rectangular group of image pixels composited together; unrelated to a world's streaming tiles.

**Level**:
How coarse a tile is: level 0 is the file's splats, each level up stands in for the eight tiles below it with fewer, larger splats.
Made offline, never on the phone.
_Avoid_: LOD, mip, layer

**Tileset**:
The index of a tiled world: every tile's bounds, level, file and children, plus the size of the smallest splat each stands in for.
_Avoid_: manifest, catalogue, tree

**Streaming**:
Loading and dropping tiles by what the camera can see while walking, so memory holds a neighbourhood, never the world.
_Avoid_: downloading (that is fetching a source), lazy loading

**Residency budget**:
The most splats held resident at once; what streaming evicts against.
_Avoid_: cache size, memory limit
