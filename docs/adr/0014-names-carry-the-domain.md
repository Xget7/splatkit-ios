# 0014. Names carry the domain

Status: accepted. Date: 2026-09-09.

## Context

After [ADR 0013](0013-engine-modules.md) the code had the right seams and the wrong labels.
The root object was `Engine`, loose at the top of `cpp/`, while every other class lived in the directory of its job.
The Kotlin wrapper of that same object was `NativeEngine`, so one thing had two names and the JNI symbols carried the second.
`SurfaceRenderer` and `WorldLoader` said what they did but not what project they belonged to, and a reader landing on any of them from a stack trace, a profiler or another repository had to work out the context.
The reference libraries this project is measured against name their roots for the domain: `SplatRenderer`, `SplatIO`.

## Decision

The root object of each layer carries the domain: `SplatEngine` in C++ and in Kotlin, `VulkanSplatRenderer`, `SplatWorldLoader`, next to the existing `SplatSurfaceView` and `SplatPipeline`.
The same object has the same name on both sides of a language boundary, and the JNI symbols derive from that one name.
Every object lives in the directory of its job; nothing sits loose at the root of `cpp/`.
Objects below the root keep short job names, `Swapchain`, `FrameLoop`, `Benchmark`, because the directory and the root already say the domain.

## Consequences

`Engine.cpp` moved to `engine/SplatEngine.cpp`, `NativeEngine.kt` became `SplatEngine.kt`, and the ProGuard rule and the JNI prefix followed the class.
The public Kotlin API did not change; the rename is inside the AAR, so hosts on alpha04 are unaffected.
Any future layer, an iOS renderer for one, gets its root name from the rule rather than from taste.
