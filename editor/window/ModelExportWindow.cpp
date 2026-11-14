// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "window/ModelExportWindow.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "util/FbxImporter.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "model/MeshExport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>

using namespace quantum;
using namespace quantum::editor;

namespace {

    namespace fs = std::filesystem;
    using namespace quantum::model;

    constexpr size_t MAX_SCANNED = 20000;

    // Generated and somebody-else's-export folders, the same list the Material Library
    // skips: walking a build tree for meshes costs seconds and only finds copies.
    const char* const kSkipDirs[] = { ".git", ".quantum", "node_modules", "build", "export",
                                      "exports", "Intermediate", "Binaries" };

    std::string lowered(const std::string& text) {
        std::string out = text;
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::string number(int64_t value) {
        return std::to_string(value);
    }

    std::string decimal(float value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", value);
        return buffer;
    }

    std::string plural(size_t count, const char* one, const char* many) {
        return number((int64_t)count) + " " + (count == 1 ? one : many);
    }

    // The stem the writers are given is also the name of the file on disk, so the two can
    // never disagree about which object the file holds.
    std::string sanitized(const std::string& text, const std::string& fallback) {
        std::string out;
        for (const char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
                out.push_back(c);
            else if (!out.empty() && out.back() != '_')
                out.push_back('_');
        }
        while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
        return out.empty() ? fallback : out;
    }

    void setField(char* field, size_t size, const std::string& text) {
        std::snprintf(field, size, "%s", text.c_str());
    }

}

editor::ModelExportWindow::ModelExportWindow(Project* project) : project(project) {
    // Not here: the window is built before a project is open, and the assets of the project
    // that arrives later are the only list worth showing.
    rescanPending = true;
}

void editor::ModelExportWindow::show() {
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

    if (rescanPending || (project && project->getAssetsPath() != scannedRoot)) rescan();

    drawSources();
    ImGui::SameLine();
    ImGui::BeginGroup();
    drawDetails();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawOptions();
    ImGui::EndGroup();

    ImGui::End();
}

void editor::ModelExportWindow::setOpen(bool open) {
    if (open == windowOpen) return;
    windowOpen = open;
    if (open) {
        focusRequested = true;
        rescanPending = true;
    }
}

bool editor::ModelExportWindow::isOpen() const {
    return windowOpen;
}

void editor::ModelExportWindow::focus() {
    setOpen(true);
}

void editor::ModelExportWindow::openFile(const fs::path& path) {
    setOpen(true);
    select(path);
}

void editor::ModelExportWindow::rescan() {
    rescanPending = false;

    std::vector<fs::path> files;
    std::error_code ec;
    if (project) {
        // getAssetsPath(), not getAssetsDir(): the yaml value is often "." and would walk
        // whatever folder the editor process started in, which is the engine copy.
        const fs::path root = project->getAssetsPath();
        scannedRoot = root;
        if (!root.empty() && fs::is_directory(root, ec)) {
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator() && files.size() < MAX_SCANNED; ++it) {
                ec.clear();
                const fs::path& path = it->path();
                if (it->is_directory(ec)) {
                    const std::string name = lowered(path.filename().string());
                    for (const char* skip : kSkipDirs) {
                        if (name == skip) {
                            it.disable_recursion_pending();
                            break;
                        }
                    }
                    continue;
                }
                if (it->is_regular_file(ec)) {
                    if (Util::isModelFile(path.extension().string())) files.push_back(path);
                }
            }
        }
    }

    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return lowered(a.string()) < lowered(b.string());
    });

    const fs::path previous = selected >= 0 && selected < (int)sources.size()
        ? sources[(size_t)selected].path : fs::path();

    sources.clear();
    sources.reserve(files.size());
    for (const fs::path& file : files) {
        Source entry;
        entry.path = file;
        entry.readFile = file;
        if (FbxImporter::isFbxFile(file) && project) {
            const fs::path imported = project->getImportedModelPath(file);
            if (!imported.empty() && fs::exists(imported, ec)) {
                entry.readFile = imported;
                entry.fromImport = true;
            }
        }
        sources.push_back(std::move(entry));
    }

    selected = -1;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (!previous.empty() && sources[i].path == previous) {
            selected = (int)i;
            break;
        }
    }
    applyFilter();
}

