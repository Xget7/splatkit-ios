import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("benchmark_report", Path(__file__).resolve().parents[1] / "benchmark_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

START = "benchmark started: 2970075 splats, one turn over 30 s, gpu 40.0 C\n"
FRAME = "benchmark: 1200 frames, 40.0 fps mean, frame ms mean 25.0 p50 24.0 p95 33.0 max 50.0\n"
GPU = "benchmark gpu ms: mean 23.0 p50 22.0 p95 32.0 max 49.0, gpu 60.0 C at the end\n"


class BenchmarkReportTest(unittest.TestCase):
    def test_legacy_summary_does_not_invent_p99_or_gpu_sample_count(self):
        result = report.summarize(START + FRAME + GPU, "physical")
        self.assertEqual(result["status"], "passed")
        self.assertIsNone(result["runs"][0]["frame_p99_ms"])
        self.assertIsNone(result["runs"][0]["gpu_sample_count"])
        self.assertFalse(result["phone_performance_validated"])

    def test_short_capture_does_not_pass_sustained_gate(self):
        result = report.summarize(START + FRAME + GPU, "physical", minimum_seconds=600)
        self.assertEqual(result["runs"][0]["status"], "too_short")
        self.assertEqual(result["status"], "failed")

    def test_incomplete_and_interrupted_runs_are_not_silently_dropped(self):
        for text in ("", START, START + FRAME, START + START + FRAME + GPU):
            self.assertEqual(report.summarize(text, "host")["status"], "failed")

    def test_filter_pid_and_fail_on_captured_validation_errors(self):
        def line(message, pid):
            return f"09-12 18:00:00.001 {pid} 99 I SplatKit: {message}\n"
        log = "".join(line(m, 42) for m in (START, FRAME, GPU))
        log += line("VUID-invalid", 99)
        self.assertEqual(report.summarize(log, "physical", 42)["status"], "passed")
        self.assertEqual(report.summarize(log + line("VUID-invalid", 42), "physical", 42)["status"], "failed")

    def test_new_tails_and_missing_gpu_queries(self):
        text = START + FRAME + GPU + "benchmark tails: frame p99 42.0, gpu p99 0.0, gpu samples 0 of 1200\n"
        run = report.summarize(text, "host")["runs"][0]
        self.assertEqual(run["frame_p99_ms"], 42)
        self.assertIsNone(run["gpu_mean_ms"])
        self.assertEqual(run["gpu_sample_count"], 0)

    def test_stage_means_are_separate_from_frame_means(self):
        sample = "40.0 fps, gpu 23.0 ms, sort 3.0 ms, cull 2.0 ms, select 4.0 ms, 100 drawn of 200 selected of 2970075\n"
        run = report.summarize(START + sample + FRAME + GPU, "host")["runs"][0]
        self.assertEqual(run["stage_samples"], 1)
        self.assertEqual(run["stage_sample_mean_ms"], {"sort": 3, "cull": 2, "select": 4})

    def test_idle_samples_after_finish_do_not_contaminate_the_run(self):
        sample = "40.0 fps, gpu 23.0 ms, sort 3.0 ms, cull 2.0 ms, select 4.0 ms, 100 drawn of 200 selected of 2970075\n"
        run = report.summarize(START + sample + FRAME + GPU + sample, "host")["runs"][0]
        self.assertEqual(run["stage_samples"], 1)


if __name__ == "__main__":
    unittest.main()
