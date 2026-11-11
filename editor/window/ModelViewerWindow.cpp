// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "ModelViewerWindow.h"

#include "Backend.h"
#include "Engine.h"
#include "Log.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/ModelLoadCmd.h"
#include "component/MeshComponent.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/MeshSystem.h"
#include "util/FbxImporter.h"
#include "util/Util.h"
#include "window/Widgets.h"

#include <algorithm>
#include <cmath>

using namespace quantum;

namespace {
    // tan(22.5), the half of the 45 degree vertical field of view the preview camera uses
    constexpr float PREVIEW_HALF_TAN = 0.4142f;
    constexpr int MAX_PREVIEW_DIMENSION = 4096;
    constexpr float ORBIT_SPEED = 0.01f;
    constexpr float PITCH_LIMIT = 1.5f;

    const char* importStateName(editor::FbxImporter::State state) {
        switch (state) {
            case editor::FbxImporter::State::UpToDate: return "up to date";
            case editor::FbxImporter::State::Missing: return "not imported yet";
            case editor::FbxImporter::State::Stale: return "out of date";
        }
        return "unknown";
    }
}

editor::ModelViewerWindow::ModelViewerWindow(Project* project) : project(project) {
}

editor::ModelViewerWindow::~ModelViewerWindow() {
    closeAll();
}

std::filesystem::path editor::ModelViewerWindow::resolveFilepath(const std::filesystem::path& path) const {
    if (path.is_absolute()) {
        return path;
    }
    return project->getProjectPath() / path;
}

std::string editor::ModelViewerWindow::toRelativePath(const std::filesystem::path& path) const {
    const std::filesystem::path normalizedPath = path.lexically_normal();
    if (normalizedPath.is_relative() || project->getProjectPath().empty()) {
        return normalizedPath.string();
    }

    std::error_code ec;
    std::filesystem::path relativePath = std::filesystem::relative(
        normalizedPath,
        project->getProjectPath(),
        ec
    );
    return (!ec && !relativePath.empty()) ? relativePath.lexically_normal().string()
                                          : normalizedPath.string();
}

std::string editor::ModelViewerWindow::getWindowId(const std::filesystem::path& relativePath) {
    return "###ModelViewer:" + relativePath.lexically_normal().string();
}

void editor::ModelViewerWindow::clearInstance(Instance& instance) {
    // The preview scene is registered with the engine by the one-shot render, so it has
    // to leave that list before its Scene is destroyed under the renderer.
    Scene* scene = instance.render.getScene();
    if (scene) {
        Engine::removeScene(scene);
    }
    instance.render.clearScene();
    instance.framed = false;
    instance.frameWidth = 0;
    instance.frameHeight = 0;
}

bool editor::ModelViewerWindow::ensureImported(Instance& instance, const std::filesystem::path& fullPath) {
    instance.notice.clear();

    if (!FbxImporter::isFbxFile(fullPath)) {
        return fs::exists(fullPath);
    }

    if (!project->importFbxModel(fullPath)) {
        instance.loadError = "The FBX could not be converted. See the Output window for the ufbx report.";
        return false;
    }

    const std::string glbPath = MeshSystem::getImportedModelPath(fullPath.string());
    const std::vector<std::filesystem::path> missing = FbxImporter::findMissingLinkedFiles(fullPath, glbPath);
    if (!missing.empty()) {
        std::string names;
        for (size_t i = 0; i < missing.size() && i < 6; i++) {
            if (!names.empty()) names += ", ";
            names += missing[i].filename().string();
        }
        if (missing.size() > 6) names += ", ...";
        instance.notice = "Missing textures the converted model links: " + names +
                ". Put the files beside the FBX or inside the project's assets and Reimport.";
    }

    return true;
}

