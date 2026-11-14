// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "window/MaterialBrowserWindow.h"

#include "App.h"
#include "Backend.h"
#include "Engine.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "pool/TexturePool.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/GraphicUtils.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <system_error>

using namespace quantum;
using namespace quantum::editor;

namespace {

    namespace fs = std::filesystem;

    // Fixed on purpose: the grid can be zoomed without re-rendering every sphere, and
    // the cache stays valid across sessions, window sizes and machines.
    constexpr int THUMB_PX = 256;
    constexpr size_t MAX_LOADED_THUMBS = 384;

    // A few seconds at 60fps: the maps of one material are decoded on a worker thread, and
    // a file the decoder rejects must leave the queue instead of wedging it.
    constexpr int MAX_WARM_FRAMES = 240;

    // Bumped whenever the preview renderer changes what a finished sphere looks like, so
    // PNGs written by an older renderer are ignored instead of served from the cache
    // forever. 2: the maps are now waited for before the framebuffer is read back, and a
    // textureless white sphere used to be cached as if it were the real material.
    constexpr int PREVIEW_REVISION = 2;

    // Folders that are either generated, huge, or somebody else's export.
    const char* const kSkipDirs[] = { ".git", ".quantum", "node_modules", "thumbs",
                                      "build", "export", "exports", "Intermediate", "Binaries" };

