// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/DefaultMaterial.h"

#include "util/FileUtils.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace quantum;
using namespace quantum::editor;

namespace {

    namespace fs = std::filesystem;

    const char* kDir = "materials/default";

    // The names carry the surface-map convention util/MaterialSets classifies, so the
    // same files also group into one Material Browser entry.
    const std::string kColor = "floor-tile-white-ceramic-tiles-1024-color.jpg";
    const std::string kNormal = "floor-tile-white-ceramic-tiles-1024-normal-gl.png";
    const std::string kRoughness = "floor-tile-white-ceramic-tiles-1024-roughness.png";
    const std::string kHeight = "floor-tile-white-ceramic-tiles-1024-height.png";

    const std::vector<std::string> kFiles = { kColor, kNormal, kRoughness, kHeight };

    // The maps the shader of a mesh actually samples.
    const std::vector<std::string> kShadedFiles = { kColor, kNormal, kRoughness, kHeight };

    Texture mapTexture(const fs::path& assetRelative) {
        Texture texture(assetRelative.generic_string());
        // A floor is seen at a grazing angle and the maps are 1024 wide, so the mip
        // chain is what keeps the tile grout from shimmering.
        texture.setMinFilter(TextureFilter::LINEAR_MIPMAP_LINEAR);
        return texture;
    }

    // isInstalled is one stat per map, and the editor pays it twice for every primitive it creates
    // because ensureInstalled and build each ask. The verdict only changes when this file installs
    // the set itself, so the checked directory and its answer are remembered, and the copy path
    // below is the one place that drops the memory.
    fs::path cachedRoot;
    bool cachedVerdict = false;
    bool cacheValid = false;

}

std::filesystem::path editor::DefaultMaterial::sourceDir() {
    std::error_code ec;

    const fs::path shipped = FileUtils::getEngineDir() / "assets" / kDir;
    if (fs::is_directory(shipped, ec)) {
        return shipped;
    }

    // The copy next to the executable only exists once copy_quantum_editor_engine
    // has run; a checkout that has not built yet still has the set in the source tree.
    fs::path dir = FileUtils::getExecutableDir();
    for (int depth = 0; depth <= 5 && !dir.empty(); ++depth) {
        const fs::path candidate = dir / "engine" / "assets" / kDir;
        if (fs::is_directory(candidate, ec)) {
            return candidate;
        }
        dir = dir.parent_path();
    }

    return {};
}

const std::vector<std::string>& editor::DefaultMaterial::fileNames() {
    return kFiles;
}

bool editor::DefaultMaterial::isInstalled(const fs::path& assetsRoot) {
    if (assetsRoot.empty()) {
        return false;
    }

    if (cacheValid && cachedRoot == assetsRoot) {
        return cachedVerdict;
    }

    const fs::path directory = assetsRoot / kDir;
    std::error_code ec;
    bool installed = true;
    for (const std::string& name : kShadedFiles) {
        if (!fs::is_regular_file(directory / name, ec)) {
            installed = false;
            break;
        }
    }

    cachedRoot = assetsRoot;
    cachedVerdict = installed;
    cacheValid = true;
    return installed;
}

bool editor::DefaultMaterial::ensureInstalled(const fs::path& assetsRoot, std::string* error) {
    if (isInstalled(assetsRoot)) {
        return true;
    }

    const fs::path source = sourceDir();
    if (source.empty()) {
        if (error) {
            *error = "the engine has no material set under engine/assets/" + std::string(kDir);
        }
        return false;
    }

    if (assetsRoot.empty()) {
        if (error) {
            *error = "no project assets directory";
        }
        return false;
    }

    const fs::path destination = assetsRoot / kDir;
    std::error_code ec;
    // whatever was remembered about this directory is about to change
    cacheValid = false;
    fs::create_directories(destination, ec);
    if (ec) {
        if (error) {
            *error = "cannot create " + destination.string() + ": " + ec.message();
        }
        return false;
    }

    for (const std::string& name : kFiles) {
        std::error_code copyEc;
        fs::copy_file(source / name, destination / name, fs::copy_options::skip_existing, copyEc);
        if (copyEc) {
            if (error) {
                *error = "cannot copy " + name + " into " + destination.string() + ": " + copyEc.message();
            }
            return false;
        }
    }

    return true;
}

Material editor::DefaultMaterial::build(const fs::path& assetsRoot) {
    if (!isInstalled(assetsRoot)) {
        return Material();
    }

    const fs::path directory(kDir);

    Material material;
    material.name = "default";
    material.baseColorTexture = mapTexture(directory / kColor);
    material.normalTexture = mapTexture(directory / kNormal);
    material.metallicRoughnessTexture = mapTexture(directory / kRoughness);
    material.roughnessFactor = 1.0f;
    // The map is a lone grayscale roughness, and glTF reads metal from the blue
    // channel of this slot - which here holds the same roughness value. Left at one,
    // every ceramic tile would shade as polished metal.
    material.metallicFactor = 0.0f;

    // Grout lines are recessed and the tiles are raised, which is exactly what the
    // height map encodes; the scale is a fraction of the UV tile, so keeping it small
    // stops the displacement from reading as a detached relief at grazing angles.
    material.heightTexture = mapTexture(directory / kHeight);
    material.heightScale = 0.02f;
    return material;
}
