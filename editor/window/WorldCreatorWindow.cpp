// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "window/WorldCreatorWindow.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/WorldGenApplyCmd.h"
#include "util/PngWriter.h"
#include "util/TerrainMapUtils.h"
#include "window/Widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using namespace quantum;

namespace {

    const int PREVIEW_MAX_SIDE = 512;
    const int PRESET_COUNT = 5;

    // The bands a heightmap is drawn in: water, shore, lowland, rock, snow. The preview is the
    // generator's own numbers coloured by altitude, so it needs no art asset.
    struct RampStop {
        float height;
        unsigned char red, green, blue;
    };

    const RampStop RAMP[] = {
        { 0.00f,  18,  34,  62},
        { 0.28f,  26,  58,  96},
        { 0.34f, 120, 108,  74},
        { 0.44f,  66,  96,  52},
        { 0.62f,  84,  78,  66},
        { 0.78f, 116, 112, 110},
        { 1.00f, 236, 240, 244},
    };

    void rampColor(float height, float* red, float* green, float* blue) {
        const int stopCount = static_cast<int>(sizeof(RAMP) / sizeof(RAMP[0]));
        for (int i = 1; i < stopCount; i++) {
            const RampStop& b = RAMP[i];
            if (height > b.height && i != stopCount - 1) continue;

            const RampStop& a = RAMP[i - 1];
            const float span = b.height - a.height;
            const float t = span > 0.0f ? std::clamp(height - a.height, 0.0f, span) / span : 0.0f;
            *red = (a.red + (b.red - a.red) * t) / 255.0f;
            *green = (a.green + (b.green - a.green) * t) / 255.0f;
            *blue = (a.blue + (b.blue - a.blue) * t) / 255.0f;
            return;
        }
    }

    // TextureData takes a raw pointer the pixels have to outlive, so the buffer is malloc'd and the
    // texture is told it owns it - the same handover TerrainEditWindow::setOwnedTextureData does.
    void uploadPixels(Texture& texture, unsigned long long generation, int width, int height,
                      const std::vector<unsigned char>& rgba) {
        const size_t size = rgba.size();
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(size));
        if (!raw) return;
        std::memcpy(raw, rgba.data(), size);

        TextureData data(width, height, static_cast<unsigned int>(size), ColorFormat::RGBA, 4, raw);
        data.setDataOwned(false);
        texture.setData("editor:worldcreator:" + std::to_string(generation), data);
        texture.getData().setDataOwned(true);
    }

    NoiseType noiseTypeFromIndex(int index) {
        switch (index) {
            case 0: return NoiseType::PERLIN;
            case 2: return NoiseType::VALUE;
            default: return NoiseType::SIMPLEX;
        }
    }

    int noiseTypeIndex(NoiseType type) {
        switch (type) {
            case NoiseType::PERLIN: return 0;
            case NoiseType::VALUE: return 2;
            case NoiseType::SIMPLEX: break;
        }
        return 1;
    }

    void floatRow(const char* label, float* value, float step, const char* format) {
        ImGui::PushItemWidth(130.0f);
        ImGui::InputFloat(label, value, 0.0f, step, format);
        ImGui::PopItemWidth();
    }

    void intRow(const char* label, int* value, int step) {
        ImGui::PushItemWidth(130.0f);
        ImGui::InputInt(label, value, step);
        ImGui::PopItemWidth();
    }

    std::string presetList() {
        std::string entries;
        for (int index = 0; index < PRESET_COUNT; index++) {
            entries += TerrainGenerator::presetName(static_cast<TerrainPreset>(index));
            entries += '\0';
        }
        entries += '\0';
        return entries;
    }

}

editor::WorldCreatorWindow::WorldCreatorWindow(Project* project) : project(project) {
    params = TerrainGenerator::preset(TerrainPreset::ISLAND);
    params.size = 512;
}

editor::WorldCreatorWindow::~WorldCreatorWindow() {
    preview.destroy();
}

void editor::WorldCreatorWindow::show() {
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    const bool wasOpen = windowOpen;
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen) setOpen(false);
        return;
    }

    updateTarget();

    ImGui::BeginGroup();
    drawParameters();
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    drawPreview();
    drawTarget();
    ImGui::EndGroup();

    if (!message.empty()) {
        if (messageOk) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImVec4(0.45f, 0.80f, 0.45f, 1.0f)));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImVec4(0.92f, 0.55f, 0.30f, 1.0f)));
        }
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

