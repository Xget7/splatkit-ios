#!/usr/bin/env python3
"""Export reviewed SDK paths into a new directory without touching the Git index."""
import hashlib
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
    "scripts/package-ios.sh", "scripts/build-ios.sh", "scripts/sdk_harness.py",
    "scripts/export-ios-source.py", "scripts/tests/test_sdk_harness.py",
    ".github/workflows/ios.yml", ".github/workflows/engine.yml", ".github/workflows/core.yml",
    "docs/BENCHMARKS.md", "docs/benchmarks",
)
FORBIDDEN = {".ply", ".spz", ".glb", ".lodsplat", ".a", ".so", ".dylib", ".pem", ".p12", ".key", ".keystore", ".mobileprovision"}
SECRET = re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}")


def main():
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *PACKAGES, *FILES],
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
        selected[name] = source
    for name in ("README.md", "CONTRIBUTING.md", "AGENTS.md"):
        selected[name] = ROOT / "packages/splatkit-ios/distribution" / name
    destination = Path(tempfile.mkdtemp(prefix="splatkit-ios-public-"))
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
