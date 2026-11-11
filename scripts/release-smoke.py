#!/usr/bin/env python3
# (c) SR VINIX
# SPDX-License-Identifier: MIT

"""Validate an installed or packaged build without opening a GUI."""

from __future__ import annotations

import argparse
import hashlib
import json
import plistlib
import subprocess
import sys
import zipfile
from pathlib import Path


MANIFEST_PLATFORMS = {"windows", "linux", "macos", "android", "ios", "web"}
MANIFEST_BACKENDS = {"glcore", "vulkan", "metal", "gles3"}
MANIFEST_BUILD_TYPES = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}


def validate_manifest(data: object, path: Path) -> dict[str, object]:
    if not isinstance(data, dict):
        raise RuntimeError(f"release manifest must be a JSON object: {path}")
    required = ("schema", "product", "version", "channel", "platform",
                "architecture", "graphicBackend", "buildType")
    missing = [key for key in required if key not in data or data[key] in (None, "")]
    if missing:
        raise RuntimeError(f"release manifest is missing {', '.join(missing)}")
    if data["schema"] != 1 or isinstance(data["schema"], bool):
        raise RuntimeError(f"unsupported release manifest schema: {data['schema']}")
    for key in ("product", "version", "channel", "platform", "architecture",
                "graphicBackend", "buildType"):
        if not isinstance(data[key], str):
            raise RuntimeError(f"release manifest field {key} must be a string")
    if data["product"] != "Quantum":
        raise RuntimeError(f"unexpected release product: {data['product']}")
    if data["platform"] not in MANIFEST_PLATFORMS:
        raise RuntimeError(f"unsupported release platform: {data['platform']}")
    if data["graphicBackend"] not in MANIFEST_BACKENDS:
        raise RuntimeError(f"unsupported release graphic backend: {data['graphicBackend']}")
    if data["buildType"] not in MANIFEST_BUILD_TYPES:
        raise RuntimeError(f"unsupported release build type: {data['buildType']}")
    expected_backend = {
        "android": "gles3",
        "ios": "metal",
        "macos": "metal",
        "web": "gles3",
    }.get(data["platform"])
    if expected_backend and data["graphicBackend"] != expected_backend:
        raise RuntimeError(
            f"release platform {data['platform']} requires graphic backend {expected_backend}, "
            f"got {data['graphicBackend']}"
        )
    if data["platform"] in {"windows", "linux"} and data["graphicBackend"] not in {"glcore", "vulkan"}:
        raise RuntimeError(
            f"desktop platform {data['platform']} requires glcore or vulkan, "
            f"got {data['graphicBackend']}"
        )
    return data


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_file_format(path: Path) -> dict[str, object]:
    suffix = path.suffix.lower()
    checks: dict[str, object] = {}
    if suffix in {".apk", ".aab"}:
        try:
            with zipfile.ZipFile(path) as archive:
                broken = archive.testzip()
                names = set(archive.namelist())
        except (OSError, zipfile.BadZipFile) as exc:
            raise RuntimeError(f"invalid Android archive {path}: {exc}") from exc
        if broken is not None:
            raise RuntimeError(f"corrupt Android archive entry in {path}: {broken}")
        if "AndroidManifest.xml" not in names:
            raise RuntimeError(f"Android archive has no AndroidManifest.xml: {path}")
        checks = {
            "format": "zip",
            "zipEntries": len(names),
            "hasAndroidManifest": True,
        }
    elif suffix == ".ipa":
        try:
            with zipfile.ZipFile(path) as archive:
                broken = archive.testzip()
                names = set(archive.namelist())
                plistNames = sorted(
                    name for name in names
                    if name.startswith("Payload/")
                    and name.endswith(".app/Info.plist")
                )
                if broken is not None:
                    raise RuntimeError(f"corrupt iOS archive entry: {broken}")
                if len(plistNames) != 1:
                    raise RuntimeError(
                        f"iOS archive must contain exactly one Payload app Info.plist: {path}"
                    )
                plist = plistlib.loads(archive.read(plistNames[0]))
                bundleId = plist.get("CFBundleIdentifier")
                bundleExecutable = plist.get("CFBundleExecutable")
                if not isinstance(bundleId, str) or not bundleId:
                    raise RuntimeError(f"iOS archive plist has no CFBundleIdentifier: {path}")
                if not isinstance(bundleExecutable, str) or not bundleExecutable:
                    raise RuntimeError(f"iOS archive plist has no CFBundleExecutable: {path}")
                appPrefix = plistNames[0][:-len("Info.plist")]
                executableName = appPrefix + bundleExecutable
                if executableName not in names:
                    raise RuntimeError(f"iOS archive is missing bundle executable: {executableName}")
        except (OSError, KeyError, TypeError, ValueError, plistlib.InvalidFileException,
                zipfile.BadZipFile) as exc:
            raise RuntimeError(f"invalid iOS archive {path}: {exc}") from exc
        checks = {
            "format": "ipa",
            "zipEntries": len(names),
            "hasAppInfoPlist": True,
            "bundleIdentifier": bundleId,
            "bundleExecutable": bundleExecutable,
        }
    elif suffix == ".wasm":
        with path.open("rb") as stream:
            header = stream.read(8)
        if header[:4] != b"\x00asm" or header[4:] != b"\x01\x00\x00\x00":
            raise RuntimeError(f"invalid WebAssembly header/version: {path}")
        checks = {"format": "wasm", "wasmVersion": 1}
    return checks


