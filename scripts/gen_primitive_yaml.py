#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Emit the engine's inline-mesh YAML for a primitive, in the exact wire format
editor/Stream.cpp writes (decodeMeshComponent reads buffer/indices/submeshes and
never a primitive descriptor, so a hand-written {meshType: box} loads empty).

Vertex layout is the editor's: 48 bytes = position(3f) + texcoord1(2f) +
normal(3f) + color(4f), all attributes reported normalized as the encoder does.
Indices are unsigned short. Both go out base64-encoded, little-endian.

The per-vertex colour is always white: an editor-saved primitive carries the tint only
in baseColorFactor (decoded from a real "Crate A" block: 24 vertices, colour
(1,1,1,1), UV 0->0.55), so writing --color into the vertices as well would square it.

Tiling is baked into the UVs, because Material has no texture-scale key (only
heightScale and the per-map TexCoord slot) and a scene cannot pass the tiles argument
MeshSystem::createBox takes. --tile is metres per repeat and --world is the size the
entity reaches AFTER its transform scale, so a unit box scaled to 20 x 1 x 1 gets the
UV span a 20 m board would have.

    python scripts/gen_primitive_yaml.py capsule --radius 0.3 --height 1.5 --color 0.85 0.45 0.35
    python scripts/gen_primitive_yaml.py box --tile 0.5 --world 20 1 1