void editor::ModelViewerWindow::loadModel(Instance& instance) {
    const std::filesystem::path fullPath = resolveFilepath(instance.filepath);
    instance.loadError.clear();

    clearInstance(instance);

    if (!fs::exists(fullPath)) {
        instance.loadError = "The file is no longer on disk.";
        return;
    }

    if (!ensureImported(instance, fullPath)) {
        return;
    }

    if (!instance.render.loadModel(fullPath.string())) {
        instance.loadError = "The engine could not load this model.";
        Log::error("Model viewer failed to load '%s'", fullPath.string().c_str());
        return;
    }

    // A model with no materials at all renders black, which reads as a broken preview
    instance.render.fixDarkMaterials();

    frameModel(instance);

    Scene* scene = instance.render.getScene();
    if (auto meshes = scene ? scene->getComponentArray<MeshComponent>() : nullptr) {
        instance.meshCount = 0;
        for (int i = 0; i < meshes->size(); i++) {
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            if (mesh.numSubmeshes > 0) {
                instance.meshCount++;
            }
        }
    }

    instance.needsRender = true;
}

void editor::ModelViewerWindow::frameModel(Instance& instance) {
    AABB bounds;
    Vector3 center(0.0f, 0.0f, 0.0f);
    Vector3 size(1.0f, 1.0f, 1.0f);
    if (instance.render.getModelBounds(bounds)) {
        center = bounds.getCenter();
        size = bounds.getSize();
    }

    instance.focus = center;
    instance.boundsSize = size;

    float maxDimension = std::max({size.x, size.y, size.z});
    if (maxDimension <= 0.001f) {
        maxDimension = 1.0f;
    }

    instance.radius = std::max(0.05f, maxDimension * 0.5f);
    instance.fitDistance = (maxDimension * 0.5f) / PREVIEW_HALF_TAN;
    instance.fitDistance = std::max(instance.fitDistance, maxDimension * 1.5f);
    instance.distance = instance.fitDistance;
    instance.yaw = 0.65f;
    instance.pitch = 0.32f;
    instance.framed = true;
    instance.needsRender = true;
}

void editor::ModelViewerWindow::openFile(const std::filesystem::path& path, bool dockToCentral) {
    const std::string key = toRelativePath(path);
    if (key.empty()) {
        return;
    }

    auto existing = instances.find(key);
    if (existing != instances.end()) {
        existing->second->focusRequested = dockToCentral;
        existing->second->needsReload = true;
        Backend::getApp().addModelViewerWindowToDock(existing->second->filepath, dockToCentral);
        return;
    }

    std::unique_ptr<Instance> instance = std::make_unique<Instance>();
    instance->filepath = std::filesystem::path(key);
    instance->focusRequested = dockToCentral;
    Instance& created = *instance;
    instances[key] = std::move(instance);

    loadModel(created);

    project->addTab(TabType::MODEL_VIEWER, key);
    project->saveWorkspaceFile();
    Backend::getApp().addModelViewerWindowToDock(created.filepath, dockToCentral);
}

void editor::ModelViewerWindow::closeFile(const std::filesystem::path& path) {
    const std::string key = toRelativePath(path);

    auto it = instances.find(key);
    if (it != instances.end()) {
        clearInstance(*it->second);
        instances.erase(it);
    }

    if (project->hasTab(TabType::MODEL_VIEWER, key)) {
        project->removeTab(TabType::MODEL_VIEWER, key);
        project->saveWorkspaceFile();
    }
}

void editor::ModelViewerWindow::closeAll() {
    for (auto& entry : instances) {
        clearInstance(*entry.second);
    }
    instances.clear();

    for (auto& retiredInstance : retired) {
        clearInstance(*retiredInstance);
    }
    retired.clear();
}

bool editor::ModelViewerWindow::isFileOpen(const std::filesystem::path& path) const {
    return instances.find(toRelativePath(path)) != instances.end();
}

void editor::ModelViewerWindow::reloadFile(const std::filesystem::path& path) {
    auto it = instances.find(toRelativePath(path));
    if (it != instances.end()) {
        it->second->needsReload = true;
    }
}

