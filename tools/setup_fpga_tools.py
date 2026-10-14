#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install a pinned official OSS CAD Suite locally on an Apple Silicon Mac."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
RELEASE = "2026-10-14"
ARCHIVE = "oss-cad-suite-darwin-arm64-20261014.tgz"
URL = f"https://github.com/YosysHQ/oss-cad-suite-build/releases/download/{RELEASE}/{ARCHIVE}"
SHA256 = "2dbecc2a8622076e12341132fec9fefd32dad0f10b2fd019172c7c9ad270cda4"


def main(argv=None):
    argparse.ArgumentParser(description=__doc__).parse_args(argv)
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise SystemExit("This pinned installer supports Apple Silicon macOS only.")
    directory = ROOT / "build/tools"
    downloads = directory / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / ARCHIVE
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        print(f"Downloading official OSS CAD Suite {RELEASE} (526 MB)", flush=True)
        urllib.request.urlretrieve(URL, temporary)
        temporary.replace(archive)
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != SHA256:
        raise SystemExit(f"Checksum mismatch; remove {archive} and retry.")
    suite = directory / "oss-cad-suite"
    if not (suite / "bin/yosys").exists():
        subprocess.run(["tar", "-xzf", str(archive), "-C", str(directory)], check=True)
    (directory / "release.json").write_text(json.dumps({
        "release": RELEASE, "platform": "darwin-arm64", "url": URL, "sha256": SHA256,
    }, indent=2) + "\n")
    print(f"Verified tools: {suite}")
    print(f"Build the compiler smoke test: python3 {ROOT / 'tools/fpga_build.py'}")


if __name__ == "__main__":
    main()
