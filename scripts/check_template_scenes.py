#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Static health check for the template scenes - no GL context and no editor needed.

Every finding here is a scene that loads without an error and shows nothing:

  dead mesh     `mesh: {meshType: box}` is not a key decodeMeshComponent reads
                (editor/Stream.cpp looks for buffer/indices/submeshes), so the entity
                loads an empty mesh. Real geometry has to be written into the file.
  wrong parent  the same geometry under `transform:` instead of `mesh:` parses happily
                and decodes nothing, which is invisible to the check above - so every
                buffer/indices/submeshes node is decoded and its parent is verified.
  mainCamera    is an entity id and the draw path looks up CameraComponent by it, so
                pointing it at an entity without a camera leaves the scene with no view.
  outside clip  a mesh centre whose view-space z falls outside [near, far] is clipped.
                The projection is rebuilt from Matrix4::orthoMatrix/perspectiveMatrix
                exactly as updateCamera does, including the useTarget branch.
  stretched map the UV span is per-face, not per-metre: a 40 x 1 x 40 board with UV 0..25
                on every face repeats the ground map every 1.6 m on top and every 0.04 m
                on the skirt. The density of every face of a box has to agree.
  entity ids    duplicates break the bundle mask and the outliner.

    python scripts/check_template_scenes.py [templates-dir]
"""

import glob
import math
import os
import sys
import yaml


def walk(entities, out):
    for e in entities or []:
        if not isinstance(e, dict):
            continue
        comps = e.get("components") or {}
        out.append({"id": e.get("entity"), "name": e.get("name"), "comps": comps})
        walk(e.get("children"), out)


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def norm(v):
    d = math.sqrt(sum(c * c for c in v))
    return [c / d for c in v] if d else v


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def view_z(point, camera, cam_transform):
    """(z of `point` in camera space, exact?) - negative is in front, the engine's -Z forward."""
    pos = [float(c) for c in cam_transform["position"]]
    if camera.get("useTarget"):
        target = [float(c) for c in camera.get("target", [0, 0, 0])]
        forward = norm(sub(pos, target))
        return dot(sub(point, pos), forward), True
    # A non-target camera views along its own rotation. The 2D templates all carry the
    # identity quaternion, so the plain z difference is exact there and only approximate
    # elsewhere - say so rather than pretending to a number.
    rot = [float(c) for c in cam_transform.get("rotation", [1, 0, 0, 0])]
    identity = max(abs(x) for x in rot[1:]) < 1e-6 and abs(rot[0] - 1.0) < 1e-6
    return point[2] - pos[2], identity


def clip_z_ortho(vz, near, far):
    return vz * (-2.0 / (far - near)) - (far + near) / (far - near)


def clip_z_persp(vz, near, far):
    return (vz * (-((far + near) / (far - near))) - (2.0 * far * near) / (far - near)) / -vz