void editor::WorldCreatorWindow::drawParameters() {
    ImGui::TextDisabled("Generation");

    const std::string presets = presetList();
    if (ImGui::Combo("Preset", &presetIndex, presets.c_str())) {
        applyPreset();
    }

    int seed = static_cast<int>(params.seed);
    intRow("Seed", &seed, 1);
    params.seed = seed < 0 ? 0u : static_cast<uint32_t>(seed);
    ImGui::SameLine();
    if (ImGui::Button("Random")) {
        std::random_device device;
        params.seed = device();
    }

    int erosionSeed = static_cast<int>(params.erosionSeed);
    intRow("Erosion seed", &erosionSeed, 1);
    params.erosionSeed = erosionSeed < 0 ? 0u : static_cast<uint32_t>(erosionSeed);

    intRow("Size", &params.size, 16);
    const int minimum = TerrainGenerator::MIN_SIZE;
    const int maximum = TerrainGenerator::MAX_SIZE;
    params.size = std::clamp(params.size, minimum, maximum);
    const int effective = TerrainGenerator::clampedSize(params.size);
    if (effective != params.size) {
        ImGui::TextDisabled("clamped to %d", effective);
    }

    int noiseIndex = noiseTypeIndex(params.type);
    const std::string noises = "perlin\0simplex\0value\0";
    if (ImGui::Combo("Noise", &noiseIndex, noises.c_str())) {
        params.type = noiseTypeFromIndex(noiseIndex);
    }

    intRow("Octaves", &params.octaves, 1);
    floatRow("Lacunarity", &params.lacunarity, 0.1f, "%.2f");
    floatRow("Gain", &params.gain, 0.05f, "%.2f");
    floatRow("Frequency", &params.frequency, 0.001f, "%.4f");
    floatRow("Base", &params.baseHeight, 0.01f, "%.3f");
    floatRow("Amplitude", &params.amplitude, 0.01f, "%.3f");
    floatRow("Relief curve", &params.reliefCurve, 0.05f, "%.2f");
    floatRow("Ridged", &params.ridgedAmount, 0.05f, "%.2f");
    floatRow("Warp", &params.warpAmplitude, 0.1f, "%.2f");
    floatRow("Warp frequency", &params.warpFrequency, 0.05f, "%.2f");
    floatRow("Sea level", &params.seaLevel, 0.01f, "%.3f");
    floatRow("Island falloff", &params.islandFalloff, 0.05f, "%.2f");

    ImGui::Spacing();
    ImGui::TextDisabled("Erosion");
    intRow("Droplets", &params.erosionDroplets, 500);
    floatRow("Height scale", &params.erosionHeightScale, 10.0f, "%.0f");
    floatRow("Talus", &params.thermalTalus, 0.005f, "%.3f");
    floatRow("Thermal rate", &params.thermalRate, 0.05f, "%.2f");

    ImGui::Spacing();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText("Name", nameInput, sizeof(nameInput));
    ImGui::PopItemWidth();

    if (ImGui::Button("Generate")) {
        generate();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%dx%d", TerrainGenerator::clampedSize(params.size),
                        TerrainGenerator::clampedSize(params.size));
}

void editor::WorldCreatorWindow::drawPreview() {
    ImGui::TextDisabled("Preview");

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float side = std::max(64.0f, std::min(available.x, available.y * 0.62f));
    const ImVec2 from = ImGui::GetCursorScreenPos();
    const ImVec2 to(from.x + side, from.y + side);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(from, to, IM_COL32(11, 12, 14, 255));

    TextureRender* render = preview.getRender();
    if (hasMap && render && render->isCreated()) {
        drawList->AddImage(Backend::getImGuiTexture(render), from, to);
    } else {
        ImGui::SetCursorScreenPos(ImVec2(from.x + 8.0f, from.y + 8.0f));
        ImGui::TextDisabled(hasMap ? "The preview texture is not ready yet."
                                   : "Generate a map to see it here.");
    }

    ImGui::SetCursorScreenPos(ImVec2(from.x, to.y + 6.0f));

    if (hasMap) {
        ImGui::Text("min %.3f   mean %.3f   max %.3f", lowest, average, highest);
        ImGui::Text("%.1f%% below the water line (%.3f)", submerged * 100.0f, params.seaLevel);
        if (!writtenPath.empty()) ImGui::TextDisabled("file: %s", writtenPath.c_str());
    }
}