--indent names the column the fragment is pasted at, and the output starts with its own
`mesh:` key: a payload written under another component (transform: is the easy mistake)
parses cleanly, decodes nothing and shows no error.
"""

import argparse
import base64
import math
import os
import struct
import sys

STRIDE = 48


def _pack_vertex(px, py, pz, u, v, nx, ny, nz, r, g, b, a):
    return struct.pack("<12f", px, py, pz, u, v, nx, ny, nz, r, g, b, a)


def _b64(data):
    return base64.b64encode(data).decode("ascii")


def plane(size):
    h = size * 0.5
    verts = [(-h, 0.0, -h, 0.0, 0.0), (-h, 0.0, h, 0.0, 1.0),
             (h, 0.0, h, 1.0, 1.0), (h, 0.0, -h, 1.0, 0.0)]
    n = (0.0, 1.0, 0.0)
    indices = [0, 1, 2, 0, 2, 3]
    return verts, [n] * len(verts), indices


def box(width, height, depth, tile=0.0, world=None):
    """A transcription of MeshSystem::createBox (engine/core/subsystem/MeshSystem.cpp:3957)
    with tiles = 1: face order front/back/left/right/top/bottom, one UV repeat per face,
    unit normals and the per-face winding that function writes. Nothing here is invented,
    so a scene built by this tool holds the same geometry the editor creates.

    tile > 0 replaces that one-repeat-per-face UV with one repeat every `tile` metres of
    the face's own world extent, so a long ground board is not left with a stretched
    texture. world is the size the entity reaches after its transform scale; the
    transform never touches the UV attribute, so the repeat count has to be baked here."""
    hw, hh, hd = width * 0.5, height * 0.5, depth * 0.5
    wx, wy, wz = world if world else (width, height, depth)

    def span(metres):
        return 1.0 if tile <= 0.0 else max(metres / tile, 1e-6)

    # corners, normal, unit UV pattern, winding, then the face's (u, v) world extents
    faces = [
        (((-hw, -hh, hd), (hw, -hh, hd), (hw, hh, hd), (-hw, hh, hd)),
         (0.0, 0.0, 1.0), ((0, 1), (1, 1), (1, 0), (0, 0)), (0, 1, 2, 0, 2, 3),
         (span(wx), span(wy))),
        (((-hw, -hh, -hd), (hw, -hh, -hd), (hw, hh, -hd), (-hw, hh, -hd)),
         (0.0, 0.0, -1.0), ((0, 1), (1, 1), (1, 0), (0, 0)), (0, 2, 1, 0, 3, 2),
         (span(wx), span(wy))),
        (((-hw, -hh, hd), (-hw, hh, hd), (-hw, hh, -hd), (-hw, -hh, -hd)),
         (-1.0, 0.0, 0.0), ((0, 1), (0, 0), (1, 0), (1, 1)), (0, 1, 2, 0, 2, 3),
         (span(wz), span(wy))),
        (((hw, -hh, hd), (hw, hh, hd), (hw, hh, -hd), (hw, -hh, -hd)),
         (1.0, 0.0, 0.0), ((0, 1), (0, 0), (1, 0), (1, 1)), (0, 2, 1, 0, 3, 2),
         (span(wz), span(wy))),
        (((-hw, hh, hd), (hw, hh, hd), (hw, hh, -hd), (-hw, hh, -hd)),
         (0.0, 1.0, 0.0), ((0, 1), (1, 1), (1, 0), (0, 0)), (0, 1, 2, 0, 2, 3),
         (span(wx), span(wz))),
        (((-hw, -hh, hd), (hw, -hh, hd), (hw, -hh, -hd), (-hw, -hh, -hd)),
         (0.0, -1.0, 0.0), ((0, 0), (1, 0), (1, 1), (0, 1)), (0, 2, 1, 0, 3, 2),
         (span(wx), span(wz))),
    ]
    verts, normals, indices = [], [], []
    for base, (corners, normal, uvs, order, (su, sv)) in enumerate(faces):
        for corner, (u, v) in zip(corners, uvs):
            verts.append((corner[0], corner[1], corner[2], u * su, v * sv))
            normals.append(normal)
        indices += [base * 4 + i for i in order]
    return verts, normals, indices


def cylinder(radius, height, slices, caps=True):
    half = height * 0.5
    verts, normals, indices = [], [], []

    def ring(y, v, radius_at):
        start = len(verts)
        for i in range(slices):
            a = 2.0 * math.pi * i / slices
            verts.append((math.cos(a) * radius_at, y, math.sin(a) * radius_at,
                          i / slices, v))
            normals.append((math.cos(a), 0.0, math.sin(a)))
        return start

    bottom = ring(-half, 0.0, radius)
    top = ring(half, 1.0, radius)
    for i in range(slices):
        b0, b1 = bottom + i, bottom + (i + 1) % slices
        t0, t1 = top + i, top + (i + 1) % slices
        indices += [b0, b1, t1, b0, t1, t0]

    if caps:
        for y, sign in ((half, 1.0), (-half, -1.0)):
            centre = len(verts)
            verts.append((0.0, y, 0.0, 0.5, 0.5))
            normals.append((0.0, sign, 0.0))
            start = len(verts)
            for i in range(slices):
                a = 2.0 * math.pi * i / slices
                verts.append((math.cos(a) * radius, y, math.sin(a) * radius,
                              0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)))
                normals.append((0.0, sign, 0.0))
            for i in range(slices):
                cur, nxt = start + i, start + (i + 1) % slices
                if sign > 0:
                    indices += [centre, cur, nxt]
                else:
                    indices += [centre, nxt, cur]
    return verts, normals, indices


def capsule(radius, height, slices, stacks):
    """height is the total height, radius the base radius - same convention as
    MeshSystem::createCapsule, so a script and a scene file agree."""
    half_cyl = max(height * 0.5 - radius, 0.0)
    verts, normals, indices = [], [], []

    rings = []
    for s in range(stacks + 1):
        t = s / stacks
        y = -height * 0.5 + height * t
        # the ring's radius and normal come from the sphere cap it sits on
        if y > half_cyl:
            phi = math.acos(max(-1.0, min(1.0, (y - half_cyl) / radius)))
        elif y < -half_cyl:
            phi = math.acos(max(-1.0, min(1.0, (y + half_cyl) / radius)))
        else:
            phi = math.pi * 0.5
        r = radius * math.sin(phi)
        start = len(verts)
        for i in range(slices):
            a = 2.0 * math.pi * i / slices
            nx, nz = math.cos(a), math.sin(a)
            verts.append((nx * r, y, nz * r, i / slices, t))
            normals.append((nx * math.sin(phi), math.cos(phi), nz * math.sin(phi)))
        rings.append(start)
    for s in range(stacks):
        lo, hi = rings[s], rings[s + 1]
        for i in range(slices):
            i0, i1 = lo + i, lo + (i + 1) % slices
            j0, j1 = hi + i, hi + (i + 1) % slices
            indices += [i0, i1, j1, i0, j1, j0]
    return verts, normals, indices


def sphere(radius, slices, stacks):
    verts, normals, indices = [], [], []
    rings = []
    for s in range(stacks + 1):
        phi = math.pi * s / stacks
        y = radius * math.cos(phi)
        r = radius * math.sin(phi)
        start = len(verts)
        for i in range(slices):
            a = 2.0 * math.pi * i / slices
            nx, nz = math.cos(a), math.sin(a)
            verts.append((nx * r, y, nz * r, i / slices, s / stacks))
            normals.append((nx * math.sin(phi), math.cos(phi), nz * math.sin(phi)))
        rings.append(start)
    for s in range(stacks):
        lo, hi = rings[s], rings[s + 1]
        for i in range(slices):
            i0, i1 = lo + i, lo + (i + 1) % slices
            j0, j1 = hi + i, hi + (i + 1) % slices
            indices += [i0, i1, j1, i0, j1, j0]
    return verts, normals, indices


def torus(radius, tube, slices, stacks):
    verts, normals, indices = [], [], []
    rings = []
    for s in range(stacks + 1):
        u = 2.0 * math.pi * s / stacks
        cu, su = math.cos(u), math.sin(u)
        start = len(verts)
        for i in range(slices):
            v = 2.0 * math.pi * i / slices
            cv, sv = math.cos(v), math.sin(v)
            nx, ny, nz = cv * cu, sv, cv * su
            verts.append(((radius + tube * cv) * cu, tube * sv, (radius + tube * cv) * su,
                          s / stacks, i / slices))
            normals.append((nx, ny, nz))
        rings.append(start)
    for s in range(stacks):
        lo, hi = rings[s], rings[s + 1]
        for i in range(slices):
            i0, i1 = lo + i, lo + (i + 1) % slices
            j0, j1 = hi + i, hi + (i + 1) % slices
            indices += [i0, i1, j1, i0, j1, j0]
    return verts, normals, indices


SHAPES = {
    "plane": lambda a: plane(a.size),
    "box": lambda a: box(a.width, a.height, a.depth, a.tile, a.world),
    "sphere": lambda a: sphere(a.radius, a.slices, a.stacks),
    "cylinder": lambda a: cylinder(a.radius, a.height, a.slices),
    "capsule": lambda a: capsule(a.radius, a.height, a.slices, a.stacks),
    "torus": lambda a: torus(a.radius, a.top_radius, a.slices, a.stacks),
}

# The same four maps and the same factors editor/util/DefaultMaterial.cpp puts on every
# primitive the editor creates, so a scene written by this tool matches a scene authored
# by hand in the Create menu. Keep in sync with that file.
DEFAULT_DIR = "materials/default"
DEFAULT_SET = {
    "baseColorTexture": "floor-tile-white-ceramic-tiles-1024-color.jpg",
    "normalTexture": "floor-tile-white-ceramic-tiles-1024-normal-gl.png",
    "metallicRoughnessTexture": "floor-tile-white-ceramic-tiles-1024-roughness.png",
    "heightTexture": "floor-tile-white-ceramic-tiles-1024-height.png",
}
# encodeMaterial emits the maps in this order.
DEFAULT_ORDER = ["baseColorTexture", "metallicRoughnessTexture",
                 "normalTexture", "heightTexture"]


def texture_lines(pad, key, path):
    """One Texture node as encodeTexture writes it: the enum name, then source/path,
    then the filters and the releaseDataAfterLoad flag."""
    out = [f"{pad}{key}:",
           f"{pad}  textureType: 2D",
           f"{pad}  source: path",
           f"{pad}  path: {path}",
           f"{pad}  minFilter: linear_mipmap_linear",
           f"{pad}  magFilter: linear",
           f"{pad}  wrapU: repeat",
           f"{pad}  wrapV: repeat",
           f"{pad}  releaseDataAfterLoad: true"]
    return out


def material_lines(pad, color, default_material):
    """The `material:` node. Without a texture set this is the plain tinted material;
    with one it carries the four ceramic maps and the factors DefaultMaterial::build
    sets (roughness 1 so the map drives it, metallic 0 because glTF reads metal from
    this slot's blue channel and a lone grayscale roughness map would put the same
    value there)."""
    rough = 1.0 if default_material else 0.7
    out = [f"{pad}baseColorFactor: [{_f(color[0])}, {_f(color[1])}, {_f(color[2])}, {_f(color[3])}]",
           f"{pad}metallicFactor: 0.0",
           f"{pad}roughnessFactor: {rough}",
           f"{pad}alphaCutoff: 0.5",
           f"{pad}alphaMode: ALPHA_OPAQUE",
           f"{pad}emissiveFactor: [0, 0, 0]"]
    if default_material:
        for key in DEFAULT_ORDER:
            out += texture_lines(pad, key, f"{DEFAULT_DIR}/{DEFAULT_SET[key]}")
        # encodeMaterial only writes the parallax tuning alongside a height map
        out.append(f"{pad}heightScale: 0.02")
        out.append(f"{pad}heightMinLayers: 8.0")
        out.append(f"{pad}heightMaxLayers: 32.0")
    for key in ("baseColorTexCoord", "metallicRoughnessTexCoord", "occlusionTexCoord",
                "emissiveTexCoord", "normalTexCoord", "heightTexCoord"):
        out.append(f"{pad}{key}: 0")
    out.append(f'{pad}name: ""' if not default_material else f'{pad}name: default')
    return out


ATTRIBUTE_ORDER = [
    ("position", 3, 0),
    ("texcoord1", 2, 12),
    ("normal", 3, 20),
    ("color", 4, 32),
]


def emit(shape, color, indent, **kwargs):
    class Args:
        pass

    a = Args()
    for name in ("size", "width", "height", "depth", "radius", "top_radius", "slices", "stacks"):
        setattr(a, name, kwargs.get(name, {"size": 1.0, "width": 1.0, "height": 1.0,
                                           "depth": 1.0, "radius": 0.5, "top_radius": 0.25,
                                           "slices": 24, "stacks": 12}[name]))
    a.tile = kwargs.get("tile", 0.0)
    a.world = kwargs.get("world")
    default_material = kwargs.get("default_material", False)
    verts, normals, indices = SHAPES[shape](a)

    count = len(verts)
    blob = bytearray()
    for (vx, vy, vz, u, v), (nx, ny, nz) in zip(verts, normals):
        blob += _pack_vertex(vx, vy, vz, u, v, nx, ny, nz, 1.0, 1.0, 1.0, 1.0)
    idx = bytearray()
    for i in indices:
        idx += struct.pack("<H", i)

    pad = " " * (indent + 2)
    # The fragment owns its component key. Emitting only the fields under it is what let a
    # payload be pasted under `transform:` - it parses, decodes nothing and stays invisible,
    # so the tool now writes the whole `mesh:` node at --indent.
    out = [" " * indent + "mesh:"]
    out.append(f"{pad}buffer:")
    out.append(f"{pad}  type: vertex_buffer")
    out.append(f"{pad}  usage: immutable")
    out.append(f"{pad}  stride: {STRIDE}")
    out.append(f"{pad}  size: {len(blob)}")
    out.append(f"{pad}  count: {count}")
    out.append(f"{pad}  renderAttributes: true")
    out.append(f"{pad}  instanceBuffer: false")
    out.append(f"{pad}  attributes:")
    for name, elements, offset in ATTRIBUTE_ORDER:
        out.append(f"{pad}    - type: {name}")
        out.append(f"{pad}      dataType: float")
        out.append(f'{pad}      bufferName: ""')
        out.append(f"{pad}      elements: {elements}")
        out.append(f"{pad}      offset: {offset}")
        out.append(f"{pad}      count: {count}")
        out.append(f"{pad}      normalized: true")
        out.append(f"{pad}      perInstance: false")
    out.append(f"{pad}  data: {_b64(blob)}")
    # encodeInterleavedBuffer puts vertexSize inside the buffer node, and decodeBuffer reads the
    # vertex layout from there - at the mesh level it is simply lost.
    out.append(f"{pad}  vertexSize: {STRIDE}")
    out.append(f"{pad}indices:")
    out.append(f"{pad}  type: index_buffer")
    out.append(f"{pad}  usage: immutable")
    out.append(f"{pad}  stride: 2")
    out.append(f"{pad}  size: {len(idx)}")
    out.append(f"{pad}  count: {len(indices)}")
    out.append(f"{pad}  renderAttributes: true")
    out.append(f"{pad}  instanceBuffer: false")
    out.append(f"{pad}  attributes:")
    out.append(f"{pad}    - type: index")
    out.append(f"{pad}      dataType: unsigned_short")
    out.append(f'{pad}      bufferName: ""')
    out.append(f"{pad}      elements: 1")
    out.append(f"{pad}      offset: 0")
    out.append(f"{pad}      count: {len(indices)}")
    out.append(f"{pad}      normalized: true")
    out.append(f"{pad}      perInstance: false")
    out.append(f"{pad}  data: {_b64(idx)}")
    out.append(f"{pad}eBuffers: ~")
    out.append(f"{pad}submeshes:")
    out.append(f"{pad}  - material:")
    out += material_lines(f"{pad}      ", color, default_material)
    out.append(f"{pad}    textureRect: [0, 0, 1, 1]")
    out.append(f"{pad}    primitiveType: triangles")
    out.append(f"{pad}    vertexCount: {len(indices)}")
    out.append(f"{pad}    faceCulling: true")
    out.append(f"{pad}    textureShadow: false")
    out.append(f"{pad}    hasTexCoord1: true")
    out.append(f"{pad}    hasTexCoord2: false")
    out.append(f"{pad}    hasNormalMap: {'true' if default_material else 'false'}")
    out.append(f"{pad}    hasTangent: false")
    out.append(f"{pad}    hasVertexColor3: false")
    out.append(f"{pad}    hasVertexColor4: true")
    out.append(f"{pad}    hasTextureRect: false")
    out.append(f"{pad}    hasSkinning: false")
    out.append(f"{pad}    hasMorphTarget: false")
    out.append(f"{pad}    hasMorphNormal: false")
    out.append(f"{pad}    hasMorphTangent: false")
    mins = [min(v[i] for v in verts) for i in range(3)]
    maxs = [max(v[i] for v in verts) for i in range(3)]
    out.append(f"{pad}aabb:")
    out.append(f"{pad}  min: [{_f(mins[0])}, {_f(mins[1])}, {_f(mins[2])}]")
    out.append(f"{pad}  max: [{_f(maxs[0])}, {_f(maxs[1])}, {_f(maxs[2])}]")
    out.append(f"{pad}verticesAABB:")
    out.append(f"{pad}  min: [{_f(mins[0])}, {_f(mins[1])}, {_f(mins[2])}]")
    out.append(f"{pad}  max: [{_f(maxs[0])}, {_f(maxs[1])}, {_f(maxs[2])}]")
    out.append(f"{pad}receiveLights: true")
    out.append(f"{pad}receiveIBL: true")
    out.append(f"{pad}castShadows: true")
    out.append(f"{pad}receiveShadows: true")
    out.append(f"{pad}renderInReflectionProbes: true")
    out.append(f"{pad}transparent: false")
    out.append(f"{pad}cullingMode: back")
    out.append(f"{pad}windingOrder: ccw")
    return "\n".join(out)


def _f(value):
    text = f"{float(value):.6g}"
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("shape", choices=sorted(SHAPES))
    p.add_argument("--size", type=float, default=1.0)
    p.add_argument("--width", type=float, default=1.0)
    p.add_argument("--height", type=float, default=1.0)
    p.add_argument("--depth", type=float, default=1.0)
    p.add_argument("--radius", type=float, default=0.5)
    p.add_argument("--top-radius", dest="top_radius", type=float, default=0.25)
    p.add_argument("--slices", type=int, default=24)
    p.add_argument("--stacks", type=int, default=12)
    p.add_argument("--color", type=float, nargs=4, default=[1.0, 1.0, 1.0, 1.0])
    p.add_argument("--tile", type=float, default=0.0,
                   help="metres per texture repeat baked into the UVs (box; 0 = one repeat per face)")
    p.add_argument("--world", type=float, nargs=3, metavar=("W", "H", "D"),
                   help="size the entity reaches after its transform scale, for --tile")
    p.add_argument("--default-material", action="store_true", dest="default_material",
                   help="attach the ceramic set editor/util/DefaultMaterial.cpp gives primitives")
    p.add_argument("--project", help="project dir to check the material set is installed in")
    p.add_argument("--indent", type=int, default=6,
                   help="column of the emitted mesh: key (a component sits 6 spaces in an "
                        "entity block, 10 in a nested child)")
    args = p.parse_args()

    if args.default_material and args.project:
        missing = [n for n in DEFAULT_SET.values()
                   if not os.path.isfile(os.path.join(args.project, DEFAULT_DIR, n))]
        if missing:
            sys.exit(f"gen_primitive_yaml: {DEFAULT_DIR} is not installed in {args.project}: "
                     + ", ".join(missing))
    elif args.default_material:
        print("gen_primitive_yaml: warning: --project not given, the material maps were not "
              "checked and the scene will load with an unresolvable texture path",
              file=sys.stderr)

    print(emit(args.shape, args.color, args.indent,
               size=args.size, width=args.width, height=args.height, depth=args.depth,
               radius=args.radius, top_radius=args.top_radius,
               slices=args.slices, stacks=args.stacks,
               tile=args.tile, world=args.world, default_material=args.default_material))


if __name__ == "__main__":
    main()
