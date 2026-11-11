#!/usr/bin/env python3
"""Install the engine's stock asset pack: ground textures, foliage, trees, characters and weapons.

The pack is fetched, never committed: every file here is CC0 1.0 Universal and comes from a URL
recorded in LICENCES.md next to the output, so a machine can be rebuilt with
`python scripts/fetch_asset_pack.py` without putting a hundred megabytes of binaries in git.

Sources are restricted on purpose:
  * Poly Haven - all assets CC0, a public JSON API and a direct file host, so author and licence
    are read from the API instead of typed by hand;
  * the Universal Animation Library mirror - CC0, ships the licence text inside the pack.
Sources that only serve flat-coloured models (Quaternius' low-poly packs) or that refuse scripted
downloads (kenney.nl returns 403 to anything that is not a browser) are not used: every model in
this pack has to arrive with real textures.

Usage:
    python scripts/fetch_asset_pack.py                    # everything
    python scripts/fetch_asset_pack.py --only ground,weapons
    python scripts/fetch_asset_pack.py --list             # show the plan, download nothing
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO, "engine", "assets")

CC0 = "CC0 1.0 Universal, https://creativecommons.org/publicdomain/zero/1.0/"
PH_PAGE = "https://polyhaven.com/a/"
PH_API = "https://api.polyhaven.com/"
UAL_REPO = "https://raw.githubusercontent.com/NafisRayan/Animate-Rigged-Humanoid-No-Blender/main/"
UAL_LICENSE = ("CC0 1.0 Universal (the pack ships License.txt with that dedication), "
               "mirror of Quaternius' Universal Animation Library, https://quaternius.com")

# Ground albedo/normal/roughness/height/ao sets. The engine classifies map roles by filename
# suffix, so each map lands with the same suffix the stock default material uses.
GROUND = [
    ("grass_ground", "ground-grass"),
    ("leafy_grass", "ground-grass-leafy"),
    ("forrest_ground_01", "ground-forest"),
    ("forest_floor", "ground-forest-litter"),
    ("brown_mud", "ground-mud"),
    ("brown_mud_leaves_01", "ground-mud-leaves"),
    ("aerial_sand", "ground-sand"),
    ("clean_pebbles", "ground-pebbles"),
    ("cobblestone_01", "ground-cobblestone"),
    ("asphalt_01", "ground-asphalt"),
    ("snow_01", "ground-snow"),
    ("aerial_grass_rock", "ground-mossy-rock"),
]

# Photogrammetry nature. Poly Haven's leafy trees are geometry-node scenes whose vertex buffer
# alone is 60 MB to 950 MB, so the pack takes the saplings and the two island trees that stay
# under ~60 MB, and the light dead/roots/stump assets for ground dressing.
FOLIAGE = [
    "grass_medium_01", "grass_medium_02", "grass_bermuda_01", "fern_02", "moss_01",
    "shrub_01", "shrub_02", "celandine_01", "dandelion_01", "nettle_plant",
    "anthurium_botany_01", "calathea_orbifolia_01", "periwinkle_plant", "crystalline_iceplant",
]

TREES = [
    "island_tree_02", "pine_sapling_small", "fir_sapling", "quiver_tree_01", "quiver_tree_02",
    "dead_tree_trunk", "dead_tree_trunk_02", "tree_stump_01", "tree_stump_02", "pine_roots",
    "root_cluster_01", "single_root", "dry_branches_medium_01", "bark_debris_01",
    "rock_07", "rock_09", "boulder_01", "mountainside",
]

WEAPONS = [
    "service_pistol", "bolt_action_rifle_7_62", "stick_grenade", "ammo_box", "binoculars",
    "antique_katana_01", "antique_estoc", "ornate_medieval_dagger", "ornate_medieval_mace",
    "ornate_war_hammer", "machete", "hatchet", "wooden_axe", "wooden_handle_saber",
    "kite_shield", "fish_knife",
]

# Character bodies, rigged, animated and textured, plus the hairstyles and eyebrows that attach to
# their head bone. The mirror's plain UAL1/UAL2/Mannequin bodies are deliberately absent: they are
# the animation sources but ship with zero images, and every asset in this pack has to be textured.
CHARACTERS = [
    ("human_male", "test/human_male.glb", "glb"),
    ("human_female", "test/human_female.glb", "glb"),
    ("Superhero_Male", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Base%20Characters/Godot%20-%20UE/Superhero_Male_FullBody.gltf", "gltf"),
    ("Superhero_Female", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Base%20Characters/Godot%20-%20UE/Superhero_Female_FullBody.gltf", "gltf"),
    ("Hair_Beard", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Hair_Beard.gltf", "gltf"),
    ("Hair_Long", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Hair_Long.gltf", "gltf"),
    ("Hair_Buns", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Hair_Buns.gltf", "gltf"),
    ("Hair_Buzzed", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Hair_Buzzed.gltf", "gltf"),
    ("Hair_BuzzedFemale", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Hair_BuzzedFemale.gltf", "gltf"),
    ("Hair_SimpleParted", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Hair_SimpleParted.gltf", "gltf"),
    ("Eyebrows_Regular", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Eyebrows_Regular.gltf", "gltf"),
    ("Eyebrows_Female", "Universal%20Base%20Characters%5BStandard%5D/Universal%20Base%20Characters%5BStandard%5D/Hairstyles/Rigged%20to%20Head%20Bone/glTF%20(Godot%20-Unreal)/Eyebrows_Female.gltf", "gltf"),
]

# Poly Haven map -> (engine material suffix, preferred file format).
# The API keys are the pack's own ("Diffuse", "Rough", "Displacement"), and each resolution holds
# exr/jpg/png. The engine reads neither exr nor 8-bit-only normals well, so colour-class maps come
# as JPEG and the two maps that carry measured data - tangent normal and height - come as PNG.
# "arm" is skipped: it repacks the same AO and roughness pixels the engine already gets separately.
GROUND_MAPS = {
    "Diffuse": ("color", "jpg"),
    "nor_gl": ("normal-gl", "png"),
    "nor_dx": ("normal-dx", "png"),
    "Rough": ("roughness", "jpg"),
    "AO": ("ao", "jpg"),
    "Displacement": ("height", "png"),
}

CATEGORIES = ("ground", "foliage", "trees", "weapons", "characters")


def api(path):
    req = urllib.request.Request(PH_API + path, headers={"User-Agent": "quantum-asset-pack"})
    with urllib.request.urlopen(req, timeout=90) as response:
        return json.loads(response.read().decode("utf-8"))


def download(url, dest, want_md5=None):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.exists(dest) and os.path.getsize(dest) > 0 and not args.force:
        if not want_md5 or md5file(dest) == want_md5:
            return False
    req = urllib.request.Request(url, headers={"User-Agent": "quantum-asset-pack"})
    with urllib.request.urlopen(req, timeout=600) as response:
        data = response.read()
    if want_md5 and hashlib.md5(data).hexdigest() != want_md5:
        raise IOError("md5 mismatch for " + url)
    with open(dest, "wb") as handle:
        handle.write(data)
    return True


def download_uri(base, ref, dest):
    """Fetch a uri a glTF references, returning the name that actually resolved.

    The UAL mirror's own glTFs carry a broken export artefact: `T_Eye_Normal_png.png` and
    `T_Hair_1_Normal_png.png` are referenced but the folders hold `T_Eye_Normal.png` and
    `T_Hair_1_Normal.png`. Dropping a normal map would quietly change how the body shades, so
    the stray infix is stripped and retried instead of skipping the texture.
    """
    candidates = [ref]
    if "_png." in os.path.basename(ref):
        candidates.append(ref.replace("_png.", "."))
    for cand in candidates:
        try:
            download(base + "/" + urllib.parse.quote(cand, safe="/"), dest)
            return cand
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
    raise IOError("referenced file is missing from the source: %s/%s" % (base, ref))


def md5file(path):
    h = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pad4(data, fill):
    r = len(data) % 4
    return data if not r else data + bytes([fill]) * (4 - r)


def read_glb_report(path):
    """Return (meshes, materials, images, textured) from a GLB header without any dependency."""
    with open(path, "rb") as handle:
        head = handle.read(20)
        if len(head) < 20 or struct.unpack("<I", head[0:4])[0] != 0x46546C67:
            return None
        json_len = struct.unpack("<I", head[12:16])[0]
        doc = json.loads(handle.read(json_len).decode("utf-8"))
    meshes = len(doc.get("meshes", []))
    mats = doc.get("materials", [])
    images = len(doc.get("images", []))
    textured = sum(1 for m in mats
                   if (m.get("pbrMetallicRoughness") or {}).get("baseColorTexture") is not None)
    return meshes, len(mats), images, textured, len(doc.get("skins", [])), len(doc.get("animations", []))


def gltf_bundle_to_glb(bundle_dir, gltf_name, out_path):
    """Fold a .gltf plus its .bin and its loose texture files into one self-contained .glb.

    The original binary buffer stays at offset zero, so every existing bufferView keeps its
    byteOffset; texture bytes are appended after it and each image switches from a uri to a
    bufferView. No geometry, no pixel and no material is touched.
    """
    with open(os.path.join(bundle_dir, gltf_name), "rb") as handle:
        doc = json.loads(handle.read().decode("utf-8"))

    bin_bytes = b""
    buffers = doc.get("buffers", [])
    if buffers and buffers[0].get("uri"):
        with open(os.path.join(bundle_dir, buffers[0]["uri"].replace("\\", os.sep)), "rb") as handle:
            bin_bytes = handle.read()
        buffers[0].pop("uri", None)
        buffers[0]["byteLength"] = len(bin_bytes)

    images = doc.get("images", [])
    body = bytearray(bin_bytes)
    for image in images:
        uri = image.pop("uri", None)
        if uri is None and image.get("bufferView") is not None:
            continue
        if uri is None:
            raise IOError("image with neither uri nor bufferView in " + gltf_name)
        with open(os.path.join(bundle_dir, uri.replace("\\", os.sep)), "rb") as handle:
            raw = handle.read()
        ext = os.path.splitext(uri)[1].lower()
        image["mimeType"] = "image/jpeg" if ext in (".jpg", ".jpeg") else "image/png"
        while len(body) % 4:
            body.append(0)
        image["bufferView"] = len(doc.setdefault("bufferViews", []))
        doc["bufferViews"].append({"buffer": 0, "byteOffset": len(body), "byteLength": len(raw)})
        body += raw

    if len(body) % 4:
        body += bytearray(4 - len(body) % 4)
    buffers[0]["byteLength"] = len(body)

    json_bytes = pad4(json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8"), 0x20)
    bin_bytes = bytes(body)
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as handle:
        handle.write(struct.pack("<III", 0x46546C67, 2, total))
        handle.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        handle.write(json_bytes)
        handle.write(struct.pack("<II", len(bin_bytes), 0x004E4942))
        handle.write(bin_bytes)


PROVENANCE = []   # (category, installed path, source url, licence, author)
MANIFEST = []     # one row per installed asset, for the headless probe and the README


def ph_model(category, asset_id, out_dir, tier):
    files = api("files/" + asset_id)
    gltf = files.get("gltf", {}).get(tier, {}).get("gltf")
    if not gltf:
        print("  skip %s: no %s gltf" % (asset_id, tier))
        return
    bundle = os.path.join(out_dir, "_" + asset_id)
    download(gltf["url"], os.path.join(bundle, asset_id + ".gltf"))
    for name, info in (gltf.get("include") or {}).items():
        download(info["url"], os.path.join(bundle, name.replace("/", os.sep)), info.get("md5"))
    out = os.path.join(out_dir, asset_id + ".glb")
    gltf_bundle_to_glb(bundle, asset_id + ".gltf", out)
    report = read_glb_report(out)
    size = os.path.getsize(out)
    PROVENANCE.append((category, os.path.relpath(out, out_root).replace("\\", "/"),
                       gltf["url"], CC0, author_of(asset_id)))
    MANIFEST.append({"kind": "model", "category": category, "asset": asset_id,
                     "path": os.path.relpath(out, out_root).replace("\\", "/"),
                     "bytes": size, "meshes": report[0], "materials": report[1],
                     "images": report[2], "texturedMaterials": report[3],
                     "skins": report[4], "animations": report[5],
                     "source": PH_PAGE + asset_id})
    print("  %-22s %8.1f MB  meshes=%d tex=%d/%d imgs=%d anims=%d"
          % (asset_id, size / 1e6, report[0], report[3], report[1], report[2], report[5]))
    remove_tree(bundle)


_ALL_ASSETS = None


def author_of(asset_id):
    """Author credit for a Poly Haven asset, read from the API rather than typed by hand.

    The API has no single-asset endpoint - `assets/{id}` is a 404 - so the one catalogue call is
    fetched lazily and shared; a run of sixty assets must not make sixty of them.
    """
    global _ALL_ASSETS
    if _ALL_ASSETS is None:
        _ALL_ASSETS = api("assets")
    meta = _ALL_ASSETS.get(asset_id) or {}
    return ", ".join(sorted((meta.get("authors") or {}).keys())) or "Poly Haven community"


def remove_tree(path):
    for root, dirs, names in os.walk(path, topdown=False):
        for name in names:
            os.remove(os.path.join(root, name))
        for name in dirs:
            os.rmdir(os.path.join(root, name))
    os.rmdir(path)


def ph_ground(asset_id, stem, out_dir, tier):
    files = api("files/" + asset_id)
    written = []
    for key, (suffix, fmt) in GROUND_MAPS.items():
        slot = files.get(key, {}).get(tier, {})
        entry = slot.get(fmt) or slot.get("jpg") or slot.get("png")
        if not entry or not entry.get("url"):
            print("    ! %s has no %s map at %s" % (asset_id, suffix, tier))
            continue
        ext = os.path.splitext(entry["url"])[1].lower() or ".jpg"
        dest = os.path.join(out_dir, "%s-%s%s" % (stem, suffix, ext))
        download(entry["url"], dest, entry.get("md5"))
        written.append(os.path.relpath(dest, out_root).replace("\\", "/"))
        PROVENANCE.append(("ground", written[-1], entry["url"], CC0, author_of(asset_id)))
    if written:
        MANIFEST.append({"kind": "material", "category": "ground", "asset": asset_id,
                         "stem": stem, "maps": written, "bytes": sum(os.path.getsize(os.path.join(out_root, p)) for p in written),
                         "source": PH_PAGE + asset_id})
    print("  %-22s %2d maps  %s" % (asset_id, len(written), stem))


def ual_character(name, encoded_path, kind, out_dir):
    url = UAL_REPO + encoded_path
    renamed = {}
    if kind == "glb":
        dest = os.path.join(out_dir, name + ".glb")
        download(url, dest)
    else:
        bundle = os.path.join(out_dir, "_" + name)
        download(url, os.path.join(bundle, name + ".gltf"))
        # A glTF bundle keeps its .bin and its textures beside the JSON: pull every file the
        # document references instead of guessing at the pack's folder layout.
        with open(os.path.join(bundle, name + ".gltf"), "rb") as handle:
            doc = json.loads(handle.read().decode("utf-8"))
        refs = [b.get("uri") for b in doc.get("buffers", []) if b.get("uri")]
        refs += [i.get("uri") for i in doc.get("images", []) if i.get("uri")]
        base = url.rsplit("/", 1)[0]
        renamed = {}
        for ref in refs:
            rel = ref.replace("\\", "/")
            parts = [p for p in rel.split("/") if p not in ("", ".")]
            if ".." in parts:
                raise IOError("bundle references a file outside its own folder: " + rel)
            resolved = download_uri(base, rel, os.path.join(bundle, *parts))
            if resolved != rel:
                renamed[rel] = resolved.rsplit("/", 1)[-1]
        dest = os.path.join(out_dir, name + ".glb")
        gltf_bundle_to_glb(bundle, name + ".gltf", dest)
        remove_tree(bundle)
    report = read_glb_report(dest)
    PROVENANCE.append(("characters", os.path.relpath(dest, out_root).replace("\\", "/"),
                       UAL_REPO + encoded_path, UAL_LICENSE, "Quaternius"))
    row = {"kind": "model", "category": "characters", "asset": name,
           "path": os.path.relpath(dest, out_root).replace("\\", "/"),
           "bytes": os.path.getsize(dest), "meshes": report[0], "materials": report[1],
           "images": report[2], "texturedMaterials": report[3], "skins": report[4],
           "animations": report[5],
           "source": "https://quaternius.com/packs/universalanimationlibrary.html"}
    if kind == "gltf":
        row["resolvedUris"] = renamed
    MANIFEST.append(row)
    print("  %-22s %8.1f MB  meshes=%d tex=%d/%d imgs=%d skins=%d anims=%d%s"
          % (name, os.path.getsize(dest) / 1e6, report[0], report[3], report[1], report[2],
             report[4], report[5],
             ("  renamed=" + ",".join(renamed)) if renamed else ""))


def write_licences(path):
    lines = [
        "# Licences - engine asset pack",
        "",
        "Every file below is **CC0 1.0 Universal** (Public Domain Dedication). Nothing in this",
        "folder carries a licence that requires attribution, so a project can ship it unchanged.",
        "This folder is not committed: run `python scripts/fetch_asset_pack.py` to rebuild it.",
        "",
        "Format: `<path> | <exact source URL> | <licence> | <author>`",
        "",
    ]
    current = None
    for category, rel, url, licence, author in sorted(PROVENANCE):
        if category != current:
            current = category
            lines += ["## %s/" % category, ""]
        lines.append("%s | %s | %s | %s" % (rel, url, licence, author))
    lines.append("")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))


def human(nbytes):
    return "%.1f MB" % (nbytes / 1e6) if nbytes >= 1e6 else "%.0f KB" % (nbytes / 1e3)


def main():
    global args, out_root
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--tier", default="1k", choices=("1k", "2k"))
    parser.add_argument("--only", default=",".join(CATEGORIES))
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    out_root = os.path.abspath(args.out)
    wanted = [c.strip() for c in args.only.split(",") if c.strip()]

    plan = {"ground": ["%s -> %s" % (a, s) for a, s in GROUND],
            "foliage": FOLIAGE, "trees": TREES, "weapons": WEAPONS,
            "characters": [c[0] for c in CHARACTERS]}
    if args.list:
        for category in wanted:
            print("%s: %d" % (category, len(plan[category])))
            for row in plan[category]:
                print("   ", row)
        return

    total = 0
    if "ground" in wanted:
        print("== ground textures (Poly Haven, %s)" % args.tier)
        directory = os.path.join(out_root, "materials", "ground")
        for asset_id, stem in GROUND:
            ph_ground(asset_id, stem, directory, args.tier)
    for category, ids in (("foliage", FOLIAGE), ("trees", TREES), ("weapons", WEAPONS)):
        if category not in wanted:
            continue
        print("== %s (Poly Haven, %s)" % (category, args.tier))
        directory = os.path.join(out_root, category)
        for asset_id in ids:
            ph_model(category, asset_id, directory, args.tier)
    if "characters" in wanted:
        print("== characters (Universal Animation Library, CC0)")
        directory = os.path.join(out_root, "characters")
        for name, path, kind in CHARACTERS:
            ual_character(name, path, kind, directory)

    for row in MANIFEST:
        total += row["bytes"]
    write_licences(os.path.join(out_root, "LICENCES.md"))
    with open(os.path.join(out_root, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump({"tier": args.tier, "assets": MANIFEST}, handle, indent=1)

    models = [r for r in MANIFEST if r["kind"] == "model"]
    untextured = [r["asset"] for r in models if r["images"] == 0]
    print("== installed %d entries, %s in %s" % (len(MANIFEST), human(total), out_root))
    print("   models=%d (textured %d) materials=%d"
          % (len(models), len(models) - len(untextured), len(MANIFEST) - len(models)))
    if untextured:
        print("   NOT TEXTURED: " + ", ".join(untextured))
    return 0


if __name__ == "__main__":
    sys.exit(main())
