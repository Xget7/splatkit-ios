import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:
    Image = None

spec = importlib.util.spec_from_file_location("compare_captures", Path(__file__).resolve().parents[1] / "compare_captures.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


@unittest.skipIf(Image is None, "Install scripts/requirements-validation.txt for capture checks")
class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def capture(self, name, image=None, budget=0):
        image = image or Image.new("RGB", (64, 64))
        path = self.root / (name + ".png")
        image.save(path)
        metadata = {"image": path.name, "image_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "world_sha256": "a" * 64, "environment": "synthetic test, not device evidence",
                    "camera": {"view": [1 if i % 5 == 0 else 0 for i in range(16)],
                               "projection": [1 if i % 5 == 0 else 0 for i in range(16)], "viewport": [64, 64]},
                    "settings": {"splat_budget": budget, "render_scale": 1, "sh_degree": 0, "linear_blending": False}}
        manifest = self.root / (name + ".json")
        manifest.write_text(json.dumps(metadata))
        return manifest

    def test_identical_images_are_not_automatic_perceptual_acceptance(self):
        result = compare.compare(self.capture("a"), self.capture("b", budget=100))
        self.assertTrue(result["identical_pixels"])
        self.assertEqual(result["status"], "measured")
        self.assertIsNone(result["psnr_db"])
        self.assertIn("pending", result["visual_acceptance"])

    def test_local_defects_are_reported_and_an_explicit_gate_can_fail(self):
        changed = Image.new("RGB", (64, 64))
        changed.putpixel((0, 0), (255, 255, 255))
        result = compare.compare(self.capture("a"), self.capture("b", changed), max_mae=0)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["max_channel_error_255"], 255)
        self.assertGreater(result["worst_32px_region"]["mae_255"], result["mae_255"])

    def test_unmatched_camera_settings_or_hash_is_blocked(self):
        a = self.capture("a")
        for field in ("camera", "settings", "image_sha256", "world_sha256"):
            b = self.capture("b")
            data = json.loads(b.read_text())
            if field == "camera":
                data[field]["view"][12] = 1
            elif field == "settings":
                data[field]["render_scale"] = 0.7
            else:
                data[field] = "b" * 64
            b.write_text(json.dumps(data))
            with self.subTest(field=field), self.assertRaises(ValueError):
                compare.compare(a, b)

    def test_no_implicit_resize_or_alpha_compositing(self):
        a = self.capture("a")
        for image in (Image.new("RGB", (32, 32)), Image.new("RGBA", (64, 64), (0, 0, 0, 0))):
            with self.assertRaises(ValueError):
                compare.compare(a, self.capture("b", image))

    def test_roi_is_explicit_and_bounded(self):
        a, b = self.capture("a"), self.capture("b")
        self.assertEqual(compare.compare(a, b, roi=[0, 0, 16, 16])["pixels"], 256)
        for roi in ([0, 0, 0, 2], [-1, 0, 2, 2], [60, 0, 8, 8]):
            with self.assertRaises(ValueError):
                compare.compare(a, b, roi=roi)


if __name__ == "__main__":
    unittest.main()
