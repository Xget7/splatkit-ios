# Contributing

Keep platform-free algorithms in `splat-core`, orchestration in `splatkit-engine`, Metal in `splatkit-ios`.
Use the pinned dependency revisions and format C++ with the repository `.clang-format`.
Run `python3 scripts/sdk_harness.py check engine`, then `check metal` for rendering changes.
`bash scripts/package-ios.sh` builds device/simulator binaries and includes dependency licenses.
Report skipped tests, quality limitations and device/driver provenance.
Device tests require explicit approval; synthetic tests need no scene files.
Keep docs short; contracts belong beside code.
