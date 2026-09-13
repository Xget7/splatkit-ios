# 0006. Reorder splats spatially at load

Status: accepted. Date: 2026-09-04.

## Context

The vertex shader fetches each splat record through the sort order, four times per splat.
On the 2M splat World Labs house a fetch only shader took 160 ms per frame on a Xiaomi Mi 9: the gather is bound by memory latency, not bandwidth, because consecutive entries of the distance order pointed at unrelated memory.
The 500k kitchen did not show it, since its file already came in a coherent order.

## Decision

After decoding, `splat::reorderSpatially` permutes every attribute array along a Morton curve over the cloud bounds, with a radix sort on the 30 bit code.
The renderer never depends on the order the file came in.

## Consequences

The house went from 121 to 45 ms p50 at full resolution; the kitchen did not change.
The reorder runs on the loader thread and costs 66 ms for 2M splats on an M4 Pro; the phone number is in the roadmap.
Formats that are already spatially ordered, such as SOG, pay the reorder for nothing; it stays because it is cheap and makes the renderer independent of the exporter.