void editor::ModelExportWindow::applyFilter() {
    visible.clear();
    const std::string needle = lowered(filterInput);
    for (size_t i = 0; i < sources.size(); ++i) {
        if (needle.empty() || lowered(sources[i].path.string()).find(needle) != std::string::npos)
            visible.push_back((int)i);
    }
    rebuildVisibility();
}

// A filter that hides the selected row would leave the details pane describing a file nobody
// can click, so the selection follows the list out.
void editor::ModelExportWindow::rebuildVisibility() {
    if (selected < 0) return;
    if (std::find(visible.begin(), visible.end(), selected) == visible.end()) selected = -1;
}

void editor::ModelExportWindow::select(const fs::path& path) {
    std::error_code ec;
    const fs::path wanted = fs::weakly_canonical(path, ec);

    int index = -1;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (fs::weakly_canonical(sources[i].path, ec) == wanted) { index = (int)i; break; }
    }

    if (index < 0) {
        Source entry;
        entry.path = wanted;
        entry.readFile = wanted;
        if (FbxImporter::isFbxFile(wanted) && project) {
            const fs::path imported = project->getImportedModelPath(wanted);
            if (!imported.empty() && fs::exists(imported, ec)) {
                entry.readFile = imported;
                entry.fromImport = true;
            }
        }
        sources.push_back(entry);
        index = (int)sources.size() - 1;
        applyFilter();
    }

    selected = index;
    scrollToSelection = true;
    load(sources[(size_t)index]);
}

bool editor::ModelExportWindow::load(const Source& source) {
    loaded.reset();
    loadError.clear();
    loadWarnings.clear();
    result.clear();

    ImportedModel model;
    std::string error;
    // The kernel's file APIs open with the narrow path the C runtime expects; the utf-8 form
    // is only for the log and the panel, which want text ImGui can render.
    if (!importModelFile(source.readFile.string(), model, error)) {
        loadError = error.empty() ? "the importer refused this file" : error;
        Out::error("Model export: " + FileUtils::pathToUtf8(source.path) + " - " + loadError);
        return false;
    }

    stats = meshStats(model.topology, model.attribs);
    loadWarnings = model.warnings;
    loaded = std::make_unique<ImportedModel>(std::move(model));

    // Every read file re-claims the name: this panel converts one mesh at a time, and a
    // name left over from the previous one would write the wrong file.
    setField(nameInput, sizeof(nameInput),
             sanitized(loaded->name.empty() ? source.path.stem().string() : loaded->name, "mesh"));
    if (!source.path.parent_path().empty())
        setField(outputDirInput, sizeof(outputDirInput),
                 FileUtils::pathToUtf8(source.path.parent_path()));

    Out::info("Model export: read " + plural(stats.vertices, "vertex", "vertices") + ", " +
              plural(stats.faces, "face", "faces") + " from " +
              source.path.filename().string());
    return true;
}

void editor::ModelExportWindow::browseSource() {
    const std::string start = project ? FileUtils::pathToUtf8(project->getAssetsPath()) : std::string();
    const std::string picked = FileDialogs::openFileDialog(start, FILE_DIALOG_MODEL);
    if (!picked.empty()) select(FileUtils::pathFromUtf8(picked));
}

void editor::ModelExportWindow::browseOutput() {
    const std::string picked = FileDialogs::openFileDialog(outputDirInput, FILE_DIALOG_ALL, true);
    if (!picked.empty()) setField(outputDirInput, sizeof(outputDirInput), picked);
}

fs::path editor::ModelExportWindow::outputDirectory() const {
    fs::path dir = FileUtils::pathFromUtf8(outputDirInput);
    if (dir.empty() && selected >= 0) dir = sources[(size_t)selected].path.parent_path();
    return dir;
}

fs::path editor::ModelExportWindow::outputPath() const {
    const std::string stem = sanitized(nameInput, selected >= 0
        ? sources[(size_t)selected].path.stem().string() : "mesh");
    return outputDirectory() / (stem + (format == Format::Glb ? ".glb" : ".obj"));
}

