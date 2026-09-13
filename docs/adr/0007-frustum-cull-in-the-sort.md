# 0007. Cull against the frustum inside the sort

Status: superseded by [0009](0009-sort-on-move-cull-on-turn.md). Date: 2026-09-04. Refines [0004](0004-cpu-sort-baseline.md).

## Context

0004 chose a distance sort so that turning never triggers a sort, only translation does.
On the 2M splat house the GPU cost was bound per splat it processed, whatever the resolution: the vertex shader culled what was outside the view, but only after fetching and binning it.
The sorter already visits every splat, so testing each against the view volume there is nearly free.

## Decision

`DistanceSorter::sortVisible` keeps only splats inside a frustum 1.5 times wider than the view and returns their count; the draw uses that count.
The engine re-sorts when the camera moves or turns more than 5 degrees, so turning now costs a sort, but a smaller one.

## Consequences

The house went from 45 to 34 ms p50 at full resolution and to 60 fps at render scale 0.5; the sort went from 44 to 16 ms.
Turning faster than the margin covers within one sort shows unsorted or missing splats at the edge for a frame or two; the margin and the threshold are constants in `Engine.cpp` and should become per device tuning if it shows.
A fresh world draws nothing until its first sort arrives, one or two frames after upload.
