# Benchmark log

## ISS experiments, 2026-09-11/12

Physical A19 Pro snapshot means below; Mac checks are separate.
Command intervals overlap: HUD sort includes visibility/radix, not isolated sort.
Quality remains unapproved; short runs do not establish sustained 30/60 FPS.
Exact settings, hashes, memory samples and limitations remain in the linked artifacts.

| Run | FPS mean | Evidence |
|---|---:|---|
| Original hybrid, partial | 11.06 | [JSON](benchmarks/2026-09-11-iss-metal-512.json) |
| Initial LOD 1.2M, quality rejected | 35.75 | [JSON](benchmarks/2026-09-12-iss-metal-lod-phone.json) |
| Guarded LOD 2.2M, full orbit | 21.63 | [JSON](benchmarks/2026-09-12-iss-metal-lod-guarded.json) |
| LOD hardware / hybrid | 23.11 / 18.92 | [JSON](benchmarks/2026-09-12-iss-lod-hybrid.json) |
| 16-bit radix | 23.36 | [JSON](benchmarks/2026-09-12-iss-radix16.json) |
| Horizontal framing, 10 seconds | 28.12 | [JSON](benchmarks/2026-09-12-iss-horizontal-smoke.json) |

Horizontal framing changes the visible set; it is not a radix A/B.
[Prepared startup](benchmarks/2026-09-12-iss-prepared-start.json) waits for GPU completion, not quality.
[Initial Mac LOD](benchmarks/2026-09-12-iss-metal-lod.json) and [interior traversal](benchmarks/2026-09-12-iss-metal-lod-sse.json) retain rejected image comparisons and the uninstrumented signal-9 phone failure.
Guarded measurements observed a ~3.54 GB process limit, not a guaranteed 5 GB.

Every optimisation starts with a measurement of the state before it, recorded here, and ends with the same measurement after.
Device: Xiaomi Mi 9, Adreno 640, Vulkan 1.1.128, Android 11, release build, phone cooled below 48 C before each run.
Command: `adb shell am start -n com.splatkit.devapp/.MainActivity --es world <file> --ez benchmark true [--ef scale S] [--ef seconds N]`, one full turn in place, GPU time from timestamp queries.

| Date | Commit | Scene | Settings | GPU ms mean / p50 / p95 | Sort ms | Cull ms | Drawn of total | Load: decode / reorder / upload ms |
|---|---|---|---|---|---|---|---|---|
| 2026-09-04 | 91b8d76 | house 2M | scale 1.0 | 32.4 / 32.0 / 41.1 | 41 to 46 | 10 | 143k to 291k of 2M | 530 / 470 / 270 |
| 2026-09-04 | 91b8d76 | raccoon 932k, SH 3 | scale 1.0 | 14.4 / 12.9 / 27.8 | 95 | n/a | up to 932k | 634 / 216 / 501 |
| 2026-09-04 | 91b8d76 | raccoon 932k, SH 0 | scale 1.0 | 13.9 / 12.4 / 26.7 | 95 | n/a | up to 932k | |

| 2026-09-04 | LoD tree | house 2M, budget 500k | scale 1.0, budget spread over the sphere | 25.6 / 21.7 / 45.7 | 18 | 13 | 18k to 52k of 500k | tree build 4.3 s on the Mac, blurry: rejected |
| 2026-09-04 | LoD tree | house 2M, budget 500k | scale 1.0, budget weighted ahead (0.02 behind) | 32.4 / 32.5 / 42.4 | 16 | 14 | 152k of 500k | select 91 ms, still soft |
| 2026-09-04 | LoD tree | house 2M, budget 1M | scale 1.0, budget weighted ahead | 31.1 / 32.1 / 41.3 | 34 | 14 | 239k of 1M | select 218 ms, looks like the full scene |
| 2026-09-04 | LoD tree | house 2M, no budget | scale 1.0 | 32.3 / 31.9 / 40.9 | 44 | 10 | 149k to 289k of 2M | reference |

Reading: at full resolution the house draws 150k to 290k splats and nearly all of them are larger than a pixel, so a level of detail tree has nothing to merge in view.
A budget below what the view needs blurs the image without saving GPU time, because the merged nodes are bigger and cost the fragments the leaves would have.
The tree pays off when the scene is bigger than what the view needs at a pixel each (far content, many millions of splats) or at low quality modes; it is off by default.