std::string editor::ModelExportWindow::formatName() const {
    return format == Format::Glb ? "glTF binary" : "Wavefront";
}

void editor::ModelExportWindow::runExport() {
    result.clear();
    if (!loaded) {
        result = "Nothing to export: no file has been read.";
        resultOk = false;
        return;
    }

    ExportOptions options;
    options.name = sanitized(nameInput, loaded->name.empty() ? "mesh" : loaded->name);
    options.writeNormals = writeNormals;
    options.writeUVs = writeUVs;

    const fs::path target = outputPath();
    const std::string file = target.string();
    const std::string shown = FileUtils::pathToUtf8(target);
    const std::string shownName = FileUtils::pathToUtf8(target.filename());
    std::string error;
    const bool written = format == Format::Glb
        ? writeGlb(file, loaded->topology, loaded->attribs, options, error)
        : writeObj(file, loaded->topology, loaded->attribs, options, error);

    if (!written) {
        result = "The " + formatName() + " writer refused the mesh: " +
                 (error.empty() ? "no reason given" : error);
        resultOk = false;
        Out::error("Model export: " + shown + " - " + result);
        return;
    }

    if (!verify) {
        result = "Wrote " + shownName + " (" +
                 plural(stats.vertices, "vertex", "vertices") + ", " +
                 plural(stats.triangles, "triangle", "triangles") + "), unverified.";
        resultOk = true;
        Out::success("Model export: wrote " + shown);
        return;
    }

    // The writer is only proven by its reader, so the file just written is read back through
    // the same kernel and the two sets of numbers are compared.
    ImportedModel back;
    std::string readError;
    if (!importModelFile(file, back, readError)) {
        result = "Wrote " + shownName + ", but reading it back failed: " +
                 (readError.empty() ? "no reason given" : readError);
        resultOk = false;
        Out::error("Model export: " + shown + " - " + result);
        return;
    }

    MeshStats expected = stats;
    if (!writeUVs) expected.textureCoordinates = 0;

    std::vector<std::string> differences;
    if (compareMeshStats(expected, meshStats(back.topology, back.attribs), differences)) {
        result = "Wrote and re-read " + shownName + ": " +
                 plural(expected.vertices, "vertex", "vertices") + ", " +
                 plural(expected.triangles, "triangle", "triangles") + " preserved.";
        resultOk = true;
        Out::success("Model export: round trip of " + shown + " matches");
    } else {
        result = "Wrote " + shownName + ", and reading it back differs:";
        for (const std::string& difference : differences) result += "\n  - " + difference;
        resultOk = false;
        Out::warning("Model export: round trip of " + shown + " differs in " +
                     number((int64_t)differences.size()) + " counts");
    }
}

