# 0009. Sort on move, cull on turn

Status: accepted. Date: 2026-09-04. Supersedes [0007](0007-frustum-cull-in-the-sort.md).

## Context

0007 sorted only the splats inside a widened frustum, so every turn past 5 degrees cost a sort (10 to 95 ms depending on how much was visible) and the fixed margin of 1.5 times the field of view covered about 7 degrees horizontally.
A normal turn of the phone covers more than that within one sort, so the edges of the view were visibly empty for a few frames.
An optimisation has to give the same image for less work, and this one gave a worse image.

## Decision

The distance order is rotation invariant, which is what 0004 chose it for, so the sorter thread keeps the full order and sorts only when the camera position changed.
Turning runs `DistanceSorter::cull`: a filter of that order through the frustum, in two streaming passes (frustum test in index order into a bitmap, then compaction of the order through the bitmap) on four threads.
The margin is angular, 10 degrees plus the turn rate times 50 milliseconds, and a one degree turn asks for a new cull.

## Consequences

Turning costs 10 ms of background work for 2M splats instead of a sort, and the GPU time did not change (31.8 against 31.2 ms p50 on the house).
Walking costs the full sort again (44 ms for 2M) rather than the sort of the visible subset; distance order changes slowly at walking speed, which is the trade 0004 made.
The cull is still a gather per visible splat; a thread pool instead of per call threads and a SIMD frustum test are the next steps if it shows.
