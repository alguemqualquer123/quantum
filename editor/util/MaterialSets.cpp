// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/MaterialSets.h"

#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

using namespace quantum;
using namespace quantum::editor;

namespace {

    namespace fs = std::filesystem;

    std::string lowered(const std::string& text) {
        std::string out = text;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Split on the separators every library uses, keeping the pieces in order.
    std::vector<std::string> tokensOf(const std::string& stem) {
        std::vector<std::string> out;
        std::string current;
        for (char c : stem) {
            if (c == '_' || c == '-' || c == '.' || c == ' ') {
                if (!current.empty())
                    out.push_back(lowered(current));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty())
            out.push_back(lowered(current));
        return out;
    }

    bool isResolution(const std::string& token) {
        size_t digits = 0;
        while (digits < token.size() && std::isdigit(static_cast<unsigned char>(token[digits])))
            digits++;
        if (digits == 0)
            return false;

        const std::string unit = token.substr(digits);
        if (unit == "k" || unit == "px")
            return true;
        if (!unit.empty())
            return false;

        // A bare number is a size from 32 up - the exported resolutions of the PBR
        // libraries and the Android densities alike. Below that it is a variant index
        // ("rock_02", "brick_wall_01"), which is part of the name.
        const long value = std::strtol(token.c_str(), nullptr, 10);
        return value >= 32 && value <= 16384;
    }

    // Drops every resolution token, wherever it sits: "_1k" and "_4096" trail the map
    // token in "brick_wall_01_nor_4096", and "ic_launcher_256" carries its size in the
    // middle of nothing at all. A bare trailing number is kept: "rock_02" is a variant.
    void stripResolution(std::vector<std::string>& tokens) {
        // Signed so the walk stays inside the vector after a two-element erase: an
        // unsigned index that has already passed the end reads out of range and the
        // debug CRT stops the editor on a "vector subscript out of range" assert.
        for (int i = static_cast<int>(tokens.size()) - 1; i >= 0; i--) {
            // "0_5k" arrives as ["0","5k"]: the half-kilopixel pair is only a size when
            // the leading piece is 0 or 1, and both halves go together.
            const bool halfK = (tokens[i] == "5k" || tokens[i] == "1k" || tokens[i] == "2k") &&
                               i > 0 && (tokens[i - 1] == "0" || tokens[i - 1] == "1");
            if (halfK) {
                tokens.erase(tokens.begin() + i - 1);
                tokens.erase(tokens.begin() + i - 1);
                i--;              // the pair took two slots, so skip one here as well
                continue;
            }
            if (isResolution(tokens[i]))
                tokens.erase(tokens.begin() + i);
        }
    }

    struct TokenMap {
        const char* token;
        int tokens;
        MaterialMapKind kind;
    };

    // Longest match wins, so "metallic_roughness" is never read as "roughness" with a
    // stray "metallic" left in the name.
    const TokenMap kTokens[] = {
        { "base_color", 2, MaterialMapKind::BaseColor },
        { "normal_gl", 2, MaterialMapKind::Normal },
        { "nor_gl", 2, MaterialMapKind::Normal },
        { "normal_dx", 2, MaterialMapKind::NormalDX },
        { "nor_dx", 2, MaterialMapKind::NormalDX },
        { "metallic_roughness", 2, MaterialMapKind::MetallicRoughness },
        { "rough_metallic", 2, MaterialMapKind::MetallicRoughness },
        { "ambient_occlusion", 2, MaterialMapKind::Occlusion },

        { "diff", 1, MaterialMapKind::BaseColor },
        { "dif", 1, MaterialMapKind::BaseColor },
        { "diffuse", 1, MaterialMapKind::BaseColor },
        { "albedo", 1, MaterialMapKind::BaseColor },
        { "basecolor", 1, MaterialMapKind::BaseColor },
        { "color", 1, MaterialMapKind::BaseColor },
        { "colour", 1, MaterialMapKind::BaseColor },
        { "col", 1, MaterialMapKind::BaseColor },

        { "nor", 1, MaterialMapKind::Normal },
        { "nrm", 1, MaterialMapKind::Normal },
        { "normal", 1, MaterialMapKind::Normal },
        { "normalgl", 1, MaterialMapKind::Normal },
        { "normaldx", 1, MaterialMapKind::NormalDX },

        { "rough", 1, MaterialMapKind::Roughness },
        { "roughness", 1, MaterialMapKind::Roughness },
        { "gloss", 1, MaterialMapKind::Roughness },

        { "met", 1, MaterialMapKind::Metallic },
        { "metal", 1, MaterialMapKind::Metallic },
        { "metallic", 1, MaterialMapKind::Metallic },

        { "mr", 1, MaterialMapKind::MetallicRoughness },
        { "metalrough", 1, MaterialMapKind::MetallicRoughness },

        { "ao", 1, MaterialMapKind::Occlusion },
        { "occ", 1, MaterialMapKind::Occlusion },
        { "occl", 1, MaterialMapKind::Occlusion },
        { "occlusion", 1, MaterialMapKind::Occlusion },

        { "arm", 1, MaterialMapKind::PackedAO },
        { "orm", 1, MaterialMapKind::PackedAO },
        { "mask", 1, MaterialMapKind::PackedAO },

        { "em", 1, MaterialMapKind::Emissive },
        { "emi", 1, MaterialMapKind::Emissive },
        { "emis", 1, MaterialMapKind::Emissive },
        { "emissive", 1, MaterialMapKind::Emissive },
        { "emission", 1, MaterialMapKind::Emissive },

        { "disp", 1, MaterialMapKind::Height },
        { "displacement", 1, MaterialMapKind::Height },
        { "height", 1, MaterialMapKind::Height },
        { "hgt", 1, MaterialMapKind::Height },
        { "bump", 1, MaterialMapKind::Height },
        { "ord", 1, MaterialMapKind::Height },
    };

    // Matches the tail of the name, so a map token in the middle of a name
    // ("ground_rock_diff") does not steal it.
    MaterialMapKind matchTail(const std::vector<std::string>& tokens, size_t& consumed) {
        for (const TokenMap& entry : kTokens) {
            const size_t need = static_cast<size_t>(entry.tokens);
            if (tokens.size() < need)
                continue;
            std::string tail;
            for (size_t i = tokens.size() - need; i < tokens.size(); i++) {
                if (!tail.empty())
                    tail += "_";
                tail += tokens[i];
            }
            if (tail == entry.token) {
                consumed = need;
                return entry.kind;
            }
        }
        consumed = 0;
        return MaterialMapKind::Unknown;
    }

    std::filesystem::path* slotFor(editor::MaterialSet& set, editor::MaterialMapKind kind) {
        using editor::MaterialMapKind;
        switch (kind) {
            case MaterialMapKind::BaseColor: return &set.baseColor;
            case MaterialMapKind::Normal: return &set.normal;
            case MaterialMapKind::NormalDX: return &set.normalDX;
            case MaterialMapKind::Roughness: return &set.roughness;
            case MaterialMapKind::Metallic: return &set.metallic;
            case MaterialMapKind::MetallicRoughness: return &set.metallicRoughness;
            case MaterialMapKind::Occlusion: return &set.occlusion;
            case MaterialMapKind::PackedAO: return &set.packedAO;
            case MaterialMapKind::Emissive: return &set.emissive;
            case MaterialMapKind::Height: return &set.height;
            case MaterialMapKind::Unknown: break;
        }
        return nullptr;
    }

}

int editor::MaterialSet::mapCount() const {
    int count = 0;
    for (const std::filesystem::path& file : files)
        if (!file.empty())
            count++;
    return count;
}

std::string editor::MaterialSet::kindLabel(const std::filesystem::path& file) const {
    switch (MaterialSets::classify(file.filename().string())) {
        case MaterialMapKind::BaseColor: return "Base color";
        case MaterialMapKind::Normal: return "Normal (OpenGL)";
        case MaterialMapKind::NormalDX: return "Normal (DirectX)";
        case MaterialMapKind::Roughness: return "Roughness";
        case MaterialMapKind::Metallic: return "Metallic";
        case MaterialMapKind::MetallicRoughness: return "Metallic-roughness";
        case MaterialMapKind::Occlusion: return "Ambient occlusion";
        case MaterialMapKind::PackedAO: return "Packed (AO / rough / metal)";
        case MaterialMapKind::Emissive: return "Emissive";
        case MaterialMapKind::Height: return "Height";
        case MaterialMapKind::Unknown: break;
    }
    return "Extra";
}

MaterialMapKind editor::MaterialSets::classify(const std::string& filename) {
    const std::string stem = lowered(std::filesystem::path(filename).stem().string());
    std::vector<std::string> tokens = tokensOf(stem);
    stripResolution(tokens);

    size_t consumed = 0;
    return matchTail(tokens, consumed);
}

std::string editor::MaterialSets::stemOf(const std::string& filename) {
    const std::filesystem::path path(filename);
    const std::string stem = path.stem().string();
    std::vector<std::string> tokens = tokensOf(stem);
    stripResolution(tokens);

    size_t consumed = 0;
    const MaterialMapKind kind = matchTail(tokens, consumed);
    if (kind != MaterialMapKind::Unknown && consumed <= tokens.size())
        tokens.resize(tokens.size() - consumed);

    if (tokens.empty())
        return stem;

    std::string out;
    for (const std::string& token : tokens) {
        if (!out.empty())
            out += "_";
        out += token;
    }
    return out;
}

std::vector<editor::MaterialSet> editor::MaterialSets::group(
        const std::vector<std::filesystem::path>& files) {
    std::vector<MaterialSet> sets;
    std::unordered_map<std::string, size_t> index;

    struct Claim {
        size_t set;
        MaterialMapKind kind;
        std::filesystem::path file;
    };
    std::vector<Claim> claims;

    for (const std::filesystem::path& file : files) {
        if (!Util::isImageFile(file.string()))
            continue;

        const std::filesystem::path directory = file.parent_path();
        const std::string name = file.filename().string();
        const std::string stem = stemOf(name);
        const std::string key = (directory / lowered(stem)).generic_string();

        const auto found = index.find(key);
        size_t slot;
        if (found == index.end()) {
            MaterialSet set;
            set.name = stem;
            set.directory = directory;
            slot = sets.size();
            index.emplace(key, slot);
            sets.push_back(std::move(set));
        } else {
            slot = found->second;
        }

        sets[slot].files.push_back(file);
        claims.push_back({ slot, classify(name), file });
    }

    // Named maps claim their slots before unnamed ones: a directory full of
    // "brick_diff/rough/nor" plus a stray "brick_preview" must not lose the base colour
    // to the preview, and which file iterated first is not a design decision.
    for (const Claim& claim : claims) {
        if (claim.kind == MaterialMapKind::Unknown)
            continue;
        std::filesystem::path* target = slotFor(sets[claim.set], claim.kind);
        if (target && target->empty())
            *target = claim.file;
    }
    for (const Claim& claim : claims) {
        if (claim.kind != MaterialMapKind::Unknown)
            continue;
        // A lone ground texture with no convention in its name is still a surface colour.
        if (sets[claim.set].baseColor.empty())
            sets[claim.set].baseColor = claim.file;
    }

    // A lone image whose name carries no map convention at all is just an image - an app
    // icon, a preview, a screenshot - and a folder of them is noise rather than a
    // material library. Two files in one set already say "maps of the same surface".
    std::vector<MaterialSet> plausible;
    for (MaterialSet& set : sets) {
        const bool named = std::any_of(set.files.begin(), set.files.end(),
            [](const std::filesystem::path& file) {
                return classify(file.filename().string()) != MaterialMapKind::Unknown;
            });
        if (set.files.size() >= 2 || named)
            plausible.push_back(std::move(set));
    }
    sets.swap(plausible);

    std::sort(sets.begin(), sets.end(), [](const MaterialSet& a, const MaterialSet& b) {
        return lowered(a.name) < lowered(b.name);
    });
    return sets;
}

Material editor::MaterialSets::buildMaterial(const MaterialSet& set) {
    Material material;
    material.name = set.name;

    if (!set.baseColor.empty())
        material.baseColorTexture = Texture(set.baseColor.string());

    // The site publishes both conventions; the engine shades with +Y up, so the
    // DirectX map is only used when nothing else exists.
    if (!set.normal.empty())
        material.normalTexture = Texture(set.normal.string());
    else if (!set.normalDX.empty())
        material.normalTexture = Texture(set.normalDX.string());

    const bool packed = !set.packedAO.empty();
    if (packed) {
        // arm and orm are the same three channels glTF asks of its two slots: red is
        // the occlusion, green the roughness, blue the metal.
        material.occlusionTexture = Texture(set.packedAO.string());
        material.metallicRoughnessTexture = Texture(set.packedAO.string());
    } else {
        if (!set.metallicRoughness.empty())
            material.metallicRoughnessTexture = Texture(set.metallicRoughness.string());
        if (!set.occlusion.empty())
            material.occlusionTexture = Texture(set.occlusion.string());
    }

    // Material only carries the glTF metallicRoughness slot, whose blue channel is the
    // metal. A lone roughness (or metallic) map would fill that channel with roughness
    // and turn every stone into a mirror, so it stays listed in the set but unbound.
    const bool hasRoughnessMap = !set.metallicRoughness.empty() || packed;
    const bool hasMetalMap = hasRoughnessMap;

    // A factor multiplies its map in glTF, so a map present means the factor stays at
    // one. With no maps the material is read as a dielectric surface, which is what
    // almost every ground, rock and foliage set actually is.
    material.roughnessFactor = hasRoughnessMap ? 1.0f : 0.6f;
    material.metallicFactor = hasMetalMap ? 1.0f : 0.0f;

    if (!set.emissive.empty()) {
        material.emissiveTexture = Texture(set.emissive.string());
        material.emissiveFactor = Vector3(1.0f, 1.0f, 1.0f);
    }
    if (!set.height.empty())
        material.heightTexture = Texture(set.height.string());

    return material;
}

std::filesystem::path editor::MaterialSets::engineMaterialsRoot() {
    std::error_code ec;

    const fs::path shipped = FileUtils::getEngineDir() / "assets" / "materials";
    if (fs::is_directory(shipped, ec)) {
        return shipped;
    }

    // copy_quantum_editor_engine only runs after a build; a checkout that has not built
    // yet still has the library in the source tree.
    fs::path dir = FileUtils::getExecutableDir();
    for (int depth = 0; depth <= 5 && !dir.empty(); ++depth) {
        const fs::path candidate = dir / "engine" / "assets" / "materials";
        if (fs::is_directory(candidate, ec)) {
            return candidate;
        }
        dir = dir.parent_path();
    }

    return {};
}

std::vector<editor::MaterialSet> editor::MaterialSets::engineSets() {
    const fs::path root = engineMaterialsRoot();
    std::vector<MaterialSet> sets;
    if (root.empty()) {
        return sets;
    }

    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(it->path());
        }
    }