| 2026-09-04 | ADR 0011 | house 2M | scale 0.999, sRGB attachment (old default) | 32.8 / 32.4 / 41.8 | | | | reference |
| 2026-09-04 | ADR 0011 | house 2M | scale 0.999, RGBA8 UNORM attachment | 20.1 / 19.6 / 26.3 | | | | blending in the encoded space |
| 2026-09-04 | ADR 0011 | house 2M | scale 0.999, RGB565 attachment | 20.1 / 19.5 / 26.8 | | | | same as UNORM8: the cost was sRGB, not the bytes |
| 2026-09-04 | ADR 0011 | house 2M | scale 1.0, sustained performance mode | 32.6 / 32.3 / 41.8 | | | | no change: rejected |
| 2026-09-04 | ADR 0011 | house 2M | scale 1.0, new default | 19.8 / 19.4 / 26.3 | | | | |
| 2026-09-04 | ADR 0011 | house 2M | scale 0.7, new default | 13.7 / 13.6 / 17.8 | | | | 60 fps |
| 2026-09-04 | ADR 0011 | house 2M | scale 0.5, new default | 12.6 / 13.2 / 15.1 | | | | no cheaper than 0.7: per splat floor |
| 2026-09-04 | ADR 0011 | house 2M | scale 1.0, linearBlending | 32.6 / 32.2 / 41.6 | | | | the old path, still available |
| 2026-09-04 | ADR 0011 | kitchen 500k | scale 1.0, new default | 14.8 / 14.0 / 20.7 | | | | was 19.4 |
| 2026-09-04 | ADR 0011 | kitchen 500k | scale 0.7, new default | 12.6 / 12.8 / 15.0 | | | | |

| 2026-09-04 | ADR 0012 | house 2M | preset HIGH: scale 1.0, SH 3, all splats, margin 10 | 19.8 / 19.3 / 26.2 | 43 to 47 | 10 | 216k to 419k of 2M | the default, matches the ADR 0011 row |
| 2026-09-04 | ADR 0012 | house 2M | preset MEDIUM: scale 0.7, SH 1, all splats, margin 10 | 13.4 / 13.4 / 17.5 | | | | 60 fps |
| 2026-09-04 | ADR 0012 | house 2M | preset LOW: scale 0.5, SH 0, budget 500k, margin 10 | 12.1 / 12.4 / 14.7 | 18 | 14 | 178k to 207k of 500k | select 89 to 101 ms, tree 2.4 s on the phone at load |
| 2026-09-04 | ADR 0012 | house 2M | LOW without the budget: scale 0.5, SH 0 | 12.6 / 13.1 / 15.0 | | | | the budget buys 0.7 ms here; its point is scenes bigger than this |
| 2026-09-04 | ADR 0012 | house 2M | preset ULTRA: scale 1.5 (1620x3391), SH 3, all splats, margin 20 | 39.4 / 39.1 / 50.0 | 43 | 10 | 419k of 2M | supersampled, 25 fps on the Mi 9, GPU 61 C at the end |

| 2026-09-07 | abcd950 | house 2M | preset HIGH, R8 minified dev app, phone rebooted that day, 150 MB free | 21.9 / 21.6 / 27.9 | | | | |
| 2026-09-07 | abcd950 | house 2M | preset HIGH, same build unminified, same session | 21.8 / 21.5 / 28.0 | | | | R8 changes nothing on the GPU; the phone is 2 ms slower than on the 4th |

Reading: day to day the same build moves by about 2 ms on this phone, so a change under that needs an A/B in the same session, never a comparison against an older row.

Reading: the presets sit on the measured curve, medium is the 60 fps point on this phone, and ultra costs the square of its scale as predicted (1.5 squared times 19.3 is 43).

Earlier numbers, before this log existed, are in the roadmap tables.

Apartment Pool (superspl.at, CC BY, paul/nesterdigital): 3.57M splats converted with `ply2spz --sh 0`, no collider, benchmark from (-0.3, 2.3, 4.0) looking at the pool. The frame is all blended fragments here: it scales with the pixels and with the splat count alike.

| 2026-09-08 | 9732897 | pool 3.57M | preset HIGH | 73.5 / 65.8 / 123.3 | 72 to 97 | 16 to 20 | 464k of 3.57M | 14 fps: this scene's splats are large on screen, 3.5 times the house's cost for 1.75 times its splats |
| 2026-09-08 | 9732897 | pool 3.57M | preset MEDIUM | 41.4 / 36.6 / 63.3 | | | | scale 0.7 is 0.49 of the pixels and 0.56 of the time |
| 2026-09-08 | 9732897 | pool 3.57M | preset LOW (budget 500k) | 20.2 / 19.2 / 32.5 | | | | |
| 2026-09-08 | 9732897 | pool 3.57M | HIGH, the 342 splats over 5 m dropped (`--drop-over 5`) | 72.6 / 65.8 / 116.9 | | | | the huge background splats cost nothing measurable; not the fragment problem |
| 2026-09-08 | 9732897 | pool 1.79M (`--keep 2`) | preset HIGH | 36.0 / 32.5 / 56.1 | | | | half the splats, half the time |
| 2026-09-08 | 9732897 | pool 1.79M (`--keep 2`) | preset MEDIUM | 20.8 / 18.1 / 31.1 | | | | 55 fps, the demo setting for this scene |