def check_mesh(entity, project):
    """Decode one inline mesh the way the loaders read it and check it is self-consistent.
    Catches a buffer whose size is not stride*count, an index that names a vertex that
    does not exist, an aabb that disagrees with the vertices, and a material pointing at
    a texture file the project does not have."""
    import base64
    import struct

    mesh = entity["comps"]["mesh"]
    buf, idx = mesh.get("buffer"), mesh.get("indices")
    if not isinstance(buf, dict) or not isinstance(idx, dict):
        return [f"entity {entity['id']}: mesh has no buffer or no indices node"], []
    problems = []
    raw_buf = base64.b64decode(buf.get("data", ""))
    stride, count = buf.get("stride"), buf.get("count")
    if stride * count != len(raw_buf):
        problems.append(f"entity {entity['id']}: buffer size {len(raw_buf)} != "
                        f"stride {stride} * count {count}")
    if buf.get("size") not in (None, len(raw_buf)):
        problems.append(f"entity {entity['id']}: buffer declares size {buf.get('size')} "
                        f"but the base64 decodes to {len(raw_buf)} bytes")

    attributes = buf.get("attributes", [])
    for a in attributes:
        if a.get("dataType") != "float":
            continue
        past = a["offset"] + a["elements"] * 4
        if past > stride:
            problems.append(f"entity {entity['id']}: attribute {a['type']} runs "
                            f"{past - stride} bytes past the stride")
        if a.get("count") not in (None, count):
            problems.append(f"entity {entity['id']}: attribute {a['type']} count "
                            f"{a.get('count')} != buffer count {count}")

    def attr(name, fallback_offset, fallback_elements):
        for a in attributes:
            if a.get("type") == name:
                return a["offset"], a["elements"]
        # A hand-written primitive omits the descriptors it does not sample, so read the
        # documented 48-byte layout (position 0, texcoord1 12, normal 20, color 32).
        return fallback_offset, fallback_elements

    pos_off, pos_el = attr("position", 0, 3)
    nrm_off, nrm_el = attr("normal", 20, 3)
    has_normals = any(a.get("type") == "normal" for a in attributes)

    def read(offset, elements, i):
        return struct.unpack_from(f"<{elements}f", raw_buf, i * stride + offset)

    verts = [read(pos_off, min(pos_el, 3), i) for i in range(count)]

    if has_normals:
        for i in range(count):
            n = read(nrm_off, min(nrm_el, 3), i)
            length = math.sqrt(sum(c * c for c in n))
            if not (0.99 < length < 1.01):
                problems.append(f"entity {entity['id']}: vertex {i} normal {n} is not "
                                f"unit (length {length:.4f})")
                break

    raw_idx = base64.b64decode(idx.get("data", ""))
    if idx.get("stride") * idx.get("count") != len(raw_idx):
        problems.append(f"entity {entity['id']}: indices size {len(raw_idx)} != "
                        f"stride {idx.get('stride')} * count {idx.get('count')}")
    code = "H" if idx.get("stride") == 2 else "I"
    indices = struct.unpack(f"<{idx['count']}{code}", raw_idx)
    over = [k for k in indices if k >= count]
    if over:
        problems.append(f"entity {entity['id']}: {len(over)} indices name a vertex past the "
                        f"buffer (max {max(over)}, count {count})")
    unused = count - max(indices) - 1 if indices else count
    if unused > 0:
        notes = [f"entity {entity['id']}: {unused} of {count} vertices are never indexed"]
    else:
        notes = []

    lo = [min(v[a] for v in verts) for a in range(3)]
    hi = [max(v[a] for v in verts) for a in range(3)]
    for key in ("aabb", "verticesAABB"):
        box = mesh.get(key)
        if not box:
            continue
        for a in range(3):
            if abs(box["min"][a] - lo[a]) > 1e-4 or abs(box["max"][a] - hi[a]) > 1e-4:
                problems.append(f"entity {entity['id']}: {key} does not match the vertices "
                                f"({box['min']}..{box['max']} vs {lo}..{hi})")
                break

    for submesh in mesh.get("submeshes", []):
        material = submesh.get("material", {})
        for key, value in material.items():
            if not isinstance(value, dict) or "path" not in value:
                continue
            target = os.path.join(project, value["path"].replace("/", os.sep))
            if not os.path.isfile(target):
                problems.append(f"entity {entity['id']}: {key} points at "
                                f"{value['path']}, which the project does not have")
    return problems, notes


def check_tiling(entity):
    """One texture must keep one metres-per-repeat everywhere on a box, or the map is
    stretched on the faces the UV span forgot. Material has no scale key (only heightScale
    and the per-map TexCoord slot) and the transform never touches the UV attribute, so the
    only proof of a tiling convention is the vertex data itself. u and v follow the axes
    MeshSystem::createBox assigns them; a mesh with a curved normal is left alone."""
    import base64
    import struct

    face_axes = {(0.0, 0.0, 1.0): (0, 1), (0.0, 0.0, -1.0): (0, 1),
                 (1.0, 0.0, 0.0): (2, 1), (-1.0, 0.0, 0.0): (2, 1),
                 (0.0, 1.0, 0.0): (0, 2), (0.0, -1.0, 0.0): (0, 2)}
    mesh = entity["comps"]["mesh"]
    submeshes = mesh.get("submeshes") or []
    if not submeshes or "baseColorTexture" not in (submeshes[0].get("material") or {}):
        return []
    buf = mesh["buffer"]
    raw = base64.b64decode(buf["data"])
    scale = [abs(c) for c in entity["comps"]["transform"].get("scale", [1, 1, 1])]

    groups = {}
    for i in range(buf["count"]):
        v = struct.unpack_from("<8f", raw, i * buf["stride"])
        key = tuple(round(c) for c in v[5:8])
        if key not in face_axes:
            return []          # curved surface: the axis mapping is not defined here
        groups.setdefault(key, []).append(v)

    densities = []
    for key, vs in groups.items():
        axes = face_axes[key]
        span = lambda sel: max(sel(q) for q in vs) - min(sel(q) for q in vs)
        su, sv = span(lambda q: q[3]), span(lambda q: q[4])
        if su <= 1e-6 or sv <= 1e-6:
            return []          # unbaked UVs (a plain 0..1 map): no tiling to verify
        ex = span(lambda q: q[axes[0]]) * scale[axes[0]]
        ey = span(lambda q: q[axes[1]]) * scale[axes[1]]
        if ex <= 1e-6 or ey <= 1e-6:
            return []
        densities.append((math.sqrt(ex * ey / (su * sv)), key))
    if len(densities) < 2:
        return []
    lo, hi = min(d[0] for d in densities), max(d[0] for d in densities)
    if hi / lo > 1.05:
        worst = max(densities, key=lambda d: abs(math.log(d[0] / lo)))
        return [f"entity {entity['id']} '{entity['name']}': the texture repeats every "
                f"{lo:.2f} m on one face and every {worst[0]:.2f} m on normal {worst[1]} "
                f"({worst[0] / lo:.1f}x stretch)"]
    return []


