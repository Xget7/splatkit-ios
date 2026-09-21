## What this changes

<!-- One paragraph. Link the issue if there is one. -->

## Where it was tried

<!-- CONTRIBUTING asks for this: emulator only is fine for logic, renderer changes need a real GPU. -->

- Device, GPU and driver:
- OS version:
- World and splat count:

## Checklist

- [ ] `python3 scripts/sdk_harness.py check engine` passes, with tests added for anything touching decoding, sorting, math, navigation or policy.
- [ ] `scripts/lint-cpp.sh` is clean.
- [ ] The Khronos validation layer is silent in an Android debug build.
- [ ] `npm run check` passes for React Native package changes.
- [ ] No planning or decision documents; the reasoning is in this description and in comments beside the code.
- [ ] Markdown uses plain dashes and one sentence per line.

## Numbers, if this claims a performance change

<!-- Before and after, on the same device and world. A claim without a measurement does not land. -->