    sets = group(files);
    // group() sorts inside a folder; across folders the folder name is the tie-breaker,
    // so a listing reads as "default/*" then "ground/*" rather than alphabetically mixed.
    std::sort(sets.begin(), sets.end(), [](const MaterialSet& a, const MaterialSet& b) {
        const int folder = a.directory.string().compare(b.directory.string());
        return folder < 0 || (folder == 0 && lowered(a.name) < lowered(b.name));
    });
    return sets;
}

editor::MaterialSet editor::MaterialSets::findEngineSet(const std::string& query) {
    const auto wanted = [](const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '_' || c == '-' || c == '.' || c == ' ') {
                continue;
            }
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };

    const std::string key = wanted(query);
    const std::vector<MaterialSet> sets = engineSets();

    MaterialSet best;
    size_t matches = 0;
    for (const MaterialSet& set : sets) {
        const std::string name = wanted(set.name);
        // Exact first, then a prefix, then "contains": ground_grass beats grass_leafy for
        // the query "grass" only because it is the shorter name, so rank by how much of
        // the name the query actually explains.
        if (name == key) {
            return set;
        }
        if (!key.empty() && name.rfind(key, 0) == 0) {
            best = set;
            matches++;
        }
    }
    if (matches > 0) {
        return best;
    }
    for (const MaterialSet& set : sets) {
        const std::string name = wanted(set.name);
        if (!key.empty() && name.find(key) != std::string::npos) {
            best = set;
            matches++;
        }
    }
    if (matches == 1) {
        return best;
    }
    return MaterialSet();
}

bool editor::MaterialSets::installSet(const MaterialSet& set, const fs::path& assetsRoot,
                                      std::vector<std::string>* installed, std::string* error) {
    if (set.directory.empty() || set.files.empty()) {
        if (error) {
            *error = "that material set has no files";
        }
        return false;
    }
    if (assetsRoot.empty()) {
        if (error) {
            *error = "no project assets directory";
        }
        return false;
    }

    const fs::path folder = set.directory.filename();
    const fs::path destination = assetsRoot / "materials" / folder;

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        if (error) {
            *error = "cannot create " + destination.string() + ": " + ec.message();
        }
        return false;
    }

    for (const fs::path& file : set.files) {
        std::error_code copyEc;
        fs::copy_file(file, destination / file.filename(), fs::copy_options::skip_existing, copyEc);
        if (copyEc) {
            if (error) {
                *error = "cannot copy " + file.filename().string() + " into "
                      + destination.string() + ": " + copyEc.message();
            }
            return false;
        }
        if (installed) {
            installed->push_back((fs::path("materials") / folder / file.filename()).generic_string());
        }
    }
    return true;
}