    std::string lowered(const std::string& text) {
        std::string out = text;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::string ellipsize(const std::string& text, float maxWidth) {
        if (maxWidth <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
            return text;
        std::string cut;
        for (char c : text) {
            cut.push_back(c);
            if (ImGui::CalcTextSize((cut + "...").c_str()).x > maxWidth) {
                cut.pop_back();
                break;
            }
        }
        return cut + "...";
    }

    void fnvMix(uint64_t& hash, const std::string& text) {
        for (unsigned char c : text) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
    }

    // A map whose pixels have not reached the pool is drawn as the empty white fallback, so
    // a readback taken that early caches a textureless sphere. The inspector runs the same
    // test every frame it repaints a preview (Properties::getMaterialPreview).
    bool mapPending(const Texture& texture) {
        if (texture.isFramebuffer())
            return false;
        const std::string id = texture.getId();
        return !id.empty() && !TexturePool::get(id);
    }

    bool mapsPending(const Material& material) {
        return mapPending(material.baseColorTexture) || mapPending(material.normalTexture) ||
               mapPending(material.metallicRoughnessTexture) || mapPending(material.occlusionTexture) ||
               mapPending(material.emissiveTexture);
    }

}

editor::MaterialBrowserWindow::MaterialBrowserWindow(Project* project)
    : project(project) {
    cacheDir = App::getUserCacheBaseDir() / "quantum" / "material-previews";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    if (ec) {
        notice = "Preview cache is not writable: " + ec.message();
        Out::warning("Material library: cannot create %s (%s)", cacheDir.string().c_str(), ec.message().c_str());
    }

    cellSize = Theme::dpi(132.0f);

    // Both renders keep the studio look the thumbnails are judged in: one key light,
    // the embedded environment for reflections, and a ball fine enough to read as round.
    gridRender.setLightIntensity(lightIntensity);
    gridRender.setReceiveIBL(receiveIBL);
    heroRender.setLightIntensity(lightIntensity);
    heroRender.setReceiveIBL(receiveIBL);
    heroRender.setBackground(Vector4(0.045f, 0.047f, 0.055f, 1.0f));
}

editor::MaterialBrowserWindow::~MaterialBrowserWindow() {
    // The detached PNG writers hold a pointer to this window. They never touch it
    // outside the mutex, so retiring the generation and waiting for the count is enough.
    retireGeneration();
    std::unique_lock<std::mutex> lock(saveMutex);
    saveCondition.wait(lock, [this]() { return activeSaves == 0; });
}

void editor::MaterialBrowserWindow::setOpen(bool open) {
    windowOpen = open;
    if (open)
        rescanPending = true;
}

bool editor::MaterialBrowserWindow::isOpen() const {
    return windowOpen;
}

bool editor::MaterialBrowserWindow::hasActiveSave() {
    std::lock_guard<std::mutex> lock(saveMutex);
    return activeSaves > 0;
}

void editor::MaterialBrowserWindow::retireGeneration() {
    std::lock_guard<std::mutex> lock(saveMutex);
    ++generation;
}

fs::path editor::MaterialBrowserWindow::thumbnailPath(const std::string& key) const {
    return cacheDir / ("m" + key + ".png");
}

std::string editor::MaterialBrowserWindow::hashSet(const MaterialSet& set, float tiling) {
    uint64_t hash = 1469598103934665603ULL;
    fnvMix(hash, std::to_string(PREVIEW_REVISION));
    fnvMix(hash, set.name);
    fnvMix(hash, std::to_string(tiling));
    fnvMix(hash, std::to_string(THUMB_PX));

    std::error_code ec;
    for (const fs::path& file : set.files) {
        fnvMix(hash, file.generic_u8string());
        const auto size = fs::file_size(file, ec);
        fnvMix(hash, ec ? "?" : std::to_string(size));
        const auto time = fs::last_write_time(file, ec);
        fnvMix(hash, ec ? "?" : std::to_string(time.time_since_epoch().count()));
        ec.clear();
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

void editor::MaterialBrowserWindow::collectFrom(const fs::path& root, std::vector<fs::path>& out) const {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return;

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && out.size() < MAX_SOURCE_FILES; ++it) {
        ec.clear();
        const fs::path& path = it->path();

        if (it->is_directory(ec)) {
            const std::string name = lowered(path.filename().string());
            for (const char* skip : kSkipDirs) {
                if (name == lowered(skip)) {
                    it.disable_recursion_pending();
                    break;
                }
            }
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;

        // never browse the editor's own cache back into the library
        if (!cacheDir.empty() && path.string().rfind(cacheDir.string(), 0) == 0)
            continue;

        out.push_back(path);
    }
}

void editor::MaterialBrowserWindow::rescan() {
    rescanPending = false;
    notice.clear();

    std::vector<fs::path> files;
    if (project)
        // getAssetsDir() keeps what project.yaml says, which is "." for most projects
        // and would walk the folder the editor process started in (the engine copy
        // next to the exe). getAssetsPath() is the same value resolved against the
        // project folder.
        collectFrom(project->getAssetsPath(), files);
    const size_t projectCount = files.size();
    if (!externalDir.empty())
        collectFrom(externalDir, files);

    std::vector<MaterialSet> sets = MaterialSets::group(files);

    entries.clear();
    entries.reserve(sets.size());
    for (MaterialSet& set : sets) {
        Entry entry;
        entry.set = std::move(set);
        entry.key = hashSet(entry.set, tiling);
        entry.thumbPath = thumbnailPath(entry.key);
        std::error_code ec;
        entry.thumbReady = fs::exists(entry.thumbPath, ec) && fs::file_size(entry.thumbPath, ec) > 0;
        entries.push_back(std::move(entry));
    }

    selected = -1;
    applyFilter();

    if (projectCount && !externalDir.empty())
        notice = std::to_string(projectCount) + " project files, " +
                 std::to_string(files.size() - projectCount) + " from the folder";
    else if (files.size() >= MAX_SOURCE_FILES)
        notice = "Stopped scanning at " + std::to_string(MAX_SOURCE_FILES) + " files";
}

void editor::MaterialBrowserWindow::applyFilter() {
    visible.clear();
    const std::string needle = lowered(filter);
    for (size_t i = 0; i < entries.size(); i++) {
        if (needle.empty() || lowered(entries[i].set.name).find(needle) != std::string::npos ||
            lowered(entries[i].set.directory.string()).find(needle) != std::string::npos) {
            visible.push_back(i);
        }
    }
}

void editor::MaterialBrowserWindow::invalidateThumbs() {
    for (Entry& entry : entries) {
        entry.key = hashSet(entry.set, tiling);
        entry.thumbPath = thumbnailPath(entry.key);
        entry.thumbBusy = false;
        std::error_code ec;
        entry.thumbReady = fs::exists(entry.thumbPath, ec) && fs::file_size(entry.thumbPath, ec) > 0;
    }
    applyFilter();
}

editor::MaterialBrowserWindow::Entry* editor::MaterialBrowserWindow::entryByKey(const std::string& key) {
    if (key.empty())
        return nullptr;
    for (Entry& entry : entries) {
        if (entry.key == key)
            return &entry;
    }
    return nullptr;
}

void editor::MaterialBrowserWindow::stageNextThumb() {
    // One readback at a time: glReadPixels stalls the pipeline, and a page of them per
    // frame would make the editor unusable while the library warms up.
    if (hasActiveSave())
        return;

    // A rescan, a tiling change or a filter edit can retire the key this sphere was
    // rendered for. Let it go - its busy flag was reset with the keys.
    if (!stagedKey.empty() && !entryByKey(stagedKey)) {
        stagedKey.clear();
        stagedPath.clear();
        warmFrames = 0;
    }

    if (stagedKey.empty()) {
        Entry* target = nullptr;
        for (size_t i : visible) {
            Entry& entry = entries[i];
            if (!entry.thumbReady && !entry.thumbBusy) {
                target = &entry;
                break;
            }
        }
        if (!target)
            return;

        // Busy is never cleared on a failure: a material the renderer cannot draw would be
        // retried every frame otherwise. Any tiling, light or IBL change resets the flags
        // and gives it another go.
        target->thumbBusy = true;
        stagedKey = target->key;
        stagedPath = target->thumbPath;
        warmFrames = 0;

        gridRender.setTiling(tiling, tiling);
        gridRender.setFrameSize(THUMB_PX, THUMB_PX);
        gridRender.setLightIntensity(lightIntensity);
        gridRender.setReceiveIBL(receiveIBL);
        gridRender.applyMaterial(MaterialSets::buildMaterial(target->set));
        gridRender.setCameraPose(0.55f, 0.3f, 2.55f);
    }

    // A scene registered as one-time is dropped after the draw that used it, so it has to
    // be registered again every frame the maps are still arriving.
    Engine::executeSceneOnce(gridRender.getScene());

    // systemDraw() runs only on a frame that is really rendered, so an idling editor has
    // to be woken or neither the maps nor the sphere itself ever reach the framebuffer.
    Backend::getApp().requestRedraw();

    const Material applied = gridRender.getMaterial();
    if (mapsPending(applied)) {
        if (++warmFrames < MAX_WARM_FRAMES)
            return;
        Out::warning("Material library: timed out waiting for the maps of %s", stagedKey.c_str());
        stagedKey.clear();
        stagedPath.clear();
        warmFrames = 0;
        return;
    }

    stagePending = true;
}

void editor::MaterialBrowserWindow::processPendingPreviews() {
    if (!stagePending || hasActiveSave())
        return;

    const std::string key = stagedKey;
    const fs::path path = stagedPath;
    stagePending = false;
    stagedKey.clear();
    stagedPath.clear();
    warmFrames = 0;

    Framebuffer* framebuffer = gridRender.getFramebuffer();
    if (!framebuffer) {
        Out::warning("Material library: the preview of %s has no framebuffer", key.c_str());
        return;
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const unsigned long takenGeneration = generation;
    {
        std::lock_guard<std::mutex> lock(saveMutex);
        ++activeSaves;
    }

    const bool started = GraphicUtils::saveFramebufferImage(framebuffer, path, false,
        [this, key, takenGeneration]() {
            std::lock_guard<std::mutex> lock(saveMutex);
            if (takenGeneration == generation)
                finishedKeys.push_back(key);
            --activeSaves;
            saveCondition.notify_all();
        });

    if (!started) {
        std::lock_guard<std::mutex> lock(saveMutex);
        --activeSaves;
        Out::warning("Material library: could not read back the preview of %s", key.c_str());
    }
}

void editor::MaterialBrowserWindow::drainFinishedSaves() {
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(saveMutex);
        keys.swap(finishedKeys);
    }
    if (keys.empty())
        return;

    for (const std::string& key : keys) {
        for (Entry& entry : entries) {
            if (entry.key != key)
                continue;
            entry.thumbBusy = false;
            entry.thumbReady = true;
            // drop the GPU copy so the next draw loads the file that now exists
            thumbs.erase(entry.thumbPath.generic_u8string());
        }
    }
}

ImTextureID editor::MaterialBrowserWindow::thumbTexture(const fs::path& path) {
    const std::string id = path.generic_u8string();
    auto it = thumbs.find(id);
    if (it == thumbs.end()) {
        if (thumbs.size() >= MAX_LOADED_THUMBS)
            evictThumbs();

        Thumb loaded;
        loaded.texture = Texture(id, TextureData(id.c_str()));
        loaded.failed = !loaded.texture.load();
        it = thumbs.emplace(id, std::move(loaded)).first;
    }

    Thumb& thumb = it->second;
    thumb.lastUsedFrame = ImGui::GetFrameCount();
    if (thumb.failed)
        return ImTextureID{};

    TextureRender* render = thumb.texture.getRender();
    if (!render || !render->isCreated()) {
        thumb.failed = true;
        return ImTextureID{};
    }
    return Backend::getImGuiTexture(render);
}

void editor::MaterialBrowserWindow::evictThumbs() {
    unsigned long oldest = ~0UL;
    auto victim = thumbs.end();
    for (auto it = thumbs.begin(); it != thumbs.end(); ++it) {
        if (it->second.lastUsedFrame < oldest) {
            oldest = it->second.lastUsedFrame;
            victim = it;
        }
    }
    if (victim != thumbs.end())
        thumbs.erase(victim);
}

void editor::MaterialBrowserWindow::chooseFolder() {
    const std::string picked = FileDialogs::openFileDialog(
        externalDir.empty() ? std::string() : FileUtils::pathToUtf8(externalDir),
        FILE_DIALOG_ALL, true);
    if (picked.empty())
        return;
    externalDir = FileUtils::pathFromUtf8(picked);
    rescanPending = true;
}

void editor::MaterialBrowserWindow::drawToolbar() {
    ImGui::PushID("MaterialBrowserToolbar");

    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    if (ImGui::InputTextWithHint("##filter", "Search materials", filterInput, IM_ARRAYSIZE(filterInput))) {
        filter = filterInput;
        applyFilter();
    }

    ImGui::SameLine();
    if (ImGui::Button("Folder..."))
        chooseFolder();

    ImGui::SameLine();
    if (ImGui::Button("Rescan"))
        rescanPending = true;

    ImGui::SameLine();
    if (!externalDir.empty() && ImGui::Button("Clear folder")) {
        externalDir.clear();
        rescanPending = true;
    }

    const int shown = static_cast<int>(visible.size());
    const int total = static_cast<int>(entries.size());
    const int ready = static_cast<int>(std::count_if(entries.begin(), entries.end(),
        [](const Entry& entry) { return entry.thumbReady; }));
    ImGui::SameLine();
    if (shown == total)
        ImGui::TextDisabled("%d materials, %d previewed", total, ready);
    else
        ImGui::TextDisabled("%d of %d materials match, %d previewed", shown, total, ready);

    if (!notice.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f), "%s", notice.c_str());
    }

    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Tiling");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    float tiles = tiling;
    if (ImGui::SliderFloat("##tiling", &tiles, 1.0f, 8.0f, "%.1f")) {
        tiling = tiles;
        invalidateThumbs();
        needsHeroRender = true;
    }

    ImGui::SameLine();
    ImGui::Text("Size");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    float cell = cellSize;
    if (ImGui::SliderFloat("##cell", &cell, Theme::dpi(88.0f), Theme::dpi(320.0f), "%.0f"))
        cellSize = cell;

    ImGui::SameLine();
    ImGui::Text("Light");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    float light = lightIntensity;
    if (ImGui::SliderFloat("##light", &light, 0.0f, 8.0f, "%.1f")) {
        lightIntensity = light;
        needsHeroRender = true;
        invalidateThumbs();
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("IBL", &receiveIBL)) {
        gridRender.setReceiveIBL(receiveIBL);
        heroRender.setReceiveIBL(receiveIBL);
        needsHeroRender = true;
        invalidateThumbs();
    }

    ImGui::PopID();
}

void editor::MaterialBrowserWindow::drawHero(Entry& entry) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float canvas = std::clamp(std::min(avail.x * 0.42f, avail.y * 0.55f), Theme::dpi(160.0f), Theme::dpi(420.0f));

    ImGui::BeginGroup();
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##heroCanvas", ImVec2(canvas, canvas));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    if (hovered && io.MouseWheel != 0.0f) {
        distance = std::clamp(distance * std::pow(1.15f, io.MouseWheel), 1.2f, 8.0f);
        needsHeroRender = true;
    }
    if (active && hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        yaw -= io.MouseDelta.x * 0.01f;
        pitch = std::clamp(pitch + io.MouseDelta.y * 0.01f, -1.3f, 1.3f);
        needsHeroRender = true;
    }
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    const int wanted = std::max(1, (int)std::lround(canvas * Theme::dpiScale()));
    if (wanted != heroWidth) {
        heroWidth = wanted;
        heroHeight = wanted;
        heroRender.setFrameSize(wanted, wanted);
        needsHeroRender = true;
    }
    if (needsHeroRender) {
        heroRender.setTiling(tiling, tiling);
        heroRender.setLightIntensity(lightIntensity);
        heroRender.setCameraPose(yaw, pitch, distance);
        heroRender.applyMaterial(MaterialSets::buildMaterial(entry.set));
        Engine::executeSceneOnce(heroRender.getScene());
        // The pose staged here reaches the framebuffer on the next systemDraw(), so the
        // frame that shows it has to be scheduled even if the editor was about to idle.
        Backend::getApp().requestRedraw();
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMax(canvasMin.x + canvas, canvasMin.y + canvas);
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(11, 12, 14, 255));

