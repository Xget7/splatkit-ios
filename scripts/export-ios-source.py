#!/usr/bin/env python3
"""Export reviewed SDK paths into a new directory without touching the Git index."""
import hashlib
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ("packages/splat-core", "packages/splatkit-engine", "packages/splatkit-ios")
FILES = (
    "Package.swift", "LICENSE", "CONTEXT.md", ".clang-format", ".gitignore",
    "CODE_OF_CONDUCT.md", "SECURITY.md",
    "scripts/package-ios.sh", "scripts/build-ios.sh", "scripts/sdk_harness.py",
    "scripts/export-ios-source.py", "scripts/tests/test_sdk_harness.py",
    ".github/workflows/ios.yml", ".github/workflows/engine.yml", ".github/workflows/core.yml",
    ".github/ISSUE_TEMPLATE", ".github/PULL_REQUEST_TEMPLATE.md",
    "docs/BENCHMARKS.md", "docs/benchmarks",
)
# React Native publishes this package at its repository root, with distribution/ files beside it.
RN_PACKAGE = "packages/react-native-splatkit"
# iOS publishes these files at its repository root; they link from there.
IOS_DISTRIBUTION = "packages/splatkit-ios/distribution/"
FORBIDDEN = {".ply", ".spz", ".glb", ".lodsplat", ".a", ".so", ".dylib", ".pem", ".p12", ".key", ".keystore", ".mobileprovision"}
# The only automated gate between the monorepo and two public repositories. Prefix-matched
# tokens are high-confidence; the generic key=value branch insists on a quoted, unbroken,
# non-template value so it does not fire on docs or shell examples.
SECRET = re.compile(
    rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED |PGP )?PRIVATE KEY-----"
    rb"|gh[pousr]_[A-Za-z0-9]{30,}"
    rb"|github_pat_[A-Za-z0-9_]{40,}"
    rb"|npm_[A-Za-z0-9]{36}"
    rb"|pypi-AgEIcHlwaS5vcmc[A-Za-z0-9_\-]{20,}"
    rb"|AKIA[A-Z0-9]{16}"
    rb"|aws_secret_access_key\s*[=:]\s*[\"']?[A-Za-z0-9/+=]{40}"
    rb"|xox[abprs]-[A-Za-z0-9-]{10,}"
    rb"|hooks\.slack\.com/services/[A-Za-z0-9_/]{20,}"
    rb"|AIza[0-9A-Za-z_\-]{35}"
    rb"|(?:sk|rk)_live_[0-9a-zA-Z]{20,}"
    rb"|(?:password|passwd|api[_-]?key|secret[_-]?key|access[_-]?token|auth[_-]?token)"
    rb"\s*[=:]\s*[\"'](?![^\"'<${}]*[ <${}])[A-Za-z0-9_\-./+]{16,}[\"']",
    re.IGNORECASE,
)


def published(platform, name):
    """The path a repository file takes in the platform's public repository."""
    if platform == "ios" and name.startswith(IOS_DISTRIBUTION):
        return name[len(IOS_DISTRIBUTION):]
    prefix = RN_PACKAGE + "/"
    if platform != "react-native" or not name.startswith(prefix):
        return name
    return name[len(prefix):].removeprefix("distribution/")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("ios", "android", "react-native"), default="ios")
    platform = parser.parse_args().platform
    packages, files = PACKAGES, FILES
    if platform == "react-native":
        packages = (RN_PACKAGE,)
        files = ("LICENSE", "CODE_OF_CONDUCT.md", "SECURITY.md",
                 ".github/ISSUE_TEMPLATE", ".github/PULL_REQUEST_TEMPLATE.md")
    if platform == "android":
        packages += ("packages/splatkit-android",)
        files += ("README.md", "CONTRIBUTING.md", "AGENTS.md", "CHANGELOG.md", ".clang-tidy",
                  "apps/android-dev", "scripts/lint-cpp.sh", "scripts/fetch-validation-layers.sh",
                  "scripts/check_android_alignment.py", "scripts/benchmark_report.py",
                  "scripts/compare_captures.py", "scripts/requirements-validation.txt", "scripts/tests",
                  ".github/workflows/android.yml", ".github/workflows/lint.yml",
                  "docs/AGENT_HARNESS.md", "docs/VALIDATION.md")
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *packages, *files],
        cwd=ROOT).decode().split("\0")
    selected = {}
    for name in sorted(set(names) - {""}):
        source = ROOT / name
        if not source.exists():  # Deleted source shader, superseded by shaders/Splat.metal.
            continue
        if source.is_symlink() or not source.is_file() or source.suffix in FORBIDDEN:
            raise ValueError(f"unexpected export file: {name}")
        if source.stat().st_size > 2_000_000:
            raise ValueError(f"oversized export file: {name}")
        if SECRET.search(source.read_bytes()):
            raise ValueError(f"secret-pattern match, review required: {name}")
        if platform == "react-native" and name == f"{RN_PACKAGE}/.gitignore":
            # This package's own .gitignore is for building it inside this monorepo;
            # distribution/.gitignore, unwrapped by published(), is the public repo's.
            continue
        target = published(platform, name)
        if target in selected:
            raise ValueError(f"two files publish to {target}")
        selected[target] = source
    destination = Path(tempfile.mkdtemp(prefix=f"splatkit-{platform}-public-"))
    manifest = {}
    for name, source in selected.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        manifest[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    (destination / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"directory": str(destination), "files": len(manifest),
                      "bytes": sum(path.stat().st_size for path in selected.values())}))


if __name__ == "__main__":
    main()
