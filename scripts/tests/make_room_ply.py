#!/usr/bin/env python3
"""Write a small Gaussian splat PLY of a closed room, for exercising scripts/prepare-world.sh.

    scripts/tests/make_room_ply.py room.ply

The room is a 4 by 3 by 4 metre box of opaque splats centred on the origin, which is where
splat_collider starts its walker, so every step of the pipeline, the collider included, has
something real to work on. The layout is the 3DGS reference one: float x, y, z, f_dc_0..2,
opacity (a logit), scale_0..2 (logs) and rot_0..3 (w first). Only the standard library is used.
"""

import math
import struct
import sys

HALF_WIDTH = 2.0
HALF_HEIGHT = 1.5
SPACING = 0.05
SPLAT_SIZE = 0.04
OPACITY_LOGIT = 5.0  # sigmoid(5) is about 0.993, solid enough to block the walker
PROPERTIES = (
    "x", "y", "z",
    "f_dc_0", "f_dc_1", "f_dc_2",
    "opacity",
    "scale_0", "scale_1", "scale_2",
    "rot_0", "rot_1", "rot_2", "rot_3",
)


def steps(half):
    count = int(round(2 * half / SPACING))
    return [-half + i * SPACING for i in range(count + 1)]


def room():
    """Yields (position, colour) for every splat of the six faces."""
    for a in steps(HALF_WIDTH):
        for b in steps(HALF_WIDTH):
            yield (a, -HALF_HEIGHT, b), (0.4, 0.3, 0.2)  # floor
            yield (a, HALF_HEIGHT, b), (0.9, 0.9, 0.9)  # ceiling
    for a in steps(HALF_WIDTH):
        for h in steps(HALF_HEIGHT):
            yield (a, h, -HALF_WIDTH), (0.8, 0.2, 0.2)
            yield (a, h, HALF_WIDTH), (0.2, 0.8, 0.2)
            yield (-HALF_WIDTH, h, a), (0.2, 0.2, 0.8)
            yield (HALF_WIDTH, h, a), (0.8, 0.8, 0.2)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    log_size = math.log(SPLAT_SIZE)
    # f_dc is the degree-0 harmonic: colour = 0.5 + C0 * f_dc.
    c0 = 0.28209479177387814
    rows = [
        struct.pack(
            "<14f",
            *position,
            *((channel - 0.5) / c0 for channel in colour),
            OPACITY_LOGIT,
            log_size, log_size, log_size,
            1.0, 0.0, 0.0, 0.0,
        )
        for position, colour in room()
    ]
    header = (
        "ply\nformat binary_little_endian 1.0\n"
        f"element vertex {len(rows)}\n"
        + "".join(f"property float {name}\n" for name in PROPERTIES)
        + "end_header\n"
    )
    with open(sys.argv[1], "wb") as out:
        out.write(header.encode("ascii"))
        out.writelines(rows)
    print(f"wrote {len(rows)} splats: {sys.argv[1]}")


if __name__ == "__main__":
    main()