    Texture preview = heroRender.getTexture();
    TextureRender* render = preview.getRender();
    // The maps land over several frames, so the pose stays dirty until the sphere on
    // screen is actually shaded with them - clearing it on the first drawn frame would
    // freeze the preview on its white one.
    const bool ready = render && render->isCreated() && !mapsPending(heroRender.getMaterial());
    if (ready)
        needsHeroRender = false;
    if (render && render->isCreated()) {
        drawList->AddImage(Backend::getImGuiTexture(render), canvasMin, canvasMax);
    } else {
        drawList->AddText(ImVec2(canvasMin.x + Theme::dpi(10.0f), canvasMin.y + Theme::dpi(10.0f)),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), "Rendering...");
    }
    drawList->AddRect(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + std::max(0.0f, avail.x - canvas - Theme::dpi(16.0f)));
    ImGui::TextUnformatted(entry.set.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("%s", entry.set.directory.string().c_str());
    ImGui::Spacing();

    if (ImGui::SmallButton("Reset view")) {
        yaw = 0.6f;
        pitch = 0.25f;
        distance = 2.6f;
        needsHeroRender = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear cache")) {
        thumbs.erase(entry.thumbPath.generic_u8string());
        std::error_code ec;
        fs::remove(entry.thumbPath, ec);
        entry.thumbReady = false;
        entry.thumbBusy = false;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Maps");
    if (ImGui::BeginTable("##heroMaps", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
        ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(140.0f));
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(64.0f));
        ImGui::TableSetupColumn("file", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const fs::path& file : entry.set.files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.set.kindLabel(file).c_str());
            ImGui::TableNextColumn();
            int width = 0, height = 0, channels = 0;
            std::error_code ec;
            if (fs::exists(file, ec)) {
                const std::string utf8 = FileUtils::pathToUtf8(file);
                stbi_info(utf8.c_str(), &width, &height, &channels);
            }
            ImGui::TextDisabled("%dx%d", width, height);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(file.filename().string().c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
}

void editor::MaterialBrowserWindow::drawGrid() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float label = Theme::dpi(18.0f);
    const float gap = Theme::dpi(6.0f);
    const float cell = cellSize;
    const float rowHeight = cell + label + gap;
    const int columns = std::max(1, (int)((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));

    int column = 0;
    for (size_t index : visible) {
        Entry& entry = entries[index];
        const bool isSelected = selected == static_cast<int>(index);

        if (column > 0)
            ImGui::SameLine(0.0f, gap);

        const ImVec2 cellMin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cell, rowHeight));

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selected = static_cast<int>(index);
            needsHeroRender = true;
        }

        drawList->AddRectFilled(cellMin, ImVec2(cellMin.x + cell, cellMin.y + cell),
                                ImGui::GetColorU32(ImGuiCol_Button), Theme::dpi(4.0f));

        if (entry.thumbReady) {
            if (ImTextureID texture = thumbTexture(entry.thumbPath)) {
                const float inset = Theme::dpi(3.0f);
                drawList->AddImage(texture,
                                   ImVec2(cellMin.x + inset, cellMin.y + inset),
                                   ImVec2(cellMin.x + cell - inset, cellMin.y + cell - inset));
            } else if (entry.thumbBusy) {
                drawList->AddText(ImVec2(cellMin.x + Theme::dpi(8.0f), cellMin.y + cell * 0.5f),
                                  ImGui::GetColorU32(ImGuiCol_TextDisabled), "...");
            }
        } else if (entry.thumbBusy) {
            drawList->AddText(ImVec2(cellMin.x + Theme::dpi(8.0f), cellMin.y + cell * 0.5f),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), "...");
        }

        const std::string label_ = ellipsize(entry.set.name, cell);
        const ImVec2 textSize = ImGui::CalcTextSize(label_.c_str());
        drawList->AddText(ImVec2(cellMin.x + (cell - textSize.x) * 0.5f, cellMin.y + cell + Theme::dpi(2.0f)),
                          ImGui::GetColorU32(isSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled), label_.c_str());

        if (isSelected) {
            drawList->AddRect(cellMin, ImVec2(cellMin.x + cell, cellMin.y + cell),
                              ImGui::GetColorU32(ImGuiCol_CheckMark), Theme::dpi(4.0f), 0, Theme::dpi(2.0f));
        }

        column = (column + 1) % columns;
    }

    if (visible.empty()) {
        ImGui::Spacing();
        if (entries.empty()) {
            ImGui::TextDisabled("No material sets found. Add textures to the project, or pick a folder "
                                "of maps (diffuse / normal / rough / ao, named the way the free PBR "
                                "libraries name them).");
        } else {
            ImGui::TextDisabled("No material matches \"%s\".", filter.c_str());
        }
    }
}

void editor::MaterialBrowserWindow::show() {
    const bool wasOpen = windowOpen;
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen)
            setOpen(false);
        return;
    }

    if (rescanPending)
        rescan();

    drainFinishedSaves();
    drawToolbar();
    ImGui::Separator();

    if (selected >= 0 && selected < static_cast<int>(entries.size())) {
        drawHero(entries[static_cast<size_t>(selected)]);
        ImGui::Separator();
    }

    drawGrid();
    stageNextThumb();

    ImGui::End();
}
