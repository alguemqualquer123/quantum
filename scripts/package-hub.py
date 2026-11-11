#!/usr/bin/env python3
# (c) SR VINIX
# SPDX-License-Identifier: MIT

"""Create the standalone Quantum Hub download from an installed build."""

from __future__ import annotations

import argparse
import re
import zipfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path, help="installed bin directory")
    parser.add_argument("--version", required=True, help="release version, for example v0.8.3")
    parser.add_argument("--platform", default="windows-amd64", help="package platform label")
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        raise SystemExit(f"Hub package root does not exist: {root}")

    if "win" in args.platform.lower():
        required = [root / "quantum-hub.exe", root / "quantum.dll"]
        files = [root / "quantum-hub.exe", root / "quantum.dll"]
        files.extend(sorted(root.glob("*.dll")))
    elif "mac" in args.platform.lower():
        required = [root / "quantum-hub", root / "libquantum.dylib"]
        files = required[:]
    else:
        required = [root / "quantum-hub"]
        files = [root / "quantum-hub"]
        files.extend(sorted(root.glob("libquantum.so*")))

    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise SystemExit("Hub package is missing: " + ", ".join(missing))

    files = list(dict.fromkeys(path for path in files if path.is_file()))
    if any(path.name.startswith("quantum-editor") for path in files):
        raise SystemExit("standalone Hub package must not contain the editor executable")

    version = args.version if args.version.startswith("v") else "v" + args.version
    safe_version = re.sub(r"[^A-Za-z0-9._-]", "", version)
    package_root = f"quantum-hub-{safe_version}-{args.platform}"
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            archive.write(path, f"{package_root}/{path.name}")
        readme = (
            "Quantum Hub\n"
            "===========\n\n"
            "Run quantum-hub to download and install the Quantum Engine editor from GitHub.\n"
            "The Hub verifies the SHA-256 checksum published in the release manifest.\n"
        )
        archive.writestr(f"{package_root}/README.txt", readme)

    print(f"created {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
