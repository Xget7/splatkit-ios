## Summary

<!-- The smallest view that makes the change clear: a call tree, a file tree, a diff sketch, pseudocode or a Mermaid diagram, each beside one or two lines of text. -->
<!-- Link the issue if there is one, and give the reasoning here: this repo keeps no decision documents. -->

## Evidence

<!-- A failing then passing test, command output, a capture or a screenshot. -->

- **Before:**
- **After:**
- **Where it ran:** <!-- Physical device or emulator/simulator, GPU and driver, OS version, world and splat count. -->
  <!-- Emulator only is fine for logic; renderer changes need a real GPU. -->
- **Checks:** <!-- Which of `python3 scripts/sdk_harness.py check engine`, `scripts/lint-cpp.sh`, the Android validation layer, `npm run check` and `npm run lint` ran, and which were skipped. -->
  <!-- Changes to decoding, sorting, math, navigation, loading or policy add tests in `splat-core` or `splatkit-engine`. -->

<!-- A performance claim needs before and after numbers on the same device and world. -->

## Merge Danger

**Door:** <!-- One-way or two-way. -->

<!-- Optional: why it can or cannot be walked back. -->

**Blast Radius:** <!-- One word. -->

<!-- What could break for hosts, the mirrors, releases or CI if this merges. -->
