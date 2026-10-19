// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATERIAL_H
#define MATERIAL_H

#include "math/Vector3.h"
#include "texture/Texture.h"

namespace doriax{

    // AUTO preserves Doriax's historical texture-alpha detection for materials
    // created in the editor. The other values map directly to glTF alphaMode.
    // The editor's generic enum property path stores enum values as int, matching
    // the engine's other editable enums.
    enum class MaterialAlphaMode {
        AUTO,
        ALPHA_OPAQUE,
        MASK,
        BLEND
    };

    struct DORIAX_API Material{
        // --- start shader part
        Vector4 baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);  // linear color
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float alphaCutoff = 0.5f;
        uint8_t _pad_28[4];
        Vector3 emissiveFactor = Vector3(0.0f, 0.0f, 0.0f);  // linear color
        float foliageTransmission = 0.0f; // thin-leaf backlighting, 0 disables
        // --- end shader part

        // --- start clearcoat (KHR_materials_clearcoat)
        float clearcoatFactor = 0.0f;
        float clearcoatRoughness = 0.0f;
        uint8_t _pad_clearcoat[8];
        // --- end clearcoat

        // --- start sheen (KHR_materials_sheen)
        Vector3 sheenColorFactor = Vector3(0.0f, 0.0f, 0.0f);  // linear color
        float sheenRoughnessFactor = 0.0f;
        // --- end sheen

        // --- start transmission (KHR_materials_transmission)
        float transmissionFactor = 0.0f;
        uint8_t _pad_transmission[12];
        // --- end transmission

        // --- start volume (KHR_materials_volume)
        Vector3 attenuationColor = Vector3(1.0f, 1.0f, 1.0f);
        float attenuationDistance = 0.0f;
        // --- end volume

        // --- start anisotropy (KHR_materials_anisotropy)
        float anisotropyStrength = 0.0f;
        float anisotropyRotation = 0.0f;  // radians
        uint8_t _pad_anisotropy[8];
        // --- end anisotropy

        // --- start height (parallax occlusion mapping)
        float heightScale = 0.05f;
        int heightMinLayers = 8;
        int heightMaxLayers = 32;
        uint8_t _pad_height[4];
        // --- end height

        Texture baseColorTexture;
        Texture emissiveTexture;
        Texture metallicRoughnessTexture;
        Texture occlusionTexture;
        Texture normalTexture;

        // --- new material textures
        Texture clearcoatTexture;          // clearcoat factor (r)
        Texture clearcoatRoughnessTexture; // clearcoat roughness (g)
        Texture clearcoatNormalTexture;    // clearcoat normal map
        Texture sheenColorTexture;         // sheen color (rgb)
        Texture sheenRoughnessTexture;     // sheen roughness (a)
        Texture transmissionTexture;       // transmission factor (r)
        Texture thicknessTexture;          // surface thickness (g) for volume
        Texture anisotropyTexture;         // anisotropy strength (g), rotation (b)
        Texture heightTexture;             // height map for parallax

        MaterialAlphaMode alphaMode = MaterialAlphaMode::AUTO;

        // glTF texCoord index (UV set) each texture samples: 0 = primary (a_texcoord1),
        // 1 = secondary (a_texcoord2). Only two sets are supported; higher indices are
        // clamped to 1 on load. Not part of the shader UBO prefix; uploaded separately.
        int baseColorTexCoord = 0;
        int metallicRoughnessTexCoord = 0;
        int occlusionTexCoord = 0;
        int emissiveTexCoord = 0;
        int normalTexCoord = 0;

        // additional tex coord sets for new textures
        int clearcoatTexCoord = 0;
        int clearcoatRoughnessTexCoord = 0;
        int clearcoatNormalTexCoord = 0;
        int sheenColorTexCoord = 0;
        int sheenRoughnessTexCoord = 0;
        int transmissionTexCoord = 0;
        int thicknessTexCoord = 0;
        int anisotropyTexCoord = 0;
        int heightTexCoord = 0;

        // material name or file
        std::string name = "";

        bool operator == (const Material& other) const {
            return baseColorFactor == other.baseColorFactor &&
                   metallicFactor == other.metallicFactor &&
                   roughnessFactor == other.roughnessFactor &&
                   alphaCutoff == other.alphaCutoff &&
                   emissiveFactor == other.emissiveFactor &&
                   foliageTransmission == other.foliageTransmission &&
                   clearcoatFactor == other.clearcoatFactor &&
                   clearcoatRoughness == other.clearcoatRoughness &&
                   sheenColorFactor == other.sheenColorFactor &&
                   sheenRoughnessFactor == other.sheenRoughnessFactor &&
                   transmissionFactor == other.transmissionFactor &&
                   attenuationColor == other.attenuationColor &&
                   attenuationDistance == other.attenuationDistance &&
                   anisotropyStrength == other.anisotropyStrength &&
                   anisotropyRotation == other.anisotropyRotation &&
                   heightScale == other.heightScale &&
                   baseColorTexture == other.baseColorTexture &&
                   emissiveTexture == other.emissiveTexture &&
                   metallicRoughnessTexture == other.metallicRoughnessTexture &&
                   occlusionTexture == other.occlusionTexture &&
                   normalTexture == other.normalTexture &&
                   clearcoatTexture == other.clearcoatTexture &&
                   clearcoatRoughnessTexture == other.clearcoatRoughnessTexture &&
                   clearcoatNormalTexture == other.clearcoatNormalTexture &&
                   sheenColorTexture == other.sheenColorTexture &&
                   sheenRoughnessTexture == other.sheenRoughnessTexture &&
                   transmissionTexture == other.transmissionTexture &&
                   thicknessTexture == other.thicknessTexture &&
                   anisotropyTexture == other.anisotropyTexture &&
                   heightTexture == other.heightTexture &&
                   alphaMode == other.alphaMode &&
                   baseColorTexCoord == other.baseColorTexCoord &&
                   metallicRoughnessTexCoord == other.metallicRoughnessTexCoord &&
                   occlusionTexCoord == other.occlusionTexCoord &&
                   emissiveTexCoord == other.emissiveTexCoord &&
                   normalTexCoord == other.normalTexCoord &&
                   name == other.name;
        }

        bool operator != (const Material& other) const {
            return !(*this == other);
        }
    };

}

#endif //MATERIAL_H