def artifact_info(path: Path) -> dict[str, object]:
    path = path.resolve()
    if not path.exists():
        raise RuntimeError(f"missing artifact: {path}")
    if path.is_file():
        result: dict[str, object] = {
            "path": path.as_posix(),
            "kind": "file",
            "size": path.stat().st_size,
            "sha256": sha256(path),
        }
        result.update(validate_file_format(path))
        return result
    if not path.is_dir():
        raise RuntimeError(f"unsupported artifact type: {path}")

    files = []
    for child in sorted(path.rglob("*")):
        if child.is_file():
            relative = child.relative_to(path).as_posix()
            files.append({
                "path": relative,
                "size": child.stat().st_size,
                "sha256": sha256(child),
            })
    if not files:
        raise RuntimeError(f"artifact directory is empty: {path}")
    result = {
        "path": path.as_posix(),
        "kind": "directory",
        "fileCount": len(files),
        "size": sum(item["size"] for item in files),
        "files": files,
    }
    if path.suffix.lower() == ".app":
        plist = path / "Info.plist"
        if not plist.is_file():
            raise RuntimeError(f"Apple app bundle has no Info.plist: {path}")
        try:
            metadata = plistlib.loads(plist.read_bytes())
        except (OSError, plistlib.InvalidFileException, ValueError) as exc:
            raise RuntimeError(f"Apple app bundle has invalid Info.plist: {path}: {exc}") from exc
        bundleId = metadata.get("CFBundleIdentifier")
        bundleExecutable = metadata.get("CFBundleExecutable")
        if not isinstance(bundleId, str) or not bundleId:
            raise RuntimeError(f"Apple app bundle has no CFBundleIdentifier: {path}")
        if not isinstance(bundleExecutable, str) or not bundleExecutable:
            raise RuntimeError(f"Apple app bundle has no CFBundleExecutable: {path}")
        if Path(bundleExecutable).name != bundleExecutable or ".." in Path(bundleExecutable).parts:
            raise RuntimeError(f"Apple app bundle has unsafe CFBundleExecutable: {path}")
        executable = path / bundleExecutable
        if not executable.is_file() or executable.stat().st_size == 0:
            raise RuntimeError(f"Apple app bundle executable is missing or empty: {executable}")
        result["format"] = "apple-app-bundle"
        result["infoPlist"] = "Info.plist"
        result["bundleIdentifier"] = bundleId
        result["bundleExecutable"] = bundleExecutable
    return result


def run_help(executable: Path) -> dict[str, object]:
    completed = subprocess.run(
        [str(executable), "--help"],
        cwd=executable.parent,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=30,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"{executable.name} --help exited with {completed.returncode}\n{completed.stdout[-2000:]}")
    if not completed.stdout.strip():
        raise RuntimeError(f"{executable.name} --help produced no output")
    print(f"smoke passed: {executable.name} --help")
    result = artifact_info(executable)
    result["helpExitCode"] = completed.returncode
    return result