void editor::WorldCreatorWindow::drawTarget() {
    ImGui::Spacing();
    ImGui::TextDisabled("Target");

    if (!hasTarget) {
        ImGui::TextDisabled("Select one Terrain entity in a stopped scene to apply the map.");
        return;
    }

    SceneProject* sceneProject = project->getScene(targetSceneId);
    TerrainComponent* terrain = sceneProject && sceneProject->scene
        ? sceneProject->scene->findComponent<TerrainComponent>(targetEntity)
        : nullptr;
    if (!terrain) {
        ImGui::TextDisabled("The selected entity lost its terrain component.");
        return;
    }

    ImGui::Text("entity %u   size %.1f   maxHeight %.1f", targetEntity, terrain->terrainSize,
                terrain->maxHeight);
    ImGui::Text("resolution %.0f   levels %d   root grid %d", terrain->resolution, terrain->levels,
                terrain->rootGridSize);
    ImGui::TextDisabled("water line at y = %.2f",
                        TerrainGenerator::waterHeight(params, terrain->maxHeight));

    const std::string current = terrain->heightMap.getPath(0);
    ImGui::TextWrapped("current map: %s", current.empty() ? "(none)" : current.c_str());

    ImGui::Spacing();
    if (ImGui::Button("Apply to terrain")) {
        applyToTarget();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("undo restores the previous file");
}

void editor::WorldCreatorWindow::applyPreset() {
    const TerrainPreset preset = static_cast<TerrainPreset>(std::clamp(presetIndex, 0, PRESET_COUNT - 1));
    const uint32_t seed = params.seed;
    const uint32_t erosionSeed = params.erosionSeed;
    const int size = params.size;

    params = TerrainGenerator::preset(preset);
    params.seed = seed;
    params.erosionSeed = erosionSeed;
    params.size = size;
}

void editor::WorldCreatorWindow::generate() {
    if (!project || project->getProjectPath().empty()) {
        setMessage(false, "Open a project first: the heightmap is written into its terrain maps.");
        return;
    }

    const fs::path directory = project->getTerrainMapsDir();
    std::error_code ec;
    fs::create_directories(directory, ec);

    std::string writtenFile;
    std::string error;
    HeightmapStats stats;
    const bool written = writeHeightmap(directory, mapFileName(), params, writtenFile, stats,
                                        &heights, error);

    mapSize = TerrainGenerator::clampedSize(params.size);
    hasMap = heights.size() == static_cast<size_t>(mapSize) * static_cast<size_t>(mapSize);

    if (!written) {
        setMessage(false, "%s", error.c_str());
        return;
    }

    std::error_code relativeError;
    const fs::path relative = fs::relative(fs::path(writtenFile), project->getAssetsPath(), relativeError);
    writtenPath = (!relativeError && !relative.empty()) ? relative.generic_string() : writtenFile;

    lowest = stats.lowest;
    average = stats.average;
    highest = stats.highest;
    submerged = stats.submerged;
    rebuildPreview();

    setMessage(true, "wrote %s (%d x %d)", writtenPath.c_str(), mapSize, mapSize);
    Out::success("World Creator: generated %s", writtenPath.c_str());
}

void editor::WorldCreatorWindow::applyToTarget() {
    if (writtenPath.empty()) {
        setMessage(false, "Generate a map before applying one.");
        return;
    }
    if (!hasTarget) {
        setMessage(false, "Select one Terrain entity in a stopped scene first.");
        return;
    }

    SceneProject* sceneProject = project->getScene(targetSceneId);
    if (!sceneProject || !sceneProject->scene) {
        setMessage(false, "The scene is gone.");
        return;
    }

    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(targetEntity);
    Texture* heightMap = terrain ? TerrainMapUtils::findTexture(
        *terrain, TerrainMapRef(TerrainMapTarget::HeightMap)) : nullptr;

    const std::string before = heightMap ? heightMap->getPath(0) : std::string();
    CommandHandle::get(targetSceneId)->addCommandNoMerge(
        new WorldGenApplyCmd(project, targetSceneId, targetEntity, before, writtenPath));

    setMessage(true, "applied %s to entity %u.", writtenPath.c_str(), targetEntity);
}

void editor::WorldCreatorWindow::updateTarget() {
    hasTarget = false;
    targetSceneId = 0;
    targetEntity = 0;
    if (!project) return;

    uint32_t sceneId = project->getSelectedSceneForProperties();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        sceneProject = project->getSelectedScene();
    }
    if (!sceneProject || !sceneProject->scene) return;
    if (sceneProject->playState != ScenePlayState::STOPPED) return;

    const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() != 1) return;
    if (!sceneProject->scene->findComponent<TerrainComponent>(selected[0])) return;

    hasTarget = true;
    targetSceneId = sceneProject->id;
    targetEntity = selected[0];
}

editor::WorldCreatorWindow::HeightmapStats
editor::WorldCreatorWindow::measureField(const std::vector<float>& field,
                                         const TerrainGenParams& generationParams) {
    HeightmapStats stats;
    stats.lowest = 1.0f;
    stats.highest = 0.0f;
    double sum = 0.0;
    long long under = 0;

    for (const float height : field) {
        stats.lowest = std::min(stats.lowest, height);
        stats.highest = std::max(stats.highest, height);
        sum += height;
        if (height < generationParams.seaLevel) under++;
    }

    stats.average = field.empty() ? 0.0f : static_cast<float>(sum / field.size());
    stats.submerged = field.empty() ? 0.0f
                                   : static_cast<float>(under) / static_cast<float>(field.size());
    return stats;
}

