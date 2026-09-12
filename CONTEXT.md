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
What a host asks the engine to show: a source of splats plus, optionally, a collider. A world is one file today and a tileset tomorrow; the host does not know which.
_Avoid_: scene, model, asset, map

**Collider**:
The triangle mesh the walk camera collides with; the world's floor and walls for navigation only, never drawn.
_Avoid_: mesh, geometry, nav mesh

**Source**:
Where a world's bytes come from: a file, an asset, a content provider or a URL. Fetching a source puts its bytes on disk; it says nothing about how they are drawn.
_Avoid_: URI (that is the wire format of a source), download

### Drawing

**Frame**:
One presented image. The engine draws a frame only when something changed.

**Sort**:
Ordering the splats back to front from the camera, on the CPU, so blending is correct.
_Avoid_: depth sort, z-order

**Cull**:
Dropping the sorted splats outside the camera's frustum, widened by a margin, before drawing.

**Render scale**:
The size of the render target relative to the surface, 0.1 to 2. Below 1 the frame is upscaled, above 1 supersampled.
_Avoid_: resolution, resolution mode, quality (that is the preset)

**Preset**:
A named quality setting (low, medium, high, ultra) that fixes the render scale, the harmonics degree and the splat budget together.
_Avoid_: mode, profile, level

**Splat budget**:
Selection capacity; zero disables reduction.
_Avoid_: limit, cap, max splats

### Level of detail

**Node**:
One entry of the in-memory hierarchy over a cloud: a leaf is a splat of the file, an interior node is one splat standing in for its children.

**Tree**:
Offline/load-time hierarchy.
_Avoid_: LOD, octree (a tree of nodes is not an octree of tiles)

**Selection**:
Covering cut; capacity≠quality.

### Scale

**Tile**:
A cube of the world at one level, stored as its own spz file, with its splats in spatial order.
_Avoid_: chunk, cell, block, node (a node is inside a cloud, a tile is a cloud)

**Level**:
How coarse a tile is: level 0 is the file's splats, each level up stands in for the eight tiles below it with fewer, larger splats. Made offline, never on the phone.
_Avoid_: LOD, mip, layer

**Tileset**:
The index of a tiled world: every tile's bounds, level, file and children, plus the size of the smallest splat each stands in for.
_Avoid_: manifest, catalogue, tree

**Streaming**:
Loading and dropping tiles by what the camera can see while walking, so memory holds a neighbourhood, never the world.
_Avoid_: downloading (that is fetching a source), lazy loading

**Residency budget**:
The most tile bytes held in memory at once; what streaming evicts against.
_Avoid_: cache size, memory limit