def check(path):
    """Return the list of findings for one scene file."""
    raw = open(path, encoding="utf-8").read()
    doc = yaml.safe_load(raw)
    ents = []
    walk(doc.get("entities"), ents)
    problems = []
    notes = []

    ids = [e["id"] for e in ents]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        problems.append(f"duplicate entity ids {dupes}")

    project = os.path.dirname(path)
    if os.path.basename(project) == "scenes":
        project = os.path.dirname(project)
    decoded = 0
    for e in ents:
        for cname, comp in e["comps"].items():
            if cname == "mesh" or not isinstance(comp, dict):
                continue
            # The mesh payload only works under components.mesh: decodeMeshComponent looks
            # the node up by key. Written at the wrong indent it parses happily and the
            # entity still loads an empty mesh, so check the nesting before the bytes.
            # submeshes/eBuffers are the keys only that decoder reads (Stream.cpp:5115-5127);
            # buffer/indices alone are not a clue, UIComponent owns a glyph buffer too.
            nested = [k for k in ("submeshes", "eBuffers") if k in comp]
            if nested:
                problems.append(f"entity {e['id']} '{e['name']}': {nested} sits under "
                                f"'{cname}', not under 'mesh' - the component never decodes")
        mesh = e["comps"].get("mesh")
        if isinstance(mesh, dict) and "buffer" in mesh:
            found, mesh_notes = check_mesh(e, project)
            problems += found
            notes += mesh_notes
            if not found:
                problems += check_tiling(e)
            decoded += 1

    cameras = [e for e in ents if "camera" in e["comps"]]
    main = doc.get("mainCamera")
    if not cameras:
        if main not in (0, None):
            problems.append(f"mainCamera {main} names an entity with no camera component")
    elif main not in [e["id"] for e in cameras]:
        problems.append(f"mainCamera {main} but the cameras are "
                        f"{[(e['id'], e['name']) for e in cameras]}")

    for e in ents:
        mesh = e["comps"].get("mesh")
        if isinstance(mesh, dict) and "meshType" in mesh and "buffer" not in mesh:
            problems.append(f"entity {e['id']} '{e['name']}': mesh only has meshType "
                            f"'{mesh.get('meshType')}', which decodeMeshComponent never reads "
                            f"- the mesh loads empty")

    for cam in cameras:
        c = cam["comps"]["camera"]
        near, far = c.get("nearClip"), c.get("farClip")
        persp = c.get("type") == "camera_perspective"
        if near is None or far is None:
            continue
        if not persp and near >= far:
            problems.append(f"camera {cam['id']}: ortho near {near} >= far {far}")
        for e in ents:
            mesh = e["comps"].get("mesh")
            if not isinstance(mesh, dict) or "buffer" not in mesh:
                continue
            pos = [float(v) for v in e["comps"]["transform"]["position"]]
            vz, exact = view_z(pos, c, cam["comps"]["transform"])
            if not exact:
                notes.append(f"camera {cam['id']} views along a rotation, depth check "
                             f"skipped for entity {e['id']}")
                continue
            if persp and vz >= -near:
                # Geometry behind the start view is normal in a 3D level, not a fault.
                notes.append(f"entity {e['id']} '{e['name']}' sits behind camera {cam['id']} "
                             f"at its start pose")
                continue
            ndc = clip_z_persp(vz, near, far) if persp else clip_z_ortho(vz, near, far)
            if not (-1.0 <= ndc <= 1.0):
                problems.append(f"entity {e['id']} '{e['name']}' view z {vz:.2f} -> ndc z "
                                f"{ndc:.3f} falls outside camera {cam['id']} "
                                f"[near {near}, far {far}]")

    for n in notes:
        print("   note:", n)
    return problems, len(ents), decoded


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))), "templates")
    paths = sorted(glob.glob(os.path.join(root, "*", "*.scene"))
                   + glob.glob(os.path.join(root, "*", "*", "*.scene")))
    paths = [p for p in paths if "/.quantum/" not in p.replace("\\", "/")]
    if not paths:
        sys.exit(f"check_template_scenes: no .scene files under {root}")

    total = 0
    meshes = 0
    for p in paths:
        problems, count, decoded = check(p)
        meshes += decoded
        label = "/".join(os.path.relpath(p, root).split(os.sep))
        if problems:
            total += len(problems)
            print(f"\n== {label}  ({count} entities, {decoded} inline meshes)")
            for pr in problems:
                print("   -", pr)
        else:
            print(f"== {label}  ({count} entities, {decoded} inline meshes)  ok")

    print(f"\n{len(paths)} scenes, {meshes} inline meshes decoded, {total} problems")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
