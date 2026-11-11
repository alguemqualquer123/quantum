# (c) SR VINIX
# SPDX-License-Identifier: MIT
"""Validate incremental FBX import and deep asset verification without the editor GUI."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def executable(build_root: Path, config: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    candidates = (
        build_root / config / f"quantum-editor-cmd{suffix}",
        build_root / f"quantum-editor-cmd{suffix}",
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise RuntimeError(f"missing quantum-editor-cmd under {build_root}")


def run_json(executable_path: Path, *arguments: str) -> tuple[int, dict[str, object], str]:
    completed = subprocess.run(
        [str(executable_path), *arguments],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"command did not return JSON: {completed.stdout!r}; stderr={completed.stderr!r}"
        ) from error
    if not isinstance(payload, dict):
        raise RuntimeError(f"command returned a non-object JSON value: {payload!r}")
    return completed.returncode, payload, completed.stderr


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--editor-executable", type=Path)
    args = parser.parse_args()

    root = args.root.resolve()
    command = args.editor_executable.resolve() if args.editor_executable else executable(
        root / "build", args.config
    )
    source = root / "templates" / "fps-fps"
    require(source.is_dir(), f"missing FBX fixture project: {source}")

    with tempfile.TemporaryDirectory(prefix="quantum-asset-import-") as temporary:
        project = Path(temporary) / "fps-fps"
        shutil.copytree(
            source,
            project,
            ignore=shutil.ignore_patterns(".quantum", "build"),
        )
        source_models = [
            path for path in source.rglob("*.glb")
            if ".quantum" not in path.parts
        ]
        require(source_models, "FBX fixture project has no visible GLB metadata fixture")
        metadata_fixture = project / "assets" / "assetdb-metadata-fixture.glb"
        metadata_fixture.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_models[0], metadata_fixture)

        first_code, first, first_stderr = run_json(
            command, "assets", "import", "--project", str(project), "--json"
        )
        scanned = int(first.get("scanned", 0))
        require(first_code == 0, f"first import failed: {first_stderr}")
        require(scanned > 0, "FBX fixture did not scan any source files")
        require(int(first.get("imported", 0)) == scanned, f"first import: {first}")
        require(int(first.get("failed", 0)) == 0, f"first import failures: {first}")

        second_code, second, second_stderr = run_json(
            command, "assets", "import", "--project", str(project), "--json"
        )
        require(second_code == 0, f"incremental import failed: {second_stderr}")
        require(int(second.get("imported", 0)) == 0, f"incremental import rewrote files: {second}")
        require(int(second.get("upToDate", 0)) == scanned, f"incremental import: {second}")

        imported_models = list((project / ".quantum" / "imported").rglob("*.glb"))
        require(imported_models, "FBX import did not produce a GLB derivative")
        imported_fixture = project / "assets" / "assetdb-fbx-import.glb"
        shutil.copy2(imported_models[0], imported_fixture)

        index_code, index, index_stderr = run_json(
            command, "assets", "index", "--project", str(project), "--json"
        )
        require(index_code == 0, f"asset index failed: {index_stderr}")
        require(int(index.get("brokenCount", 0)) == 0, f"asset index reported broken files: {index}")
        database_path = project / ".quantum" / "assetdb.json"
        metadata_sidecar = project / ".quantum" / "asset-metadata.v1"
        require(metadata_sidecar.is_file(), "asset index did not write importer metadata sidecar")
        require(metadata_sidecar.read_text(encoding="utf-8").splitlines()[0] == "QAMI1",
                "asset metadata sidecar has an unsupported format")
        database = json.loads(database_path.read_text(encoding="utf-8"))
        records = database.get("files", [])
        model_records = [record for record in records if record.get("kind") == "model"]
        require(model_records, "asset index did not persist any model records")
        require(
            any(
                record.get("metadata", {}).get("format") in ("glb", "gltf")
                and "primitiveCount" in record.get("metadata", {})
                and "morphTargetCount" in record.get("metadata", {})
                for record in model_records
            ),
            "asset index did not persist glTF primitive/morph-target counts",
        )
        imported_metadata = [
            record.get("metadata", {})
            for record in model_records
            if record.get("path") == "assets/assetdb-fbx-import.glb"
        ]
        require(imported_metadata, "asset index did not persist the imported FBX derivative")
        imported_record = next(
            record for record in model_records
            if record.get("path") == "assets/assetdb-fbx-import.glb"
        )
        expected_sha1 = hashlib.sha1(imported_fixture.read_bytes()).hexdigest()
        require(
            imported_record.get("sha1", "").lower() == expected_sha1,
            f"asset index hashed imported FBX derivative incorrectly: "
            f"expected={expected_sha1} actual={imported_record.get('sha1', '')}",
        )
        require(
            all(
                key in imported_metadata[0]
                for key in (
                    "importer",
                    "importerVersion",
                    "sourceCount",
                    "linkedSourceCount",
                    "textureCount",
                    "resolvedTextureCount",
                    "missingTextureCount",
                )
            ),
            f"asset index did not persist FBX dependency metadata: {imported_metadata[0]}",
        )

        verify_code, verify, verify_stderr = run_json(
            command, "assets", "verify", "--project", str(project), "--deep", "--json"
        )
        require(verify_code == 0, f"deep asset verify failed: {verify} {verify_stderr}")
        require(bool(verify.get("ok")), f"deep asset verify reported divergence: {verify}")
        require(int(verify.get("deepChecked", 0)) >= scanned, f"deep asset verify was incomplete: {verify}")

        invalid_fixture = project / "assets" / "assetdb-invalid-glb.glb"
        invalid_bytes = bytearray(imported_fixture.read_bytes())
        require(len(invalid_bytes) >= 12, "imported GLB fixture is too short for corruption test")
        invalid_bytes[8:12] = (len(invalid_bytes) - 4).to_bytes(4, "little")
        invalid_fixture.write_bytes(invalid_bytes)
        reindex_code, _, reindex_stderr = run_json(
            command, "assets", "index", "--project", str(project), "--json"
        )
        require(reindex_code == 0, f"reindex after invalid GLB fixture failed: {reindex_stderr}")
        invalid_verify_code, invalid_verify, invalid_verify_stderr = run_json(
            command, "assets", "verify", "--project", str(project), "--deep", "--json"
        )
        require(invalid_verify_code == 1, f"invalid GLB unexpectedly passed deep verify: {invalid_verify}")
        require(
            any(issue.get("path") == "assets/assetdb-invalid-glb.glb" and
                issue.get("type") == "invalid-format" for issue in invalid_verify.get("issues", [])),
            f"deep verify did not report the invalid GLB: {invalid_verify} {invalid_verify_stderr}",
        )

        invalid_fbx = project / "assets" / "assetdb-invalid-fbx.fbx"
        invalid_fbx.write_bytes(b"Kaydara FBX Binary  \x00\x00\x00\x00\x00\x00")
        reindex_code, _, reindex_stderr = run_json(
            command, "assets", "index", "--project", str(project), "--json"
        )
        require(reindex_code == 0, f"reindex after invalid FBX fixture failed: {reindex_stderr}")
        invalid_verify_code, invalid_verify, invalid_verify_stderr = run_json(
            command, "assets", "verify", "--project", str(project), "--deep", "--json"
        )
        require(invalid_verify_code == 1, f"invalid FBX unexpectedly passed deep verify: {invalid_verify}")
        require(
            any(issue.get("path") == "assets/assetdb-invalid-fbx.fbx" and
                issue.get("type") == "invalid-format" for issue in invalid_verify.get("issues", [])),
            f"deep verify did not report the invalid FBX: {invalid_verify} {invalid_verify_stderr}",
        )

        print(
            f"asset import smoke passed: scanned={scanned} imported={first['imported']} "
            f"incremental={second['upToDate']} deepChecked={verify['deepChecked']} "
            "invalidGlb=detected invalidFbx=detected"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"asset import smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1)
