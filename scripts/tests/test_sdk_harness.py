import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "sdk_harness.py"
spec = importlib.util.spec_from_file_location("sdk_harness", SCRIPT)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


def log(message, pid=42):
    return f"09-12 06:53:50.703 {pid} 100 I SplatKit: {message}\n"


class HarnessTest(unittest.TestCase):
    def ready(self):
        return (log("Vulkan device: SwiftShader Device (LLVM 10.0.0), API 1.3.0")
                + log("world ready: 500000 splats")
                + log("123 drawn of 500000 selected of 500000"))

    def verdict(self, text):
        return harness.android_log(text, 42, 500000, "emulator")

    def test_complete_log_is_smoke_not_benchmark(self):
        result = self.verdict(self.ready())
        self.assertEqual(result["status"], "passed")
        self.assertFalse(result["phone_performance_validated"])
        self.assertEqual(result["environment"], "emulator")

    def test_crash_fails_even_after_ready(self):
        result = self.verdict(self.ready() + log("Fatal signal 11 (SIGSEGV)"))
        self.assertEqual(result["status"], "failed")
        self.assertFalse(result["checks"]["no_logged_errors"])

    def test_other_process_cannot_supply_success(self):
        self.assertEqual(self.verdict(self.ready().replace(" 42 ", " 99 "))["status"], "failed")

    def test_other_process_crash_is_ignored(self):
        self.assertEqual(self.verdict(self.ready() + log("Fatal signal 11", 99))["status"], "passed")

    def test_empty_wrong_world_and_incomplete_logs_fail(self):
        for text in ("", log("world ready: 500000 splats"),
                     self.ready().replace("500000", "400000"),
                     self.ready().replace("123 drawn", "0 drawn")):
            with self.subTest(text=text):
                self.assertEqual(self.verdict(text)["status"], "failed")

    def test_validation_error_fails(self):
        self.assertEqual(self.verdict(self.ready() + log("VUID-vkCmdDraw-None-02700"))["status"], "failed")

    def test_plan_never_installs_or_launches(self):
        steps = harness.plan("android", 2)
        self.assertEqual(steps[0]["argv"], ["./gradlew", "--console=plain",
                                           ":splatkit:assembleDebug", ":app:assembleDebug"])
        self.assertIn("--no-tests=error", harness.plan("metal", 2)[-1]["argv"])

    def test_exit_code_and_timeout_are_failures(self):
        with tempfile.TemporaryDirectory() as folder:
            for code, timeout, expected in (("raise SystemExit(3)", 5, 3),
                                            ("import time; time.sleep(10)", 1, 124)):
                step = {"cwd": folder, "argv": [sys.executable, "-c", code]}
                result = harness.run_step(step, Path(folder) / "step.log", timeout)
                self.assertEqual(result["status"], "failed")
                self.assertEqual(result["exit_code"], expected)

    def test_missing_tool_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            step = {"cwd": folder, "argv": [str(Path(folder) / "missing-tool")]}
            self.assertEqual(harness.run_step(step, Path(folder) / "step.log", 1)["exit_code"], 127)

    def test_skips_are_reported_separately(self):
        with tempfile.TemporaryDirectory() as folder:
            junit = Path(folder) / "tests.xml"
            junit.write_text('<testsuite><testcase/><testcase><skipped/></testcase>'
                             '<testcase><failure/></testcase></testsuite>')
            self.assertEqual(harness.test_counts(junit), {"total": 3, "passed": 1, "failed": 1, "skipped": 1})

    def test_cli_plan_is_json_and_missing_log_is_nonzero(self):
        import json
        result = subprocess.run([sys.executable, str(SCRIPT), "plan", "engine"], capture_output=True, text=True)
        self.assertEqual(json.loads(result.stdout)["status"], "not_run")
        self.assertEqual(result.returncode, 0)
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run([sys.executable, str(SCRIPT), "android-log", folder + "/absent",
                                     "--pid", "42", "--expected-splats", "500000",
                                     "--environment", "emulator"], capture_output=True, text=True)
            self.assertEqual(json.loads(result.stdout)["status"], "blocked")
            self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
