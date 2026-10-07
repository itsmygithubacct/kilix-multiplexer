#!/usr/bin/env python3
"""Verify the vendored libvterm against original upstream hashes plus patches.

Only private temporary copies are patched. --root permits isolated verification
of a copied checkout, including negative tests, without changing source files.
"""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile

VENDOR = PurePosixPath("third_party/libvterm")


def upstream_files(root):
    expected = {}
    manifest = root / VENDOR / "SHA256SUMS"
    for number, line in enumerate(manifest.read_text().splitlines(), 1):
        if not line:
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise ValueError(f"invalid checksum manifest line {number}")
        checksum, name = match.groups()
        relative = PurePosixPath(name)
        if (relative.is_absolute() or ".." in relative.parts or
                relative.parts[:3] not in (VENDOR.parts + ("src",), VENDOR.parts + ("include",))):
            raise ValueError(f"manifest path outside vendored source: {name}")
        if name in expected:
            raise ValueError(f"duplicate manifest entry: {name}")
        expected[name] = checksum
    if not expected:
        raise ValueError("empty upstream checksum manifest")
    actual = set()
    for directory in ("src", "include"):
        for path in (root / VENDOR / directory).rglob("*"):
            if path.is_symlink():
                raise ValueError(f"unexpected source symlink: {path.relative_to(root)}")
            if path.is_file():
                actual.add(path.relative_to(root).as_posix())
    if actual != set(expected):
        missing = sorted(set(expected) - actual)
        extra = sorted(actual - set(expected))
        raise ValueError(f"source inventory differs from upstream manifest: missing={missing}, extra={extra}")
    return expected


def patch_series(root, expected):
    directory = root / VENDOR / "patches"
    names = [line.strip() for line in (directory / "series").read_text().splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    if not names or len(names) != len(set(names)):
        raise ValueError("patch series must list distinct numbered patches")
    numbers = []
    for name in names:
        match = re.fullmatch(r"([0-9]{4})-[A-Za-z0-9_-]+\.patch", name)
        if not match:
            raise ValueError(f"invalid numbered patch name: {name}")
        numbers.append(int(match.group(1)))
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"missing or symlinked patch: {name}")
        text = path.read_text()
        old = re.findall(r"^--- a/(.+)$", text, re.MULTILINE)
        new = re.findall(r"^\+\+\+ b/(.+)$", text, re.MULTILINE)
        if not old or old != new or any(name not in expected for name in old):
            raise ValueError(f"patch must modify only manifest files in place: {path.name}")
    if numbers != sorted(set(numbers)):
        raise ValueError("patch series numbers must be unique and increasing")
    recorded = set(names) | {"series"}
    actual = {path.name for path in directory.iterdir()}
    if actual != recorded:
        raise ValueError(f"unlisted patch entries: {sorted(actual - recorded)}")
    return [directory / name for name in names]


def verify(root):
    expected = upstream_files(root)
    patches = patch_series(root, expected)
    with tempfile.TemporaryDirectory(prefix="kmx-vendor-check-") as directory:
        scratch = Path(directory)
        for name in expected:
            target = scratch / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / name, target)
        # Undo the series from last to first, preserving the real vendor tree.
        for patch in reversed(patches):
            result = subprocess.run(["patch", "--batch", "--reverse", "--fuzz=0",
                "--no-backup-if-mismatch", "--strip=1", "--directory", str(scratch),
                "--input", str(patch)], capture_output=True, text=True)
            if result.returncode:
                raise ValueError(f"cannot reverse recorded patch {patch.name}:\n"
                                 + result.stdout + result.stderr)
        mismatches = []
        for name, checksum in expected.items():
            actual = hashlib.sha256((scratch / name).read_bytes()).hexdigest()
            if actual != checksum:
                mismatches.append(name)
        if mismatches:
            raise ValueError("unrecorded source changes after reversing patches: " + ", ".join(mismatches))
    print(f"{VENDOR}: {len(expected)} original upstream checksums verified after reversing {len(patches)} recorded patch(es)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    try:
        verify(args.root.resolve())
    except (OSError, ValueError) as error:
        print(f"check-vendor: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