def run_headless(executable: Path, arguments: list[str], label: str) -> dict[str, object]:
    try:
        completed = subprocess.run(
            [str(executable), *arguments],
            cwd=executable.parent,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=180,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"{label} timed out after 180 seconds") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            f"{label} exited with {completed.returncode}\n{completed.stdout[-4000:]}"
        )
    if not completed.stdout.strip():
        raise RuntimeError(f"{label} produced no output")
    print(f"smoke passed: {label}")
    return {
        "label": label,
        "exitCode": completed.returncode,
        "outputTail": completed.stdout[-2000:],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--executable", type=Path, default=None)
    parser.add_argument("--editor-cmd", type=Path, default=None,
                        help="Validate the headless editor command executable with --help.")
    parser.add_argument("--relay-executable", type=Path, default=None,
                        help="Validate the installed authenticated UDP relay with --help.")
    parser.add_argument("--resource-server", type=Path, default=None,
                        help="Validate the installed resource server with --help.")
    parser.add_argument("--runtime-project", type=Path, default=None,
                        help="Run the installed engine headlessly against this project.")
    parser.add_argument("--resource-project", type=Path, default=None,
                        help="Run the installed resource server once against this project.")
    parser.add_argument("--resource-compiler", type=Path, default=None,
                        help="Validate the installed headless resource compiler with --help.")
    parser.add_argument("--build-generator", type=Path, default=None,
                        help="Validate the installed headless build generator with --help.")
    parser.add_argument("--artifact", type=Path, action="append", default=[],
                        help="Verify an additional file or directory (repeatable).")
    parser.add_argument("--report", type=Path, default=None,
                        help="Write a JSON report with hashes of verified artifacts.")
    args = parser.parse_args()
    if args.runtime_project and not args.executable:
        parser.error("--runtime-project requires --executable")
    if args.resource_project and not args.resource_server:
        parser.error("--resource-project requires --resource-server")
    root = args.root.resolve()
    manifest = (args.manifest or root / "release-manifest.json").resolve()
    if not manifest.is_file():
        raise RuntimeError(f"missing release-manifest.json: {manifest}")
    data = validate_manifest(json.loads(manifest.read_text(encoding="utf-8")), manifest)
    print(f"manifest passed: {data['product']} {data['version']} ({data['platform']}/{data['architecture']})")
    artifacts = []
    runtime_smokes = []
    if args.executable:
        executable = args.executable.resolve()
        if not executable.is_file():
            raise RuntimeError(f"missing executable: {executable}")
        artifacts.append(run_help(executable))
        if args.runtime_project:
            runtime_project = args.runtime_project.resolve()
            if not runtime_project.is_dir():
                raise RuntimeError(f"missing runtime project: {runtime_project}")
            runtime_smokes.append(run_headless(
                executable,
                ["--project", str(runtime_project), "--headless", "--script-smoke",
                 "--graph-smoke", "--graph-project-smoke", "--frames", "1", "--warmup", "0"],
                f"{executable.name} installed headless runtime",
            ))
    for tool, label in ((args.editor_cmd, "editor command"),
                        (args.resource_server, "resource server")):
        if tool:
            executable = tool.resolve()
            if not executable.is_file():
                raise RuntimeError(f"missing {label} executable: {executable}")
            artifacts.append(run_help(executable))
            if label == "resource server" and args.resource_project:
                resource_project = args.resource_project.resolve()
                if not resource_project.is_dir():
                    raise RuntimeError(f"missing resource project: {resource_project}")
                runtime_smokes.append(run_headless(
                    executable,
                    ["--project", str(resource_project), "--port", "0", "--once", "--no-window"],
                    f"{executable.name} installed resource index",
                ))
    if args.relay_executable:
        relay_executable = args.relay_executable.resolve()
        if not relay_executable.is_file():
            raise RuntimeError(f"missing relay executable: {relay_executable}")
        artifacts.append(run_help(relay_executable))
    for tool in (args.resource_compiler, args.build_generator):
        if tool:
            executable = tool.resolve()
            if not executable.is_file():
                raise RuntimeError(f"missing CLI executable: {executable}")
            artifacts.append(run_help(executable))
    for artifact in args.artifact:
        artifacts.append(artifact_info(artifact))

    report = {
        "schema": 1,
        "manifest": manifest.as_posix(),
        "product": data["product"],
        "version": data["version"],
        "platform": data["platform"],
        "architecture": data["architecture"],
        "artifacts": artifacts,
        "runtimeSmokes": runtime_smokes,
    }
    report_path = (args.report or root / "release-smoke-report.json").resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"report passed: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"release smoke failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
