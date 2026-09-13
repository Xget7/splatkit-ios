# 0013. One engine, deep modules behind it

Status: accepted. Date: 2026-09-09.

## Context

`Engine.cpp` had grown to 650 lines with seven jobs in one class: the Vulkan surface, swapchain, offscreen target and pipeline lifecycle; decoding worlds and colliders and handing them across threads; mapping files; deciding when the sorter runs and how wide the cull margin is; the benchmark; publishing stats; and the frame itself.
None of it was testable without a device, including the parts that have no graphics in them.
The Kotlin side had the same shape: seven files in one package, three of them re-declaring the same twenty setters, and the float array layouts shared with C++ were known in three places.

## Decision

The engine keeps its interface and becomes the orchestrator of modules that each hide one job behind a small interface, in the sense of deep modules: a caller learns little and gets a lot, and a change to a job lands in one place.

Into `splat-core`, with unit tests, because they need no GPU:

- `MappedFile`: a file mapped read only, released with the object.
- `SplatWorldLoader`: decode, spatial reorder and the level of detail tree on the calling thread; a mailbox the render thread takes from; a `Result` back instead of a callback, so the core stays free of logging.
- `VisibilityPlanner`: sort on move, cull on turn ([ADR 0009](0009-sort-on-move-cull-on-turn.md)) and the margin that grows with the turn rate and the cull time, as a value that answers "request this frustum now, or nothing".
- `summarizeTimings`: mean, p50, p95 and max of a set of frame times.

Into `splatkit-android`:

- `VulkanSplatRenderer`: everything between an `ANativeWindow` and a presented frame, including the world bound to the pipelines and every rebuild a resize, rotation or setting change needs. A generation counter tells the engine when a drawn frame is gone.
- `Benchmark`: the capture, returning the yaw to turn each frame; it does not know the camera.
- `StatsPublisher`: the half second window, the atomics other threads read, and the log line.
- `SplatEngine`: owns the above plus the camera and the sorter, and runs the frame in about 270 lines.

Kotlin gets layers: `com.splatkit` is the public API, `com.splatkit.engine` the JNI boundary and the render thread, `com.splatkit.input` touch and the gyroscope.
`SplatEngine` decodes the stats and pose arrays itself, so the layout is one C++ file and one Kotlin file.
The JNI symbols carry the package, so the C++ entry points and the ProGuard rule moved with the class.

`clang-format` and `clang-tidy` from the pinned NDK lint every C++ file, tests and tools included, on every pull request; `scripts/lint-cpp.sh` is the one command.
Both packages are configured for the Android target when linting, so the NDK's `clang-tidy` parses them against the NDK's own libc++ whatever the host SDK ships.

## Consequences

The visibility policy and the loader are covered by tests that run on a laptop; the first version of the margin scaling shipped with a black edge on fast turns that a test now pins.
A world's positions move into the sorter instead of being copied.
The seams are real: the renderer, the loader and the planner each have one adapter today, and none was introduced for a hypothetical second one.
The clang-tidy configuration turns off the checks that would fight the code's deliberate style and says why; anything else that fires is a finding to fix, not to silence.