bool editor::ModelViewerWindow::handleFileRename(
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath
) {
    const std::string oldKey = toRelativePath(oldPath);
    const std::string newKey = toRelativePath(newPath);
    auto oldIt = instances.find(oldKey);
    if (oldIt == instances.end() || newKey.empty()) {
        return false;
    }

    if (auto newIt = instances.find(newKey); newIt != instances.end()) {
        clearInstance(*newIt->second);
        instances.erase(newIt);
    }

    auto node = instances.extract(oldIt);
    node.key() = newKey;
    node.mapped()->filepath = std::filesystem::path(newKey);
    node.mapped()->focusRequested = true;
    instances.insert(std::move(node));

    instances[newKey]->needsReload = true;

    project->removeTab(TabType::MODEL_VIEWER, oldKey);
    project->addTab(TabType::MODEL_VIEWER, newKey);
    project->saveWorkspaceFile();
    Backend::getApp().addModelViewerWindowToDock(std::filesystem::path(newKey));
    return true;
}

void editor::ModelViewerWindow::drawActions(Instance& instance, const std::filesystem::path& fullPath) {
    const bool isFbx = FbxImporter::isFbxFile(fullPath);
    const std::string suffix = getWindowId(instance.filepath);

    if (ImGui::Button((std::string(ICON_FA_ROTATE) + " Reimport" + suffix).c_str())) {
        if (isFbx) {
            if (project->importFbxModel(fullPath, true)) {
                Out::info("Reimported " + fullPath.string());
            }
        }
        loadModel(instance);
    }
    ImGui::SetItemTooltip(isFbx
        ? "Convert the FBX into a glTF again and reload this preview."
        : "Read the file from disk again.");

    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_FA_DOWN_LEFT_AND_UP_RIGHT_TO_CENTER) + " Fit" + suffix).c_str())) {
        frameModel(instance);
    }
    ImGui::SetItemTooltip("Frame the whole model");

    ImGui::SameLine();
    SceneProject* target = project->getScene(project->getSelectedSceneId());
    const bool canAdd = target && target->sceneType == SceneType::SCENE_3D &&
                        project->isInsideAssetsPath(fullPath);
    ImGui::BeginDisabled(!canAdd);
    if (ImGui::Button((std::string(ICON_FA_SQUARE_PLUS) + " Add to Scene" + suffix).c_str())) {
        std::string entityName = fullPath.stem().string();
        if (entityName.empty()) {
            entityName = "Model";
        }
        const std::string assetPath = project->normalizeToAssetsRelative(fullPath).generic_string();
        CommandHandle::get(target->id)->addCommandNoMerge(
            new ModelLoadCmd(project, target->id, entityName, Vector3(0.0f, 0.0f, 0.0f), assetPath));
        Out::info("Added " + assetPath + " to the selected scene.");
    }
    ImGui::EndDisabled();
    if (!canAdd) {
        if (target && target->sceneType != SceneType::SCENE_3D) {
            ImGui::SameLine();
            ImGui::TextDisabled("3D scene only");
        } else if (!project->isInsideAssetsPath(fullPath)) {
            ImGui::SameLine();
            ImGui::TextDisabled("not in the assets folder");
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    if (ImGui::SliderFloat(("Light" + suffix).c_str(),
            &instance.lightIntensity, 0.0f, 16.0f, "%.1f")) {
        instance.needsRender = true;
    }
}

void editor::ModelViewerWindow::drawViewport(Instance& instance, const std::filesystem::path& fullPath) {
    drawActions(instance, fullPath);
    ImGui::Separator();

    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(Theme::dpi(140.0f), canvasSize.y * 0.62f);
    canvasSize.x = std::max(1.0f, canvasSize.x);

    ImGui::InvisibleButton(("##ModelViewerCanvas" + getWindowId(instance.filepath)).c_str(), canvasSize);
    const ImVec2 canvasMin = ImGui::GetItemRectMin();
    const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    if (!instance.loadError.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + Theme::dpi(12.0f), canvasMin.y + Theme::dpi(12.0f)));
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Unable to preview this model");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", instance.loadError.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", fullPath.string().c_str());
        return;
    }

    if (!instance.framed) {
        frameModel(instance);
    }

    const int wantedWidth = std::clamp((int)std::lround(canvasSize.x * Theme::dpiScale()), 1, MAX_PREVIEW_DIMENSION);
    const int wantedHeight = std::clamp((int)std::lround(canvasSize.y * Theme::dpiScale()), 1, MAX_PREVIEW_DIMENSION);
    if (wantedWidth != instance.frameWidth || wantedHeight != instance.frameHeight) {
        instance.frameWidth = wantedWidth;
        instance.frameHeight = wantedHeight;
        instance.render.setFrameSize(wantedWidth, wantedHeight);
        instance.needsRender = true;
    }

    if (hovered || active) {
        const ImVec2 delta = io.MouseDelta;
        const bool orbiting = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
        const bool panning = active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f);

        if (orbiting) {
            instance.yaw -= delta.x * ORBIT_SPEED;
            instance.pitch = std::clamp(instance.pitch + delta.y * ORBIT_SPEED, -PITCH_LIMIT, PITCH_LIMIT);
            instance.needsRender = true;
        } else if (panning) {
            // the world per pixel at the current distance, so a drag keeps the model under the mouse
            const float perPixel = 2.0f * PREVIEW_HALF_TAN * instance.distance /
                    std::max((float)instance.frameHeight, 1.0f);
            const float sy = std::sin(instance.yaw), cy = std::cos(instance.yaw);
            const Vector3 right(cy, 0.0f, -sy);
            const Vector3 up(-sy * std::sin(instance.pitch), std::cos(instance.pitch), -cy * std::sin(instance.pitch));
            instance.focus = instance.focus - right * (delta.x * perPixel) + up * (delta.y * perPixel);
            instance.needsRender = true;
        }

        if (hovered && io.MouseWheel != 0.0f) {
            const float zoomed = instance.distance * std::pow(1.15f, io.MouseWheel);
            instance.distance = std::clamp(zoomed,
                    instance.fitDistance * MIN_ZOOM, instance.fitDistance * MAX_ZOOM);
            instance.needsRender = true;
        }

        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    if (instance.needsRender) {
        instance.render.setLightIntensity(instance.lightIntensity);
        instance.render.setCameraPose(instance.focus, instance.yaw, instance.pitch, instance.distance);
        Engine::executeSceneOnce(instance.render.getScene());
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_WindowBg));

    Texture preview = instance.render.getTexture();
    TextureRender* render = preview.getRender();
    if (render && render->isCreated()) {
        if (instance.needsRender) {
            instance.needsRender = false;
        }
        drawList->AddImage(Backend::getImGuiTexture(render), canvasMin, canvasMax);
        drawList->AddRect(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_Border));

        if (!instance.notice.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + Theme::dpi(10.0f), canvasMin.y + Theme::dpi(10.0f)));
            ImGui::PushTextWrapPos(canvasMax.x - Theme::dpi(10.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", instance.notice.c_str());
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + Theme::dpi(12.0f), canvasMin.y + Theme::dpi(12.0f)));
        ImGui::TextDisabled("Rendering the preview...");
    }
    drawList->PopClipRect();
}

