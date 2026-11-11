# (c) SR VINIX
# SPDX-License-Identifier: MIT

"""Headless contract smoke test for editor recovery and crash diagnostics."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def source_check(results: list[dict[str, object]], name: str, path: Path,
                 pattern: str, detail: str) -> None:
    matched = re.search(pattern, read_text(path), re.MULTILINE | re.DOTALL) is not None
    results.append({
        "name": name,
        "status": "PASS" if matched else "FAIL",
        "detail": detail if matched else f"missing source contract in {path}",
    })


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def make_fixture() -> tuple[Path, dict[Path, str]]:
    fixture = Path(tempfile.mkdtemp(prefix="quantum-editor-stability-"))
    files = {
        fixture / "settings.yaml": "window:\n  width: [malformed\n",
        fixture / "recent-projects.fixture.yaml": "recent_projects:\n" + "".join(
            f"  - {fixture / f'missing-project-{index}'}\n" for index in range(40)
        ),
        fixture / "editor_layout.ini": "[Docking][Data]\nSentinel=preserve\n",
        fixture / ".editor-session-active": "editor session active\n",
        fixture / "editor.log": "x" * (2 * 1024 * 1024 + 1),
        fixture / "editor.log.1": "previous crash log\n",
    }
    for path, content in files.items():
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")
    return fixture, {path: sha256(path) for path in files}


def find_editor_cmd(root: Path, requested: str | None) -> Path | None:
    if requested:
        candidate = Path(requested).resolve()
        return candidate if candidate.is_file() else None
    candidates = [
        root / "build" / "Debug" / "quantum-editor-cmd.exe",
        root / "build" / "Release" / "quantum-editor-cmd.exe",
        root / "build" / "Debug" / "quantum-editor-cmd",
        root / "build" / "Release" / "quantum-editor-cmd",
    ]
    return next((path.resolve() for path in candidates if path.is_file()), None)


def run_editor_cmd(results: list[dict[str, object]], fixture: Path,
                   executable: Path | None, required: bool) -> None:
    if executable is None:
        results.append({
            "name": "editor_cmd_help",
            "status": "FAIL" if required else "SKIP",
            "detail": "no quantum-editor-cmd executable found; GUI was not launched",
        })
        return
    if "editor-cmd" not in executable.name.lower():
        results.append({
            "name": "editor_cmd_help",
            "status": "FAIL",
            "detail": f"refusing non-console executable: {executable}",
        })
        return
    try:
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        completed = subprocess.run(
            [str(executable), "--help"], cwd=fixture, capture_output=True,
            text=True, timeout=15, check=False, creationflags=flags,
        )
        output = (completed.stdout + completed.stderr).strip()
        passed = completed.returncode == 0 and bool(re.search(r"usage|help", output, re.I))
        results.append({
            "name": "editor_cmd_help",
            "status": "PASS" if passed else "FAIL",
            "detail": f"exit={completed.returncode}, executable={executable}",
            "output": output[-500:],
        })
    except (OSError, subprocess.SubprocessError) as error:
        results.append({
            "name": "editor_cmd_help",
            "status": "FAIL",
            "detail": f"headless console smoke failed: {error}",
        })


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--editor-cmd", type=str)
    parser.add_argument("--require-editor-cmd", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    required_files = [
        root / "editor" / "AppSettings.cpp",
        root / "editor" / "App.cpp",
        root / "editor" / "cli" / "LaunchOptions.h",
        root / "editor" / "Out.cpp",
        root / "editor" / "util" / "CrashHandler.cpp",
    ]
    missing = [str(path) for path in required_files if not path.is_file()]
    if missing:
        print(json.dumps({"status": "FAIL", "missing": missing}, indent=2), file=sys.stderr)
        return 2

    settings = root / "editor" / "AppSettings.cpp"
    app = root / "editor" / "App.cpp"
    launch = root / "editor" / "cli" / "LaunchOptions.h"
    out = root / "editor" / "Out.cpp"
    crash = root / "editor" / "util" / "CrashHandler.cpp"
    results: list[dict[str, object]] = []
    source_check(results, "session_marker_observed", settings,
                 r"previousSessionUnclean\s*=\s*std::filesystem::exists\(sessionMarker\)",
                 "startup observes the active-session marker")
    source_check(results, "session_marker_lifecycle", settings,
                 r"beginSession\(\).*?std::ofstream file\(marker.*?endSession\(\).*?std::filesystem::remove\(",
                 "orderly shutdown removes only the session marker")
    source_check(results, "safe_mode_recovery", app,
                 r"safeMode\s*=\s*launch\.safeMode\s*\|\|\s*previousSessionUnclean",
                 "unclean sessions enter safe mode")
    source_check(results, "safe_mode_flag", launch,
                 r"--safe-mode.*?options\.safeMode\s*=\s*true",
                 "--safe-mode is parsed before backend startup")
    source_check(results, "reset_layout_flag", launch,
                 r"--reset-layout.*?options\.resetLayout\s*=\s*true",
                 "--reset-layout is parsed before backend startup")
    source_check(results, "reset_layout_skips_ini", app,
                 r"io\.IniFilename\s*=\s*\(safeMode\s*\|\|\s*editor::LaunchOptions::get\(\)\.resetLayout\).*?nullptr",
                 "recovery does not parse the persisted ImGui layout")
    source_check(results, "crash_log_rotation", out,
                 r"kMaxLogFileSize\s*=\s*2\s*\*\s*1024\s*\*\s*1024.*?std::filesystem::rename\(path, rotated",
                 "bounded editor logs rotate before append")
    source_check(results, "crash_handler_log_stream", crash,
                 r"openReportFile\(\).*?logPath.*?(FILE_APPEND_DATA|O_APPEND)",
                 "crash reports use the bounded log stream")
    source_check(results, "malformed_settings_isolated", settings,
                 r"YAML::LoadFile\(configFilePath\.string\(\)\).*?catch \(const std::exception& e\).*?settingsData\s*=\s*YAML::Node\(\)",
                 "malformed settings are discarded in memory")
    source_check(results, "recent_projects_bounded", settings,
                 r"recentProjects\.clear\(\).*?std::filesystem::exists\(pathStr\).*?recentProjects\.size\(\)\s*>\s*32",
                 "recent projects are filtered and capped")

    fixture, before = make_fixture()
    run_editor_cmd(results, fixture, find_editor_cmd(root, args.editor_cmd), args.require_editor_cmd)
    results.append({
        "name": "fixture_preserved",
        "status": "PASS" if all(sha256(path) == digest for path, digest in before.items()) else "FAIL",
        "detail": f"recovery fixture preserved at {fixture}",
    })
    failures = [result for result in results if result["status"] == "FAIL"]
    summary = {"status": "FAIL" if failures else "PASS", "fixture": str(fixture),
               "gui_launched": False, "results": results}
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(f"editor stability smoke: {summary['status']}")
        print(f"fixture: {fixture}")
        for result in results:
            print(f"{result['status']:4} {result['name']}: {result['detail']}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
