# 0008. One decode entry point, detected from bytes

Status: accepted. Date: 2026-09-04.

## Context

SPZ is the first format, not the last: SOG is what the fastest Android viewers load, and PLY is what every training pipeline writes.
Hosts hand the engine byte arrays from assets, downloads and content providers, so a file extension is not reliably available and should not decide anything.
Every renderer feature (spatial reorder, sorting, culling, SH) consumes `SplatCloud`, and none of them should know where the bytes came from.

## Decision

`splat::decodeSplatFile` is the only decode call the engine makes.
It detects the container from the leading bytes (`detectSplatFormat`) and hands them to that format's decoder; every decoder returns the same `SplatCloud` in the internal frame.
Adding a format is one branch in the detector, one decoder file in `splat-core/formats`, and its tests; nothing downstream changes, and the Kotlin `loadWorld(bytes)` stays as it is.
Options that only some formats need (the source frame for untagged files, the decoded size ceiling) live in `SplatDecodeOptions` and are ignored by formats that do not need them.

## Consequences

`SplatCloud` is the contract: a format that cannot produce it (for example one that streams) needs a new decision, not a special path in the engine.
Detection reads at most a few bytes, so a wrong file fails fast with `unsupportedFormat`, and a right file that is damaged fails with `corrupt`; hosts can tell the two apart.