void editor::ModelViewerWindow::drawMaterials(Instance& instance) {
    Scene* scene = instance.render.getScene();
    if (!scene || !instance.loadError.empty()) {
        return;
    }

    ImGui::TextDisabled("%s", resolveFilepath(instance.filepath).string().c_str());
    ImGui::Text("Meshes: %u   Size: %.2f x %.2f x %.2f", instance.meshCount,
            instance.boundsSize.x, instance.boundsSize.y, instance.boundsSize.z);

    if (!instance.notice.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", instance.notice.c_str());
    }

    if (FbxImporter::isFbxFile(resolveFilepath(instance.filepath))) {
        const std::string glbPath = MeshSystem::getImportedModelPath(resolveFilepath(instance.filepath).string());
        ImGui::Text("FBX import: %s", importStateName(
                FbxImporter::getState(resolveFilepath(instance.filepath), glbPath, project->getAssetsPath())));
    }

    auto meshes = scene->getComponentArray<MeshComponent>();
    if (!meshes || meshes->size() == 0) {
        return;
    }

    ImGui::Spacing();
    ImGui::Text("Materials");
    const float thumb = Theme::dpi(56.0f);

    auto slot = [thumb](const char* label, Texture& texture) {
        if (texture.empty()) {
            return;
        }
        TextureRender* render = texture.getRender();
        if (!render || !render->isCreated()) {
            return;
        }
        ImGui::BeginGroup();
        Widgets::image(Backend::getImGuiTexture(render), ImVec2(thumb, thumb), ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::TextDisabled("%s", label);
        ImGui::EndGroup();
    };

    for (int i = 0; i < meshes->size(); i++) {
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            Material& material = mesh.submeshes[s].material;
            ImGui::PushID(i * 32 + (int)s);

            const std::string title = material.name.empty()
                    ? ("Submesh " + std::to_string(i) + "." + std::to_string(s))
                    : material.name;
            if (ImGui::TreeNode(title.c_str())) {
                ImGui::ColorButton("baseColor", ImVec4(material.baseColorFactor.x, material.baseColorFactor.y,
                        material.baseColorFactor.z, 1.0f));
                ImGui::SameLine();
                ImGui::Text("metallic %.2f   roughness %.2f", material.metallicFactor, material.roughnessFactor);

                ImGui::BeginGroup();
                slot("base color", material.baseColorTexture);
                ImGui::SameLine();
                slot("normal", material.normalTexture);
                ImGui::SameLine();
                slot("metallic/rough", material.metallicRoughnessTexture);
                ImGui::SameLine();
                slot("occlusion", material.occlusionTexture);
                ImGui::SameLine();
                slot("emissive", material.emissiveTexture);
                ImGui::EndGroup();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void editor::ModelViewerWindow::show() {
    for (auto& retiredInstance : retired) {
        clearInstance(*retiredInstance);
    }
    retired.clear();

    for (auto& entry : instances) {
        if (entry.second->needsReload) {
            entry.second->needsReload = false;
            loadModel(*entry.second);
        }
    }

    for (auto it = instances.begin(); it != instances.end();) {
        Instance& instance = *it->second;

        if (instance.focusRequested) {
            ImGui::SetNextWindowFocus();
            instance.focusRequested = false;
        }
        ImGui::SetNextWindowSize(ImVec2(Theme::dpi(760.0f), Theme::dpi(560.0f)), ImGuiCond_FirstUseEver);

        std::string visibleTitle = instance.filepath.filename().string();
        if (visibleTitle.empty()) {
            visibleTitle = "Model Viewer";
        }
        const std::string windowTitle = visibleTitle + getWindowId(instance.filepath);

        const bool visible = ImGui::Begin(windowTitle.c_str(), &instance.isOpen);
        if (visible) {
            const std::filesystem::path fullPath = resolveFilepath(instance.filepath);
            drawViewport(instance, fullPath);

            ImGui::Separator();
            if (ImGui::BeginChild(("##ModelViewerInfo" + getWindowId(instance.filepath)).c_str(),
                    ImVec2(0.0f, 0.0f), true)) {
                drawMaterials(instance);
            }
            ImGui::EndChild();
        }
        ImGui::End();

        if (!instance.isOpen) {
            project->removeTab(TabType::MODEL_VIEWER, instance.filepath.string());
            retired.push_back(std::move(it->second));
            it = instances.erase(it);
            continue;
        }

        ++it;
    }
}
