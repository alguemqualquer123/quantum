#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Bake the procedural sky at several hours and render a shader-ball grid under each one.

One run of quantum-engine captures one frame, and a cubemap needs six faces, so a single panel costs
seven launches: six for the dome (demos/demo_cubemap, one face each) and one for the balls
(demos/demo_shaderball). The faces are packed into the horizontal cross the engine reads back
through SkyBox:setTexture, and that cross is what makes the panel reflect anything at all -- under a
customShader dome sky.envMapsLoaded stays false, hasIBL is false, and a metal sphere samples
emptyCubeBlack.

    python scripts/shaderball_sweep.py --project <dir> --hours 6,9,12,15,18,21

Needs two things a fresh project does not ship. The IBL mesh variant, because the runtime never
compiles a shader:

    quantum-resource-compiler.exe shaders --out <project>/assets/shaders --backend opengl \
        --shader "mesh:Puc,Nor,Vc4,Ibl" --shader "mesh:Puc,Nor,Vc4,Ibl,Dlo"

and the library, so the two demos resolve:

    quantum-editor-cmd.exe components install --project <dir>

The baked crosses land in <project>/assets/textures/shaderball/ and are removed again unless --keep
asks for them, because they are derived pixels, not authored assets.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FACES = ("px", "nx", "py", "ny", "pz", "nz")

# Both variants: the plain one lights the balls, the Dlo one is what the depth/low pass asks for
# when a shadow caster is on screen, and a missing one fails the frame the same way.
IBL_SHADERS = (
    "mesh_PucNorVc4Ibl_glsl410.sdat",
    "mesh_PucNorVc4IblDlo_glsl410.sdat",
)


def find_executable(explicit, names, start):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            sys.exit(f"error: no executable at {path}")
        return path
    here = start.resolve()
    for candidate_dir in [here] + list(here.parents):
        for name in names:
            for sub in ("Debug", "Release", ""):
                hit = candidate_dir / "build" / sub / name
                if hit.is_file():
                    return hit
    sys.exit(f"error: could not find {' or '.join(names)} under {here}/build")


def run(cmd, env, log):
    with open(log, "w", encoding="utf-8", errors="replace") as handle:
        return subprocess.call(cmd, env=env, stdout=handle, stderr=subprocess.STDOUT)


def stamp(hour):
    # A helper because Path.replace is os.rename in disguise: writing
    # out / f"balls_{hour}".replace(".", "_") silently tries to rename a file instead of
    # formatting a name.
    return f"{hour:05.1f}".replace(".", "_")


def bake_cube(engine, project, hour, tile, work, preset, moon):
    """Render the dome into six faces and pack them as one cross. Returns the project-relative map."""
    face_dir = work / f"faces_{hour:g}"
    face_dir.mkdir(parents=True, exist_ok=True)
    for face in FACES:
        env = dict(os.environ, CUBE_FACE=face, CUBE_HOUR=f"{hour:g}",
                   CUBE_PRESET=preset, CUBE_MOON=moon)
        out = face_dir / f"{face}.png"
        face_log = face_dir / f"{face}.log"
        code = run([str(engine), "--project", str(project), "--scene", "demos/demo_cubemap",
                    "--window", f"{tile}x{tile}", "--screenshot", str(out),
                    "--capture-frame", "40"], env, face_log)
        if code != 0 or not out.is_file():
            sys.exit(f"error: face {face} at hour {hour:g} failed ({code}), see {face_log}")

    cross = project / "assets" / "textures" / "shaderball" / f"sky_{stamp(hour)}.png"
    cross.parent.mkdir(parents=True, exist_ok=True)
    script = Path(__file__).parent / "assemble_sky_cubemap.py"
    code = subprocess.call([sys.executable, str(script), "assemble", str(face_dir), str(cross),
                            "--tile", str(tile)])
    if code != 0 or not cross.is_file():
        sys.exit(f"error: assembling the cross for hour {hour:g} failed ({code})")
    return cross, "textures/shaderball/" + cross.name


def render_grid(engine, project, map_rel, panel, metal, rough, window, log):
    env = dict(os.environ, BALL_SKYMAP=map_rel, BALL_METAL=metal, BALL_ROUGH=rough)
    code = run([str(engine), "--project", str(project), "--scene", "demos/demo_shaderball",
                "--window", window, "--screenshot", str(panel), "--capture-frame", "60"],
               env, log)
    return code == 0 and panel.is_file()


