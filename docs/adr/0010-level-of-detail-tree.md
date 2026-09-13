# 0010. A level of detail tree, off by default

Status: accepted. Date: 2026-09-04.

## Context

Spark 2.0 (World Labs) renders scenes of 100M splats at a steady frame rate by drawing, per frame, a fixed budget of nodes from a hierarchy in which every interior node is one splat standing in for its children.
The engine draws the visible leaves of a scene; its frame time grows with what the view contains.

## Decision

`splat-core/lod` builds the hierarchy at load (`buildLodTree`, after Spark's tiny-lod: splats join a grid whose cell grows by 1.5 per level, cells merge into one node with the members' area times opacity as weight, opacity may exceed one and the shader draws min(1, alpha * falloff)) and selects nodes per frame (`selectLodNodes`: biggest on screen first through a bucket queue, weighted towards the view direction, until every node covers about a pixel or the budget is spent).
The sorter sorts only the selected nodes and culls as before; the selection is redone when the camera moves or turns ten degrees.
`splatBudget` on the view enables it, 0 (the default) draws every splat.

## Consequences

Measured on the house (2M splats, Mi 9, full resolution) the tree does not make the frame cheaper: nearly everything in view is larger than a pixel, and a budget below what the view needs blurs the image while the merged nodes cost the same fragments.
It is the mechanism for scenes larger than the view can hold at a pixel each and for low quality modes, not the default path.
The tree costs about 1.5 times the splats in GPU memory, seconds of CPU at load (4 s for 2M on a laptop) and 90 to 220 ms per selection on the phone, all of which are the next things to cut if it gets used.
