# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Rewrite the inline boxes whose UV spans ignore the face's own world extent.

A unit box scaled to 40 x 1 x 40 with every face carrying UV 0..25 repeats the ground
texture 25 times across 40 m on the top - right - and 25 times across 1 m on the four
sides, which is a 40x vertical squash of the same map on the skirt the camera sees at
the horizon. The engine has no texture-scale key on Material, so the only fix is the UV
attribute itself.

Each face keeps the top face's density (metres per repeat); the axis that a face's u or
v follows is the one createBox uses, transcribed in scripts/gen_primitive_yaml.py.

    python scripts/fix_ground_tiling.py            # rewrite the three Ground meshes
"""

import base64
import math
import os
import struct
import sys

import yaml

TARGETS = [
    "templates/fps-fps/scenes/main.scene",
    "templates/third-person/scenes/main.scene",
    "templates/moba-click/scenes/main.scene",
]

# Which world axis u and v follow for each axis-aligned normal, per MeshSystem::createBox.
FACE_AXES = {
    (0.0, 0.0, 1.0): (0, 1), (0.0, 0.0, -1.0): (0, 1),
    (1.0, 0.0, 0.0): (2, 1), (-1.0, 0.0, 0.0): (2, 1),
    (0.0, 1.0, 0.0): (0, 2), (0.0, -1.0, 0.0): (0, 2),
}


def face_key(normal):
    return tuple(round(c) for c in normal[:3])


def repair(path, write):
    doc = yaml.safe_load(open(path, encoding="utf-8").read())
    report = []
    patches = []

    def visit(entities):
        for e in entities or []:
            if not isinstance(e, dict):
                continue
            comps = e.get("components") or {}
            mesh = comps.get("mesh")
            if e.get("name") == "Ground" and isinstance(mesh, dict) and "buffer" in mesh:
                buf = mesh["buffer"]
                stride, count = buf["stride"], buf["count"]
                raw = bytearray(base64.b64decode(buf["data"]))
                verts = [struct.unpack_from("<8f", raw, i * stride) for i in range(count)]

                # The +Y face sets the density: it is the surface the camera reads as ground.
                top = [v for v in verts if face_key(v[5:]) == (0.0, 1.0, 0.0)]
                ext = lambda vs, a: max(v[a] for v in vs) - min(v[a] for v in vs)
                du = ext(top, 3) or 1.0
                dv = ext(top, 4) or 1.0
                sx, sy, sz = comps["transform"].get("scale", [1, 1, 1])
                area = (ext(top, 0) * abs(sx)) * (ext(top, 2) * abs(sz))
                tile = math.sqrt(area / (du * dv))

                changed = 0
                for i, v in enumerate(verts):
                    axes = FACE_AXES.get(face_key(v[5:]))
                    if axes is None:
                        continue
                    # Keep each corner's 0/1 position in the face and only re-scale the span.
                    for slot, axis in ((3, axes[0]), (4, axes[1])):
                        if v[slot] == 0.0:
                            continue
                        want = (ext([q for q in verts if face_key(q[5:]) == face_key(v[5:])], axis)
                                * abs((sx, sy, sz)[axis])) / tile
                        got = ext([q for q in verts if face_key(q[5:]) == face_key(v[5:])], slot)
                        if abs(want - got) > 1e-4:
                            struct.pack_into("<f", raw, i * stride + slot * 4, want)
                            changed += 1
                if changed:
                    patches.append((buf["data"], base64.b64encode(bytes(raw)).decode("ascii")))
                    report.append(f"{os.path.relpath(path)}: Ground at {tile:.2f} m per tile, "
                                  f"{changed} UV components rewritten")
            visit(e.get("children"))

    visit(doc.get("entities"))

    if not write or not patches:
        return report
    text = open(path, encoding="utf-8").read()
    for old, new in patches:
        if text.count(old) != 1:
            sys.exit(f"{path}: buffer data appears {text.count(old)} times, refusing to guess")
        text = text.replace(old, new)
    open(path, "w", encoding="utf-8", newline="\r\n").write(text)
    return report


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    write = "--write" in sys.argv
    for p in TARGETS:
        for line in repair(p, write):
            print(("[write] " if write else "[dry run] ") + line)