def stitch(panels, sheet, gutter=8, background=(30, 34, 42)):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow is not installed: the panels are on disk, the sheet was skipped")
        return False

    images = [(hour, Image.open(path).convert("RGB")) for hour, path in panels]
    height = min(image.height for _, image in images)
    scaled = [(hour, image.resize((int(image.width * height / image.height), height)))
              for hour, image in images]
    width = sum(image.width for _, image in scaled) + gutter * (len(scaled) + 1)

    band = 22
    out = Image.new("RGB", (width, height + band), background)
    draw = ImageDraw.Draw(out)
    x = gutter
    for hour, image in scaled:
        out.paste(image, (x, band))
        label = f"{hour:g}h"
        draw.text((x + 2, 5), label, fill=(225, 228, 235))
        x += image.width + gutter
    out.save(sheet)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True, help="a project with the component library installed")
    ap.add_argument("--out", default="shaderball", help="where to write panels and the sheet")
    ap.add_argument("--hours", default="6,9,12,15,18,21", help="comma-separated hours of the day")
    ap.add_argument("--metal", default="0,1", help="comma-separated metallicFactor per row")
    ap.add_argument("--rough", default="0,0.25,0.5,0.75,1", help="comma-separated roughnessFactor")
    ap.add_argument("--preset", default="few", help="clouds preset: clear few overcast storm")
    ap.add_argument("--moon", default="0.5", help="moon phase, 0 to 1")
    ap.add_argument("--tile", type=int, default=512, help="edge of one cubemap face")
    ap.add_argument("--window", default="1280x720", help="WxH of one panel")
    ap.add_argument("--engine", help="path to quantum-engine.exe (auto-detected otherwise)")
    ap.add_argument("--compiler", help="path to quantum-resource-compiler.exe (auto-detected)")
    ap.add_argument("--keep", action="store_true", help="leave the baked crosses in the project")
    args = ap.parse_args()

    project = Path(args.project).resolve()
    if not (project / "project.yaml").is_file():
        sys.exit(f"error: {project} has no project.yaml")
    for demo in ("demo_cubemap.lua", "demo_shaderball.lua"):
        if not (project / "lua" / "demos" / demo).is_file():
            sys.exit(f"error: {project}/lua/demos/{demo} is missing. Run "
                     f"quantum-editor-cmd components install --project {project}")

    engine = find_executable(args.engine, ("quantum-engine.exe", "quantum-engine"), project)
    compiler = find_executable(args.compiler,
                               ("quantum-resource-compiler.exe", "quantum-resource-compiler"), project)

    missing = [name for name in IBL_SHADERS
               if not (project / "assets" / "shaders" / name).is_file()]
    if missing:
        print(f"error: {project}/assets/shaders is missing {', '.join(missing)}.\n"
              "The runtime never compiles a shader, and without the IBL mesh variant every metal\n"
              "sphere renders black. Generate them with:\n"
              f'  "{compiler}" shaders --out "{project / "assets" / "shaders"}" '
              '--backend opengl --shader "mesh:Puc,Nor,Vc4,Ibl" '
              '--shader "mesh:Puc,Nor,Vc4,Ibl,Dlo"')
        return 2

    hours = [float(token) for token in args.hours.split(",") if token.strip()]
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    crosses = []
    panels = []

    with tempfile.TemporaryDirectory(prefix="shaderball-") as tmp:
        work = Path(tmp)
        for hour in hours:
            cross, map_rel = bake_cube(engine, project, hour, args.tile, work,
                                       args.preset, args.moon)
            crosses.append(cross)
            panel = out / f"balls_{stamp(hour)}.png"
            panel_log = out / f"balls_{stamp(hour)}.log"
            ok = render_grid(engine, project, map_rel, panel, args.metal, args.rough,
                             args.window, panel_log)
            if not ok:
                print(f"hour {hour:g}: the panel did not render, see {panel_log}")
                return 1
            print(f"hour {hour:g}: {panel.name}")
            panels.append((hour, panel))

        sheet = out / f"shaderball_{args.preset}.png"
        if stitch(panels, sheet):
            print(f"sheet: {sheet}")

    if not args.keep:
        for cross in crosses:
            cross.unlink(missing_ok=True)
        stage = project / "assets" / "textures" / "shaderball"
        if stage.is_dir() and not any(stage.iterdir()):
            stage.rmdir()
    return 0


if __name__ == "__main__":
    sys.exit(main())
