# 0012. Quality presets over individual settings

Status: accepted. Date: 2026-09-04.

## Context

The engine has five levers that trade frame time for image: render scale, spherical harmonics degree, level of detail budget, blend space and cull margin.
A host has to know what each one does, and what it costs on a phone it has never seen, to pick them; most hosts want "make it run well here" and a way to depart from that.
Every lever was measured on the Mi 9 before this decision (`docs/BENCHMARKS.md`), so a sensible combination per tier is a matter of record, not taste.

## Decision

`RenderQuality` is a value with the five settings, and `SplatSurfaceView.applyQuality` sets them at once.
Four presets are published, each with a stated reason and a benchmark row: `LOW`, `MEDIUM`, `HIGH` (the default) and `ULTRA`.
A preset is a starting point, never a mode: every setting stays a property on the view, a `copy` of a preset changes one value, and applying a preset then setting a property is the supported way to customise.
Render scale opens above one for `ULTRA`: the offscreen target is bigger than the surface and the existing linear blit downscales it, which supersamples the thin splats that shimmer at a pixel each.
The cull margin becomes a setting so that `ULTRA` can trade off screen draws for a margin no flick can outrun.
The harmonics degree in a preset is what is drawn, decided per frame from the harmonics the world carries, so a preset change never needs a reload; the memory cap on what is uploaded (`maxShDegree`) stays a separate setting the presets do not touch.
The engine does not pick a preset from the device on its own: a wrong guess is worse than a documented default, and the host knows its frame budget.

## Consequences

Numbers on the Mi 9 with the 2M splat house, GPU milliseconds p50 (`docs/BENCHMARKS.md` has the runs):

| Preset | GPU ms p50 | Note |
|---|---|---|
| `LOW` | 12.4 | 13.1 without the budget on this scene: the floor here is per splat work, the budget is for scenes bigger than this one |
| `MEDIUM` | 13.4 | 60 fps |
| `HIGH` | 19.3 | the default |
| `ULTRA` | 39.1 | 25 fps on the Mi 9, 1620x3391 target: a flagship or a still |

A thermal step down (drop a preset at `THERMAL_STATUS_SEVERE`) is the natural next layer over this and is listed in the roadmap.
