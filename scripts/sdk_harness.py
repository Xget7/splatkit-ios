#!/usr/bin/env python3
"""Local SDK checks; JSON stdout, logs on disk, no device control."""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/sdk-harness"


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def plan(target, jobs):
    if target == "android":
        return [{"cwd": str(ROOT / "apps/android-dev"), "argv": [
            "./gradlew", "--console=plain", ":splatkit:assembleDebug", ":app:assembleDebug"]}]
    package = "splatkit-ios" if target == "metal" else "splatkit-engine"
    build = str(BUILD / target)
    commands = [
        ["cmake", "-S", str(ROOT / "packages" / package), "-B", build,
         "-DCMAKE_BUILD_TYPE=Release"],
        ["cmake", "--build", build, "--parallel", str(jobs)],
        ["ctest", "--test-dir", build, "--output-on-failure", "--no-tests=error"],
    ]
    return [{"cwd": str(ROOT), "argv": command} for command in commands]


def run_step(step, logfile, timeout):
    result = {**step, "log": str(logfile)}
    try:
        with logfile.open("w") as stream:
            process = subprocess.Popen(step["argv"], cwd=step["cwd"], stdout=stream,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                result["exit_code"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                result.update(exit_code=124, error="timeout")
    except OSError as error:
        result.update(exit_code=127, error=str(error))
    result["status"] = "passed" if result["exit_code"] == 0 else "failed"
    return result


def test_counts(path):
    cases = list(ET.parse(path).getroot().iter("testcase"))
    skipped = sum(case.find("skipped") is not None or case.get("status") == "notrun"
                  for case in cases)
    failed = sum(case.find("failure") is not None or case.find("error") is not None
                 for case in cases)
    return {"total": len(cases), "passed": len(cases) - skipped - failed,
            "failed": failed, "skipped": skipped}


def check(target, jobs, timeout):
    report = {"schema_version": 1, "target": target, "status": "failed", "steps": [],
              "host": platform.system(), "phone_performance_validated": False}
    if target == "metal" and platform.system() != "Darwin":
        return {**report, "status": "blocked", "error": "Metal checks require macOS"}
    BUILD.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=target + "-", dir=BUILD))
    commands = plan(target, jobs)
    junit = run / "tests.xml"
    if target != "android":
        commands[-1]["argv"] += ["--output-junit", str(junit)]
    for index, step in enumerate(commands):
        result = run_step(step, run / f"{index + 1}.log", timeout)
        report["steps"].append(result)
        if result["status"] != "passed":
            break
    else:
        report["status"] = "passed"
    if target == "android":
        report["scope"] = "AAR/APK packaging only; no rendering or compute execution"
        report["artifacts"] = [str(ROOT / relative) for relative in (
            "packages/splatkit-android/build/outputs/aar/splatkit-debug.aar",
            "apps/android-dev/app/build/outputs/apk/debug/app-debug.apk")]
        if report["status"] == "passed" and not all(Path(p).is_file() for p in report["artifacts"]):
            report.update(status="failed", error="build returned success without artifacts")
    else:
        report["scope"] = "macOS Metal + shared tests" if target == "metal" else "shared C++ tests"
        try:
            report["tests"] = test_counts(junit)
            if report["tests"]["passed"] == 0 or report["tests"]["failed"]:
                report["status"] = "failed"
        except (OSError, ET.ParseError) as error:
            report.update(status="failed", error=f"test evidence unavailable: {error}")
    report["report"] = str(run / "report.json")
    Path(report["report"]).write_text(json.dumps(report, indent=2) + "\n")
    return report


def android_log(contents, pid, expected, environment):
    # Standard `adb logcat -v threadtime`: ignore other apps and previous process IDs.
    line_pattern = re.compile(r"^\d\d-\d\d\s+[\d:.]+\s+(\d+)\s+\d+\s+[A-Z]\s+.*?:\s?(.*)$")
    messages = [match[2] for line in contents.splitlines()
                if (match := line_pattern.match(line)) and int(match[1]) == pid]
    text = "\n".join(messages)
    errors = [line for line in messages if re.search(
        r"Fatal signal|FATAL EXCEPTION|VUID-|Validation Error|World failed:|VK_ERROR_", line)]
    device = re.search(r"Vulkan device: (.+)", text)
    draws = re.findall(r"(\d+) drawn of (\d+) selected of (\d+)", text)
    valid_draw = any(0 < int(drawn) <= int(selected) <= int(loaded) == expected
                     for drawn, selected, loaded in draws)
    checks = {"vulkan_initialized": device is not None,
              "world_ready": f"world ready: {expected} splats" in messages,
              "nonempty_draw": valid_draw, "no_logged_errors": not errors}
    return {"schema_version": 1, "target": "android-log", "pid": pid,
            "environment": environment, "vulkan": device[1] if device else None,
            "status": "passed" if all(checks.values()) else "failed", "checks": checks,
            "errors": errors, "phone_performance_validated": False,
            "scope": "captured dev-app load/draw only; no visual or inactive-compute validation"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "check"):
        sub = commands.add_parser(name)
        sub.add_argument("target", choices=("engine", "metal", "android"))
        sub.add_argument("--jobs", type=positive, default=2)
        if name == "check":
            sub.add_argument("--timeout", type=positive, default=900, help="seconds per step")
    logs = commands.add_parser("android-log", help="validate an already captured threadtime log")
    logs.add_argument("log", type=Path)
    logs.add_argument("--pid", type=positive, required=True)
    logs.add_argument("--expected-splats", type=positive, required=True)
    logs.add_argument("--environment", choices=("emulator", "physical"), required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            report = {"schema_version": 1, "status": "not_run", "steps": plan(args.target, args.jobs)}
        elif args.command == "check":
            report = check(args.target, args.jobs, args.timeout)
        else:
            report = android_log(args.log.read_text(), args.pid, args.expected_splats, args.environment)
            report["log"] = str(args.log.resolve())
    except OSError as error:
        report = {"schema_version": 1, "status": "blocked", "error": str(error)}
    print(json.dumps(report, indent=2))
    return 0 if report["status"] in ("passed", "not_run") else 1


if __name__ == "__main__":
    sys.exit(main())