void editor::ModelExportWindow::drawSources() {
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    if (ImGui::InputTextWithHint("##filter", "filter", filterInput, sizeof(filterInput)))
        applyFilter();
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) rescanPending = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) browseSource();

    ImGui::TextDisabled("%zu of %zu models", visible.size(), sources.size());

    ImGui::BeginChild("sources", ImVec2(Theme::dpi(240.0f), 0), true);
    for (int index : visible) {
        const Source& source = sources[(size_t)index];
        std::string label = source.path.filename().string();
        if (project) {
            std::error_code ec;
            const fs::path relative = fs::relative(source.path, project->getAssetsPath(), ec);
            if (!ec && !relative.empty() && relative.string().rfind("..", 0) != 0)
                label = FileUtils::pathToUtf8(relative);
        }
        if (source.fromImport) label += " (import)";
        // The index keeps two rows with the same file name apart, which ImGui would
        // otherwise fold into one widget ID and one selection.
        const std::string row = label + "##" + std::to_string(index);

        if (ImGui::Selectable(row.c_str(), index == selected)) {
            selected = index;
            load(source);
        }
        if (index == selected && scrollToSelection) {
            ImGui::SetScrollHereY(0.5f);
            scrollToSelection = false;
        }
    }
    if (sources.empty()) {
        ImGui::PushTextWrapPos(Theme::dpi(220.0f));
        ImGui::TextDisabled("No model in the project's assets folder. Browse for a file "
                            "anywhere on disk.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

void editor::ModelExportWindow::drawStat(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

void editor::ModelExportWindow::drawDetails() {
    if (!loaded) {
        ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x +
                               std::max(Theme::dpi(240.0f), ImGui::GetContentRegionAvail().x));
        if (!loadError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", loadError.c_str());
        } else {
            ImGui::TextDisabled("Pick a model on the left. Its geometry is read by the same "
                                "kernel the modelling operations work on, so the numbers here "
                                "are the ones an edit would change.");
        }
        ImGui::PopTextWrapPos();
        return;
    }

    ImGui::TextUnformatted("Geometry");
    if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("channel", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(140.0f));
        ImGui::TableHeadersRow();

        drawStat("vertices", number((int64_t)stats.vertices));
        drawStat("faces", number((int64_t)stats.faces));
        drawStat("triangles", number((int64_t)stats.triangles));
        drawStat("edges", number((int64_t)stats.edges));
        drawStat("V - E + F", number(stats.eulerCharacteristic));
        drawStat("vertex normals", stats.hasVertexNormals ? "stored" : "derived");
        drawStat("texture coordinates", number((int64_t)stats.textureCoordinates));
        drawStat("material slots", number((int64_t)stats.materialSlots));
        drawStat("size", decimal(stats.maximum.x - stats.minimum.x) + " x " +
                        decimal(stats.maximum.y - stats.minimum.y) + " x " +
                        decimal(stats.maximum.z - stats.minimum.z));

        ImGui::EndTable();
    }

    const std::vector<std::string> problems = meshWarnings(loaded->topology, stats);
    const size_t lines = loadWarnings.size() + problems.size();
    if (lines) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f), "%s",
                           plural(lines, "thing to know", "things to know").c_str());
        for (const std::string& warning : loadWarnings) ImGui::BulletText("%s", warning.c_str());
        for (const std::string& problem : problems) ImGui::BulletText("%s", problem.c_str());
    }
}

void editor::ModelExportWindow::drawOptions() {
    ImGui::TextUnformatted("Export");

    int asInt = format == Format::Glb ? 0 : 1;
    if (ImGui::RadioButton("glTF binary (.glb)", &asInt, 0)) format = Format::Glb;
    ImGui::SameLine();
    if (ImGui::RadioButton("Wavefront (.obj + .mtl)", &asInt, 1)) format = Format::Obj;

    ImGui::TextDisabled("%s", format == Format::Glb
        ? "glTF has no polygons: every face is split into triangles."
        : "Wavefront keeps n-gons, so a quad authored here stays a quad.");

    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    ImGui::InputText("name", nameInput, sizeof(nameInput));

    ImGui::Checkbox("write normals", &writeNormals);
    ImGui::SameLine();
    ImGui::Checkbox("write texture coordinates", &writeUVs);

    ImGui::SetNextItemWidth(Theme::dpi(300.0f));
    ImGui::InputText("folder", outputDirInput, sizeof(outputDirInput));
    ImGui::SameLine();
    if (ImGui::Button("...")) browseOutput();

    ImGui::TextDisabled("to %s", FileUtils::pathToUtf8(outputPath()).c_str());

    ImGui::Checkbox("read the file back and compare", &verify);
    if (!verify) {
        ImGui::SameLine();
        ImGui::TextDisabled("(unproven)");
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!loaded);
    if (ImGui::Button("Export", ImVec2(Theme::dpi(120.0f), 0.0f))) runExport();
    ImGui::EndDisabled();

    if (result.empty()) return;
    if (std::count(result.begin(), result.end(), '\n')) {
        ImGui::NewLine();
    } else {
        ImGui::SameLine();
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x +
                           std::max(Theme::dpi(280.0f), ImGui::GetContentRegionAvail().x));
    ImGui::TextColored(resultOk ? ImVec4(0.5f, 0.85f, 0.5f, 1.0f)
                                : ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", result.c_str());
    ImGui::PopTextWrapPos();
}
