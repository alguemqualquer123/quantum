// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMapUtils.h"

#include "Out.h"
#include "util/TerrainMapFileWriter.h"

#include <cstring>
#include <system_error>

using namespace quantum;
using namespace quantum::editor;

Texture* editor::TerrainMapUtils::findTexture(TerrainComponent& terrain, const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::HeightMap){
        return &terrain.heightMap;
    }
    if (ref.target == TerrainMapTarget::BlendMap){
        if (ref.layer < 0 || ref.layer >= static_cast<int>(terrain.blendMaps.size())){
            return NULL;
        }
        return &terrain.blendMaps[ref.layer];
    }
    if (ref.layer < 0 || ref.layer >= static_cast<int>(terrain.foliageLayers.size())){
        return nullptr;
    }
    return &terrain.foliageLayers[ref.layer].densityMap;
}

std::string editor::TerrainMapUtils::getPropertyName(const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::HeightMap){
        return "heightMap";
    }
    if (ref.target == TerrainMapTarget::BlendMap){
        return "blendMaps[" + std::to_string(ref.layer) + "]";
    }
    return "foliageLayers[" + std::to_string(ref.layer) + "].densityMap";
}

std::string editor::TerrainMapUtils::getContainerPropertyName(const TerrainMapRef& ref){
    if (ref.target == TerrainMapTarget::BlendMap){
        return "blendMaps";
    }
    if (ref.target == TerrainMapTarget::DensityMap){
        return "foliageLayers";
    }
    return std::string();
}

bool editor::TerrainMapUtils::hasLoadedData(Texture& texture){
    if (texture.empty() || texture.isFramebuffer()){
        return false;
    }

    texture.setReleaseDataAfterLoad(false);
    TextureLoadResult result = texture.load();
    return result.state == ResourceLoadState::Finished && result.data && texture.getData().getData();
}

bool editor::TerrainMapUtils::writeFile(Project* project, const std::string& relativePath, int width, int height, int channels, int bytesPerChannel, const std::vector<unsigned char>& pixels){
    const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
    if (!project || project->getProjectPath().empty() || relativePath.empty() || width <= 0 || height <= 0 || channels <= 0 || pixels.size() < expectedSize){
        return false;
    }

    fs::path outputPath = project->resolveAssetPath(relativePath);

    std::error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    if (ec){
        Out::warning("Failed to create terrain texture directory: %s", outputPath.parent_path().string().c_str());
        return false;
    }

    // Encode + write on the background worker: full-map PNG writes (especially
    // 16-bit heightmaps) are slow enough to hitch the editor on every stroke end.
    TerrainMapFileWriter::get().enqueue(outputPath, width, height, channels, bytesPerChannel,
        std::vector<unsigned char>(pixels.begin(), pixels.begin() + expectedSize));
    return true;
}

void editor::TerrainMapUtils::refresh(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, const TerrainMapRegion& dirtyRegion){
    if (!sceneProject){
        return;
    }
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!terrain){
        return;
    }

    if (ref.target == TerrainMapTarget::HeightMap){
        // The painted layers read the same texture, so the upload is needed either way.
        terrain->needUpdateTexture = true;
        // Instances sit on the surface, so a sculpt moves them.
        terrain->needUpdateFoliage = true;

        if (dirtyRegion.empty() || !terrain->heightMapLoaded){
            terrain->heightMapLoaded = false;
            terrain->needUpdateTerrain = true;
        }else{
            // A tree is already built: only the leaves under the brush need new height ranges.
            // Rebuilding reads every texel of a map that can be 4096 wide, which is what made
            // each sculpt click hang for a second.
            markTerrainHeightmapDirty(*terrain, dirtyRegion);
        }
    }else if (ref.target == TerrainMapTarget::BlendMap){
        terrain->needUpdateTexture = true;
    }else{
        // Only this foliage layer's density map changed, and only inside the stamp's texels:
        // hand the engine the rect so it refills just the chunks under it. An empty rect is
        // the whole-map case (map created, resolution changed, replaced from a snapshot) and
        // keeps invalidating every chunk, as before.
        if (dirtyRegion.empty()){
            terrain->needUpdateFoliage = true;
        }else{
            markTerrainFoliageDensityDirty(*terrain, ref.layer, dirtyRegion);
        }
    }

    if (Texture* texture = findTexture(*terrain, ref)){
        texture->invalidateRender();
    }
}

std::vector<unsigned char> editor::TerrainMapUtils::copyRegion(const unsigned char* pixels, int mapWidth, int bytesPerTexel, const TerrainMapRegion& region){
    const size_t rowBytes = static_cast<size_t>(region.width()) * static_cast<size_t>(bytesPerTexel);
    std::vector<unsigned char> regionPixels(rowBytes * static_cast<size_t>(region.height()));

    for (int y = 0; y < region.height(); y++){
        const unsigned char* src = pixels + (static_cast<size_t>(region.minY + y) * mapWidth + region.minX) * bytesPerTexel;
        std::memcpy(regionPixels.data() + y * rowBytes, src, rowBytes);
    }

    return regionPixels;
}

bool editor::TerrainMapUtils::writeRegion(unsigned char* pixels, int mapWidth, int bytesPerTexel, const TerrainMapRegion& region, const std::vector<unsigned char>& regionPixels){
    const size_t rowBytes = static_cast<size_t>(region.width()) * static_cast<size_t>(bytesPerTexel);
    if (!pixels || regionPixels.size() < rowBytes * static_cast<size_t>(region.height())){
        return false;
    }

    for (int y = 0; y < region.height(); y++){
        unsigned char* dst = pixels + (static_cast<size_t>(region.minY + y) * mapWidth + region.minX) * bytesPerTexel;
        std::memcpy(dst, regionPixels.data() + y * rowBytes, rowBytes);
    }

    return true;
}
