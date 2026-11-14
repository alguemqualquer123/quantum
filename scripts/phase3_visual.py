# (c) SR VINIX
# SPDX-License-Identifier: MIT
"""Capture and compare Quantum Phase 3 render baselines.

The script deliberately keeps all generated files outside the repository. It compiles the
requested shader set into a disposable copy of the selected project, captures a real presented
frame through quantum-engine, and asks the runner to emit the pixel comparison report/diff.

Example (OpenGL runtime plus shader-only Vulkan coverage):
    python scripts/phase3_visual.py --project tests/scale/probe \
        --engine build/Debug/quantum-engine.exe \
        --resource-compiler build/Debug/quantum-resource-compiler.exe \
        --backend opengl --backend vulkan --update-baselines

For a second runtime backend pass an explicit mapping, e.g.:
    --engine-map vulkan=build-vulkan/Debug/quantum-engine.exe

The current checkout's policy is one build tree, so the script never configures or builds a
backend. A backend without a mapped runtime executable is reported as shader-only instead of
silently presenting an image from the wrong graphics API.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


FEATURES = (
    "taa",
    "motionblur",
    "hdr",
    "lut",
    "lutcinematic",
    "clustered",
    "ssgi",
    "ssr",
    "ssrfallback",
    "decals",
    "atmosphere",
    "shadows",
)

SHADER_SPECS = (
    "sky",
    "blit",
    "depth",
    "gbuffer:Nor",
    "mesh",
    "mesh:Puc,Nor,Vc4",
    "mesh:Puc,Shw,Nor,Vc4,Fog,Dlo",
    "mesh:Puc,Shw,Nor,Vc4,Aop",
    "mesh:Puc,Shw,Nor,Vc4,Aop,Dlo",
    # Runtime-authored fixtures may use Shape's unlit vertex-color material.
    # Keep this in the matrix so a Lua-built scene is captureable without relying
    # on a shader cache copied from an unrelated project.
    "mesh:Ult,Vc4",
    "tonemap",
    "exposure",
    "bloomthreshold",
    "bloomdownsample",
    "bloomupsample",
    "bloomcomposite",
    "velocity",
    "taa",
    "motionblur",
    "ssao",
    "ssaoblur",
    "ssr",
    "ssrblur",
    "composite",
    "ssgi",
    "decal",
    "atmosphere",
    "fxaa",
)


def fail(message: str) -> int:
    print(f"phase3-visual: error: {message}", file=sys.stderr)
    return 2


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        rows.extend(rgba[y * stride:(y + 1) * stride])
    payload = (b"\x89PNG\r\n\x1a\n"
               + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
               + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
               + png_chunk(b"IEND", b""))
    path.write_bytes(payload)


def make_identity_lut(path: Path, size: int = 16) -> None:
    """Write the strip layout accepted by RenderSystem's 2D-array LUT loader."""
    width = size * size
    rgba = bytearray(width * size * 4)
    for blue in range(size):
        for green in range(size):
            for red in range(size):
                x = blue * size + red
                y = green
                index = (y * width + x) * 4
                rgba[index:index + 4] = bytes((
                    round(red * 255 / (size - 1)),
                    round(green * 255 / (size - 1)),
                    round(blue * 255 / (size - 1)),
                    255,
                ))
    write_png(path, width, size, bytes(rgba))


