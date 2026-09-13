# 0018. Front to back on the GPU order path

Status: accepted. Date: 2026-09-10.

With the whole International Space Station scene resident on the iPhone 17 Pro (10M splats, ADR 0017), the frame cost was in the fragments: 9.7M quads drawn, hundreds of layers per pixel, 117 ms of GPU time after the projection had moved out of the vertex stage.
Back to front "over" blending cannot stop early: a pixel already opaque keeps shading every splat behind it.

We decided that the GPU order path draws front to back, the way the reference rasterizer of Kerbl et al. does, and stops shading a pixel once its coverage is complete.
The sort key is the squared distance itself, nearest first; the fragment writes premultiplied colour with "under" blending onto a clear of zero, and the background goes under whatever coverage is left at the end.
The draw goes in seven batches of the sorted order, 1/64 of the splats first and doubling, and between batches a full screen pass reads the coverage in tile memory and writes a depth in front of every splat where it has reached 254/255, so the batches after it fail the depth test there before the fragment shader runs.
The depth buffer is memoryless and the coverage accumulates in half floats in an offscreen target that a blit copies to the drawable, because 8 bit coverage rounds small contributions away and the weighting of everything behind depends on it.

The CPU order path (single file worlds with a level of detail tree, and any renderer without a GPU sort) keeps back to front and "over" blending; the two never mix in one frame.
Consequences: the cut at 254/255 is the same threshold the reference rasterizer uses, so the image is the one back to front would give up to rounding; the picture costs what the visible layers cost rather than what every splat costs; an extra offscreen target and a blit per frame; and a scene that is mostly transparent gains nothing from the mask.
Considered and rejected: a per tile compute rasterizer (the paper's), which also sorts per tile and stops per pixel, but is a second renderer to keep correct and can come later if the hardware path is not enough; and a single batch with no mask, which is the previous cost.
