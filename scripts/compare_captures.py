#!/usr/bin/env python3
"""Compare immutable, matched native captures. Requires Pillow; never resizes images."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys


def reject_nonfinite(value):
    raise ValueError(f"nonfinite JSON value: {value}")


def load_capture(manifest):
    from PIL import Image

    data = json.loads(manifest.read_text(), parse_constant=reject_nonfinite)
    for key in ("image", "image_sha256", "world_sha256", "camera", "settings", "environment"):
        if key not in data:
            raise ValueError(f"{manifest}: missing {key}")
    for key in ("image_sha256", "world_sha256"):
        value = data[key]
        if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
            raise ValueError(f"{manifest}: invalid {key}")
    camera = data["camera"]
    for key, count in (("view", 16), ("projection", 16), ("viewport", 2)):
        values = camera.get(key, [])
        if len(values) != count or not all(type(v) in (float, int) and math.isfinite(v) for v in values):
            raise ValueError(f"{manifest}: camera.{key} requires {count} finite numbers")
    if any(type(v) is not int or v <= 0 for v in camera["viewport"]):
        raise ValueError("viewport must contain positive integer dimensions")
    if not isinstance(data["settings"], dict) or not data["settings"]:
        raise ValueError("settings must explicitly record rendering parameters")
    required = {"render_scale", "sh_degree", "linear_blending", "splat_budget"}
    if not required <= data["settings"].keys():
        raise ValueError(f"settings require {sorted(required)}")
    path = manifest.parent / data["image"]
    if hashlib.sha256(path.read_bytes()).hexdigest() != data["image_sha256"]:
        raise ValueError(f"{path}: image hash mismatch")
    with Image.open(path) as source:
        if source.mode not in ("RGB", "RGBA"):
            raise ValueError("captures must be RGB/RGBA, not palette or grayscale")
        if source.mode == "RGBA" and source.getextrema()[3] != (255, 255):
            raise ValueError("captures must be opaque final-frame images")
        image = source.convert("RGB")
        if image.size != tuple(camera["viewport"]):
            raise ValueError("capture dimensions must match the recorded viewport")
        # Do not silently compare differently tagged colour encodings.
        data["icc_sha256"] = hashlib.sha256(source.info.get("icc_profile", b"")).hexdigest()
        data["png_gamma"] = source.info.get("gamma")
        data["png_srgb"] = source.info.get("srgb")
    return data, image


def compare(reference, candidate, varying=("splat_budget",), roi=None, max_mae=None):
    from PIL import ImageChops, ImageStat

    ref, left = load_capture(Path(reference))
    test, right = load_capture(Path(candidate))
    for key in ("world_sha256", "camera", "icc_sha256", "png_gamma", "png_srgb"):
        if ref[key] != test[key]:
            raise ValueError(f"unmatched {key}; no resize, alignment or colour conversion is implicit")
    if not set(varying) <= ref["settings"].keys() & test["settings"].keys():
        raise ValueError("varied settings must exist in both manifests")
    unchanged = lambda settings: {k: v for k, v in settings.items() if k not in varying}
    if unchanged(ref["settings"]) != unchanged(test["settings"]):
        raise ValueError("unmatched rendering settings outside --vary")
    if roi is not None:
        x, y, width, height = roi
        if min(x, y) < 0 or min(width, height) <= 0 or x + width > left.width or y + height > left.height:
            raise ValueError("ROI must be a nonempty rectangle inside both images")
        box = (x, y, x + width, y + height)
        left, right = left.crop(box), right.crop(box)
    diff = ImageChops.difference(left, right)
    hist = diff.histogram()
    bins = [sum(hist[c * 256 + i] for c in range(3)) for i in range(256)]
    channels = left.width * left.height * 3
    mae = sum(i * n for i, n in enumerate(bins)) / channels
    mse = sum(i * i * n for i, n in enumerate(bins)) / channels
    accumulated = 0
    p99 = 0
    for p99, count in enumerate(bins):
        accumulated += count
        if accumulated > (channels * 99) // 100:
            break
    worst = {"mae_255": -1.0, "rectangle": None}
    for y in range(0, left.height, 32):
        for x in range(0, left.width, 32):
            box = (x, y, min(x + 32, left.width), min(y + 32, left.height))
            error = sum(ImageStat.Stat(diff.crop(box)).mean) / 3
            if error > worst["mae_255"]:
                worst = {"mae_255": error, "rectangle": list(box)}
    if max_mae is not None and (not math.isfinite(max_mae) or not 0 <= max_mae <= 255):
        raise ValueError("max MAE must be finite and in [0, 255]")
    return {
        "schema_version": 1, "status": "measured" if max_mae is None else
        ("passed" if mae <= max_mae else "failed"),
        "reference": str(reference), "candidate": str(candidate),
        "environments": [ref["environment"], test["environment"]],
        "varied_settings": {k: [ref["settings"][k], test["settings"][k]] for k in varying},
        "roi": roi, "pixels": left.width * left.height,
        "mae_255": mae, "p99_channel_error_255": p99, "max_channel_error_255": max(i for i, n in enumerate(bins) if n),
        "psnr_db": 10 * math.log10(255 * 255 / mse) if mse else None,
        "identical_pixels": mse == 0, "channel_fraction_error_above_1": sum(bins[2:]) / channels,
        "worst_32px_region": worst, "max_mae_gate_255": max_mae,
        "visual_acceptance": "pending human review; numeric agreement is not perceptual acceptance",
        "scope": "encoded RGB differences; no temporal stability, phone sharpness or performance validation",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--vary", nargs="+", default=["splat_budget"])
    parser.add_argument("--roi", nargs=4, type=int, metavar=("X", "Y", "WIDTH", "HEIGHT"))
    parser.add_argument("--max-mae", type=float, help="explicit numeric gate in 0..255; no default quality claim")
    args = parser.parse_args()
    try:
        result = compare(args.reference, args.candidate, args.vary, args.roi, args.max_mae)
    except (OSError, ValueError, KeyError, TypeError, ImportError) as error:
        result = {"schema_version": 1, "status": "blocked", "error": str(error)}
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["status"] in ("measured", "passed") else 1


if __name__ == "__main__":
    sys.exit(main())