def make_cinematic_lut(path: Path, size: int = 16) -> None:
    """Write a deterministic warm/highlight-compressed LUT for perceptual coverage."""
    width = size * size
    rgba = bytearray(width * size * 4)

    def encode(value: float) -> int:
        return max(0, min(255, round(value * 255)))

    for blue in range(size):
        for green in range(size):
            for red in range(size):
                r = red / (size - 1)
                g = green / (size - 1)
                b = blue / (size - 1)
                # The curve is intentionally small but non-identity: warm shadows,
                # restrained blue highlights and a soft shoulder make a failed LUT
                # binding visible without depending on a particular GPU or scene.
                shoulder = lambda value: value / (value + 0.22) * 1.22
                out_r = min(1.0, shoulder(r) * 1.02 + 0.018 * (1.0 - r))
                out_g = min(1.0, shoulder(g) * 0.98 + 0.006 * (1.0 - g))
                out_b = min(1.0, shoulder(b) * 0.86)
                x = blue * size + red
                index = (green * width + x) * 4
                rgba[index:index + 4] = bytes((
                    encode(out_r), encode(out_g), encode(out_b), 255,
                ))
    write_png(path, width, size, bytes(rgba))


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    offset = 8
    width = height = bit_depth = color_type = None
    compressed = bytearray()
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload)
            if bit_depth != 8 or compression != 0 or filtering != 0 or interlace != 0:
                raise ValueError(f"unsupported PNG layout in {path}")
            if color_type not in (2, 6):
                raise ValueError(f"expected RGB/RGBA PNG in {path}")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    if width is None or height is None:
        raise ValueError(f"PNG has no header: {path}")
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(compressed))
    source_stride = width * channels
    expected = (source_stride + 1) * height
    if len(raw) != expected:
        raise ValueError(f"unexpected PNG payload size in {path}")
    result = bytearray(width * height * 4)
    previous = bytearray(source_stride)
    cursor = 0
    for y in range(height):
        filter_type = raw[cursor]
        cursor += 1
        current = bytearray(raw[cursor:cursor + source_stride])
        cursor += source_stride
        for x in range(source_stride):
            left = current[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if filter_type == 1:
                current[x] = (current[x] + left) & 0xFF
            elif filter_type == 2:
                current[x] = (current[x] + up) & 0xFF
            elif filter_type == 3:
                current[x] = (current[x] + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:
                estimate = left + up - up_left
                pa = abs(estimate - left)
                pb = abs(estimate - up)
                pc = abs(estimate - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                current[x] = (current[x] + predictor) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type} in {path}")
        for x in range(width):
            source = x * channels
            target = (y * width + x) * 4
            result[target:target + 3] = current[source:source + 3]
            result[target + 3] = current[source + 3] if channels == 4 else 255
        previous = current
    return width, height, bytes(result)


def local_compare(current: Path, baseline: Path, diff: Path, threshold: float) -> dict:
    width, height, current_rgba = read_png(current)
    base_width, base_height, baseline_rgba = read_png(baseline)
    if (width, height) != (base_width, base_height):
        raise ValueError(f"dimensions differ: current={width}x{height} baseline={base_width}x{base_height}")
    output = bytearray(len(current_rgba))
    total = 0
    maximum = 0
    differing = 0
    for i in range(0, len(current_rgba), 4):
        delta = [abs(current_rgba[i + c] - baseline_rgba[i + c]) for c in range(3)]
        error = sum(delta) / (3.0 * 255.0)
        total += sum(delta)
        maximum = max(maximum, max(delta))
        if error > threshold:
            differing += 1
        output[i:i + 4] = bytes((min(255, delta[0] * 4), min(255, delta[1] * 4),
                                 min(255, delta[2] * 4), 255))
    pixels = width * height
    metrics = {
        "width": width,
        "height": height,
        "meanError": total / (pixels * 3.0 * 255.0) if pixels else 0.0,
        "maximumError": maximum / 255.0,
        "differingPixels": differing,
        "differingRatio": differing / pixels if pixels else 0.0,
        "threshold": threshold,
        "passed": total / (pixels * 3.0 * 255.0) <= threshold if pixels else True,
    }
    write_png(diff, width, height, bytes(output))
    return metrics


def run(command: list[str], log: Path, env: dict[str, str] | None = None) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        completed = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                   env=env, cwd=Path(__file__).resolve().parents[1])
    return completed.returncode


def parse_engine_map(values: list[str]) -> dict[str, Path]:
    mapping: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--engine-map expects backend=path, got '{value}'")
        backend, path = value.split("=", 1)
        mapping[backend.strip().lower()] = Path(path).resolve()
    return mapping


def backend_name(value: str) -> str:
    value = value.lower().strip()
    return {"glcore": "opengl", "gles3": "opengles", "metal": "metal-macos"}.get(value, value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--resource-compiler", type=Path, required=True)
    parser.add_argument("--backend", action="append", default=[],
                        help="shader backend; repeat (opengl, opengles, d3d11, metal-macos, metal-ios, vulkan)")
    parser.add_argument("--feature", action="append", dest="features",
                        help="feature to capture; repeat, default is the full Phase 3 list")
    parser.add_argument("--engine-map", action="append", default=[],
                        help="runtime backend mapping, e.g. vulkan=build-vulkan/Debug/quantum-engine.exe")
    parser.add_argument("--artifact-dir", type=Path,
                        default=Path(tempfile.gettempdir()) / "quantum-phase3-visual")
    parser.add_argument("--baseline-dir", type=Path)
    parser.add_argument("--window", default="640x360")
    parser.add_argument("--capture-frame", type=int, default=60)
    parser.add_argument("--threshold", type=float, default=0.01)
    parser.add_argument("--update-baselines", action="store_true")
    parser.add_argument("--skip-shader-compile", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    project = args.project.resolve()
    engine = args.engine.resolve()
    compiler = args.resource_compiler.resolve()
    if not project.is_dir(): return fail(f"project not found: {project}")
    if not engine.is_file(): return fail(f"engine executable not found: {engine}")
    if not compiler.is_file(): return fail(f"resource compiler not found: {compiler}")
    if not (1 <= args.capture_frame): return fail("--capture-frame must be positive")
    if not (0.0 <= args.threshold <= 1.0): return fail("--threshold must be between 0 and 1")

    try:
        engine_map = parse_engine_map(args.engine_map)
    except ValueError as error:
        return fail(str(error))
    selected_features = args.features or list(FEATURES)
    selected_features = [f.lower().replace("-", "") for f in selected_features]
    unknown = sorted(set(selected_features) - set(FEATURES))
    if unknown: return fail("unknown feature(s): " + ", ".join(unknown))
    backends = []
    for value in (args.backend or ["opengl"]):
        name = backend_name(value)
        if name not in backends: backends.append(name)
    baseline_dir = (args.baseline_dir or args.artifact_dir / "baselines").resolve()
    artifact_dir = args.artifact_dir.resolve()
    artifact_dir.mkdir(parents=True, exist_ok=True)

    report = {
        "tool": "scripts/phase3_visual.py",
        "project": str(project),
        "window": args.window,
        "captureFrame": args.capture_frame,
        "threshold": args.threshold,
        "artifacts": str(artifact_dir),
        "backends": backends,
        "features": selected_features,
        "runs": [],
    }
    all_passed = True

    for backend in backends:
        backend_root = artifact_dir / backend
        backend_project = backend_root / "project"
        if not backend_project.exists():
            shutil.copytree(project, backend_project)
        make_identity_lut(backend_project / "assets" / "textures" / "phase3_identity_lut.png")
        if "lutcinematic" in selected_features:
            make_cinematic_lut(backend_project / "assets" / "textures" / "phase3_cinematic_lut.png")

        shader_log = backend_root / "shader-compile.log"
        shader_result = {"backend": backend, "returnCode": 0, "log": str(shader_log)}
        if not args.skip_shader_compile:
            command = [str(compiler), "shaders", "--out", str((backend_project / "assets" / "shaders").resolve()),
                       "--backend", backend]
            for spec in SHADER_SPECS:
                command.extend(["--shader", spec])
            shader_result["returnCode"] = run(command, shader_log)
            if shader_result["returnCode"] != 0:
                all_passed = False
        report.setdefault("shaderCompiles", []).append(shader_result)

        runtime = engine_map.get(backend)
        if runtime is None and backend == "opengl": runtime = engine
        if runtime is None:
            print(f"phase3-visual: {backend}: shader matrix complete; runtime skipped (no --engine-map)")
            for feature in selected_features:
                report["runs"].append({"backend": backend, "feature": feature,
                                       "status": "shader-only", "shaderCompile": shader_result["returnCode"]})
            continue
        if not runtime.is_file():
            all_passed = False
            report["runs"].append({"backend": backend, "status": "runtime-missing", "engine": str(runtime)})
            continue

        for feature in selected_features:
            case_root = backend_root / feature
            image = case_root / "current.png"
            stats = case_root / "stats.json"
            log = case_root / "engine.log"
            baseline = baseline_dir / backend / f"{feature}.png"
            diff = case_root / "diff.png"
            compare_report = case_root / "compare.json"
            command = [str(runtime), "--project", str(backend_project), "--window", args.window,
                       "--screenshot", str(image), "--capture-frame", str(args.capture_frame),
                       "--stats-out", str(stats), "--render-feature", feature,
                       "--log-level", "1"]
            if feature == "lut":
                command.extend(["--render-lut", "textures/phase3_identity_lut.png"])
            elif feature == "lutcinematic":
                command.extend(["--render-lut", "textures/phase3_cinematic_lut.png"])
            if baseline.exists():
                command.extend(["--compare-screenshot", str(baseline), "--compare-threshold", str(args.threshold),
                                "--compare-diff", str(diff), "--compare-report", str(compare_report)])
            return_code = run(command, log)
            row = {"backend": backend, "feature": feature, "returnCode": return_code,
                   "image": str(image), "stats": str(stats), "log": str(log),
                   "baseline": str(baseline), "compareReport": str(compare_report)}
            if not image.is_file():
                row["status"] = "capture-failed"
                all_passed = False
            elif not baseline.exists():
                if return_code != 0:
                    row["status"] = "capture-failed"
                    all_passed = False
                elif args.update_baselines:
                    baseline.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(image, baseline)
                    row["status"] = "baseline-created"
                else:
                    row["status"] = "baseline-missing"
                    all_passed = False
            else:
                try:
                    metrics = json.loads(compare_report.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError, FileNotFoundError):
                    metrics = local_compare(image, baseline, diff, args.threshold)
                row["metrics"] = metrics
                row["status"] = "pass" if metrics.get("passed", metrics.get("meanError", 1.0) <= args.threshold) else "diff"
                if row["status"] != "pass": all_passed = False
            report["runs"].append(row)
            print(f"phase3-visual: {backend}/{feature}: {row['status']}")

    report_path = artifact_dir / "phase3-visual-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    html_rows = []
    for row in report["runs"]:
        html_rows.append("<tr>" + "".join(f"<td>{str(row.get(k, ''))}</td>"
                         for k in ("backend", "feature", "status", "returnCode")) + "</tr>")
    (artifact_dir / "phase3-visual-report.html").write_text(
        "<!doctype html><meta charset=utf-8><title>Quantum Phase 3 visual validation</title>"
        "<h1>Quantum Phase 3 visual validation</h1><table border=1>"
        "<tr><th>Backend</th><th>Feature</th><th>Status</th><th>Exit</th></tr>"
        + "".join(html_rows) + "</table>\n", encoding="utf-8")
    print(f"phase3-visual: report {report_path}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
