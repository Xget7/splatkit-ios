# 0005. Internal coordinate frame is RUB

Status: accepted. Date: 2026-09-03.

## Context

World Labs exports in the OpenCV frame: +X right, +Y down, +Z forward (RDF).
glTF and three.js use +X right, +Y up, +Z back (RUB).
Metal, Vulkan and every camera convention we use look down -Z with +Y up.

## Decision

The core converts every input to RUB at decode time.
The caller declares the input frame; the default is World Labs (RDF).
Cameras, colliders and renderers never compensate.

## Consequences

One conversion, in one place, with tests.
Adding a format means declaring its frame, not touching cameras.
