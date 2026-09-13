# 0017. Visibility on the GPU where the renderer can

Status: accepted. Date: 2026-09-10.

With the whole Winter Garden at level 0 resident on the iPhone 17 Pro (ADR 0015 with a 16M residency), the CPU sort of the visible set took 39 ms a frame and its result arrived a few frames after the camera moved, while the GPU drew the frame in 67 ms.
We decided that a renderer may take over the cull and the sort: `SplatRenderer::sortsOnGpu()` tells the engine, which then hands it the slab ranges to draw in every frame instead of an order, and the renderer culls, sorts and draws them on the GPU in the same queue, with the count in an indirect draw.
The engine keeps the CPU sorter for renderers that cannot and for single file worlds with a level of detail tree, whose node selection is CPU work; the streamer pins the drawn tiles through `drawnNow` the way it pinned the tiles of a taken order, and no longer keeps positions for the CPU sorter in that mode.

The Metal implementation (`MetalVisibility`) is a least significant digit radix sort over the inverted bits of the squared distance, 4 bits per pass, 8 passes, each a block histogram, a per digit scan and a stable scatter with a fixed element order, verified against a stable CPU sort on the Mac; 5M keys take 7 ms on an M4 Pro.
Considered and rejected for the first version: onesweep with decoupled look-back (fewer passes, but forward progress between threadgroups is not something Metal promises and a wrong sort is worse than a slower one), 8 bit digits (half the passes, but the stable rank needs 256 counters per thread and does not fit threadgroup memory at 256 threads), and sorting on a different key such as view depth (turning would then re-sort; distance keeps the order valid under rotation, ADR 0007).
Since then the digits went to 8 bits and 4 passes, ranking by simdgroup with a ballot match over a simdgroup blocked layout instead of a counter per thread (5M keys in 3.8 ms on an M4 Pro, from 5.6), the kernel also projects and colours each survivor so the draw reads 32 bytes per quad, and the order became nearest first for ADR 0018.