bool editor::WorldCreatorWindow::writeHeightmap(const fs::path& directory, const std::string& fileName,
                                               const TerrainGenParams& generationParams,
                                               std::string& writtenFile, HeightmapStats& stats,
                                               std::vector<float>* heights, std::string& error) {
    error.clear();
    writtenFile.clear();
    stats = HeightmapStats();

    std::error_code ec;
    fs::create_directories(directory, ec);
    const fs::path file = directory / fileName;
    const int size = TerrainGenerator::clampedSize(generationParams.size);

    std::vector<float> field;
    // Heights and pixels come from one pass rather than through generatePng, which would run the
    // whole field a second time to fill the preview and the statistics.
    TerrainGenerator::generate(generationParams, field);

    std::vector<unsigned char> pixels;
    TerrainGenerator::encode16(field, pixels);
    const bool written = writeGray16Png(file, size, size, pixels.data(), pixels.size());

    stats = measureField(field, generationParams);
    if (heights) heights->swap(field);

    if (!written) {
        error = "cannot write " + file.string();
        return false;
    }

    writtenFile = file.generic_string();
    return true;
}

void editor::WorldCreatorWindow::measure() {
    const HeightmapStats stats = measureField(heights, params);
    lowest = stats.lowest;
    average = stats.average;
    highest = stats.highest;
    submerged = stats.submerged;
}

void editor::WorldCreatorWindow::rebuildPreview() {
    if (!hasMap || mapSize <= 0) return;

    const int step = std::max(1, (mapSize + PREVIEW_MAX_SIDE - 1) / PREVIEW_MAX_SIDE);
    const int width = mapSize / step;
    const int height = mapSize / step;
    if (width <= 0 || height <= 0) return;

    // One texel of slope in a 512-wide map is a long way, so the shading delta is scaled by the
    // step: down-sampling must not flatten the relief it is supposed to show.
    const float relief = 2.0f * step;
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const auto at = [this](int cx, int cy) {
                const int sx = std::clamp(cx, 0, mapSize - 1);
                const int sy = std::clamp(cy, 0, mapSize - 1);
                return heights[static_cast<size_t>(sy) * mapSize + sx];
            };

            const int sourceX = x * step;
            const int sourceY = y * step;
            const float sample = at(sourceX, sourceY);

            const float dzdx = (at(sourceX - step, sourceY) - at(sourceX + step, sourceY)) * relief;
            const float dzdy = (at(sourceX, sourceY - step) - at(sourceX, sourceY + step)) * relief;
            const float length = std::sqrt(dzdx * dzdx + dzdy * dzdy + 1.0f);

            // Light from the north-west, the convention every hillshade map uses.
            const float lambert = std::max(0.0f, (dzdx * 0.50f + dzdy * 0.50f + 0.71f) / length);
            const float shade = 0.45f + 0.85f * lambert;

            float red = 0.0f, green = 0.0f, blue = 0.0f;
            rampColor(sample, &red, &green, &blue);

            const size_t index = (static_cast<size_t>(y) * width + x) * 4u;
            rgba[index + 0] = static_cast<unsigned char>(std::clamp(red * shade, 0.0f, 1.0f) * 255.0f);
            rgba[index + 1] = static_cast<unsigned char>(std::clamp(green * shade, 0.0f, 1.0f) * 255.0f);
            rgba[index + 2] = static_cast<unsigned char>(std::clamp(blue * shade, 0.0f, 1.0f) * 255.0f);
            rgba[index + 3] = 255;
        }
    }

    preview.destroy();
    previewGeneration++;
    uploadPixels(preview, previewGeneration, width, height, rgba);
}

std::string editor::WorldCreatorWindow::mapFileName() const {
    // The name comes from a text box and ends up as a file inside the project, so only letters,
    // digits, dash and underscore survive.
    std::string stem;
    for (const char c : std::string(nameInput)) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (keep) stem += c;
        if (stem.size() >= 64) break;
    }
    if (stem.empty()) stem = "worldgen";

    char text[128];
    std::snprintf(text, sizeof(text), "%s_%s_%u.png", stem.c_str(),
                  TerrainGenerator::presetName(static_cast<TerrainPreset>(
                      std::clamp(presetIndex, 0, PRESET_COUNT - 1))),
                  params.seed);
    return std::string(text);
}

void editor::WorldCreatorWindow::setMessage(bool ok, const char* format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    message = text;
    messageOk = ok;
}

void editor::WorldCreatorWindow::setOpen(bool open) {
    if (open == windowOpen) return;
    windowOpen = open;
    if (open) focusRequested = true;
}

bool editor::WorldCreatorWindow::isOpen() const {
    return windowOpen;
}

void editor::WorldCreatorWindow::focus() {
    setOpen(true);
}