Mip-NeRF 360 Bicycle (superspl.at, CC BY, Seeget3D): 2.6M splats with harmonics degree 3, converted with `ply2spz` untouched, no collider, benchmark from (0, 0.7, 1.6) pitch -0.2 looking at the bike. The scene is an outdoor orbit: the bike side costs a third of the frame, the tree canopy behind the camera fills it.

| 2026-09-08 | 11daaf7 | bicycle 2.6M | preset HIGH | 19.4 / 13.7 / 45.3 | 64 | 26 to 47 | 293k to 1.48M of 2.6M | 1637 / - / 1047 |

Harmonics degree drawn, 2026-09-09, same bicycle file and pose, portrait 1080x2261 and landscape 2265x1080 (the landscape run sees the canopy across the whole width, hence the load).
The degree is chosen per frame from the data already uploaded, so the in place rows switch it on the running world with no reload; the fresh rows load the world with that degree, and each run started with the GPU below 40 C.

| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, portrait | HIGH, degree 3, fresh load | 18.9 / 13.7 / 45.2 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, portrait | HIGH, degree 0, fresh load | 17.2 / 13.8 / 39.0 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, portrait | HIGH, degree 3, fresh load again | 18.8 / 13.0 / 47.8 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, portrait | HIGH, degree 0 in place | 19.1 / 14.0 / 49.0 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, portrait | HIGH, degree 3 in place | 20.8 / 13.7 / 67.1 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, portrait | HIGH, degree 0 in place again | 19.0 / 14.4 / 51.6 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, landscape | HIGH, degree 3, fresh load | 40.9 / 16.5 / 212.9 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, landscape | HIGH, degree 0, fresh load | 28.5 / 14.6 / 122.0 | | | | |
| 2026-09-09 | 35b9400 plus the per frame degree | bicycle 2.6M, landscape | HIGH, degree 3, fresh load again | 41.1 / 16.5 / 212.3 | | | | |
| 2026-09-09 | 35b9400 unchanged | bicycle 2.6M, landscape | HIGH, degree 3 | 41.3 / 16.5 / 212.0 | | | | |
| 2026-09-09 | 35b9400 unchanged | bicycle 2.6M, landscape | HIGH, `--ei sh 0` | 28.8 / 14.7 / 122.6 | | | | |

Reading: degree 3 costs 9 percent of GPU time over degree 0 in portrait and 30 percent in landscape, where the frame is vertex bound on the canopy, and choosing the degree per frame costs nothing against the build that fixed it at load (41.1 against 41.3 in landscape).
Switching the degree on the running world was checked by capture: degree 3 set in place matches a fresh degree 3 load within 0.1 of 255 on average, degree 0 differs from it by 1.4 and degree 1 by 1.6, with no world decode in the log.

## Image quality against the reference renderer

2026-09-08, pool 3.57M with spherical harmonics degree 3, Mi 9 at preset HIGH (1080x2261, 65 degree vertical field of view, no render target), pose (-0.3, 2.3, 4.0) yaw -0.75 pitch -0.15.
The reference is the PlayCanvas engine (the renderer behind superspl.at) driven by a local page at the same pose, field of view and resolution, gamma sRGB, no tone mapping, no antialiasing.
Sharpness is the variance of a 4-neighbour Laplacian over the same 1080x1200 crop; the mean horizontal gradient is in parentheses.

| Render | Data | Sharpness |
|---|---|---|
| Reference | PLY | 71.9 (4.76) |
| Reference | PLY with every covariance rounded to half floats, as the 32 byte record stores it | 71.7 |
| Reference | the phone's SPZ, converted back to PLY | 69.3 |
| Reference | the phone's SPZ, radial sort instead of view depth | same as view depth within 0.1 |
| Mi 9 HIGH | the phone's SPZ | 53.0 (4.41) |
| Mi 9 ULTRA (1.5x supersampled) | the phone's SPZ | 49.7 |
| Mi 9 MEDIUM (0.7x) | the phone's SPZ | 31.6 |

Reading: the half float covariance and the radial sort cost nothing, the SPZ quantisation costs 4 percent, and the phone is a further 7 percent softer at the edges than the reference with the same data; that residual is open (the quad reaches 3 sigma against the reference's 2.83, and the 65 degree field of view spans 2261 rows instead of 2340).
The difference people see against the superspl.at page is elsewhere: that viewer is landscape with a wide field of view, while the phone in portrait shows 34 degrees across 1080 pixels, 1.4 times the magnification, so every splat is 1.4 times larger on screen.
Sh degree 3 against degree 0 at the home pose changes the image by 3.5 of 255 on average.
