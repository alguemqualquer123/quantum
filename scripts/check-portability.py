#!/usr/bin/env python3
# (c) SR VINIX
# SPDX-License-Identifier: MIT

"""Run cheap, dependency-free portability checks used by release CI."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


NATIVE_INCLUDE_RE = re.compile(
    r"#\s*include\s*[<\"](windows\.h|X11/[^\">]+|Cocoa/[^\">]+|UIKit/[^\">]+|"
    r"android/[^\">]+|emscripten/[^\">]+)"
)
ABSOLUTE_PATH_RE = re.compile(r"/(?:Users|runner)/")
WINDOWS_CMAKE_PATH_RE = re.compile(r"\b[A-Za-z]:[\\/]")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".m", ".mm"}


def iter_sources(root: Path):
    for base in (root / "engine", root / "editor", root / "tools"):
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix.lower() not in SOURCE_SUFFIXES or "libs" in path.parts:
                continue
            yield path


def check_platform_includes(root: Path, errors: list[str]) -> None:
    allowed_roots = {
        "windows.h": "platform/win",
        "X11/": "platform/linux",
        "Cocoa/": "platform/mac",
        "UIKit/": "platform/apple",
        "android/": "platform/android",
        "emscripten/": "platform/emscripten",
    }
    for path in iter_sources(root):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), 1):
            match = NATIVE_INCLUDE_RE.search(line)
            if not match:
                continue
            header = match.group(1)
            expected = next((folder for prefix, folder in allowed_roots.items() if header.startswith(prefix)), None)
            if expected and expected not in relative:
                if expected == "platform/linux" and "linux" in relative.lower():
                    continue
                if not re.search(r"_WIN32|__APPLE__|__ANDROID__|__EMSCRIPTEN__|QUANTUM_LINUX", text):
                    errors.append(f"{relative}:{line_number}: native include lacks a platform guard: {header}")


def check_absolute_paths(root: Path, errors: list[str]) -> None:
    candidates = [root / "CMakeLists.txt", root / "engine", root / "editor", root / "tools"]
    files: list[Path] = []
    for candidate in candidates:
        if candidate.is_file():
            files.append(candidate)
        elif candidate.exists():
            files.extend(p for p in candidate.rglob("*") if p.is_file() and p.suffix in {".cmake", ".cpp", ".h", ".hpp", ".mm", ".m", ".yml", ".yaml"})
    for path in files:
        if "libs" in path.parts or "GoogleMobileAds" in path.parts:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), 1):
            machine_path = bool(ABSOLUTE_PATH_RE.search(line))
            if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
                machine_path = machine_path or bool(WINDOWS_CMAKE_PATH_RE.search(line))
            if machine_path and "CMAKE_CURRENT" not in line and "CMAKE_BINARY" not in line:
                errors.append(f"{path.relative_to(root).as_posix()}:{line_number}: machine-specific absolute path")


def check_required_files(root: Path, errors: list[str]) -> None:
    required = [
        ".github/workflows/cmake.yml",
        ".github/workflows/engine-android.yml",
        ".github/workflows/engine-emscripten.yaml",
        ".github/workflows/engine-xcode.yaml",
        "engine/platform/win",
        "engine/platform/linux",
        "engine/platform/mac",
        "engine/platform/android",
        "engine/platform/emscripten",
        "cmake/ReleaseManifest.json.in",
    ]
    for entry in required:
        if not (root / entry).exists():
            errors.append(f"missing release input: {entry}")


def check_manifest_template(root: Path, errors: list[str]) -> None:
    template = root / "cmake" / "ReleaseManifest.json.in"
    try:
        data = json.loads(re.sub(r"@[A-Za-z0-9_]+@", "placeholder", template.read_text(encoding="utf-8")))
        if data.get("schema") != 1:
            errors.append("cmake/ReleaseManifest.json.in: unsupported schema")
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"cmake/ReleaseManifest.json.in: invalid JSON template ({exc})")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    root = parser.parse_args().root.resolve()
    errors: list[str] = []
    check_required_files(root, errors)
    check_manifest_template(root, errors)
    check_platform_includes(root, errors)
    check_absolute_paths(root, errors)
    if errors:
        print("portability check failed:", file=sys.stderr)
        print("\n".join(f"- {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"portability check passed: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
