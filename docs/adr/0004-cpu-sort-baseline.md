# 0004. CPU background sort as the baseline

Status: accepted. Date: 2026-09-03.

## Context

Alpha blending is order dependent, so splats are drawn back to front and must be sorted per view.
Every shipping mobile viewer surveyed sorts on the CPU in a background thread.
No Vulkan radix sort library declares mobile support; Unity's Android GPU sort has been broken for two years on Adreno.

## Decision

Sort on a background CPU thread, reusing the last order until a new one arrives.
The sorter sits behind an interface so a GPU sorter can be added per device family later.

## Consequences

Visible popping when the camera turns fast; measured and documented per device.
Sort time bounds the usable splat count on slower CPUs and drives the device tier table.
