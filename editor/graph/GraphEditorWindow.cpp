// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "GraphEditorWindow.h"

#include "Backend.h"
#include "Theme.h"
#include "Log.h"
#include "external/IconsFontAwesome6.h"
#include "graph/GraphCompiler.h"
#include "graph/GraphScriptProperties.h"
#include "graph/GraphSerializerYaml.h"
#include "graph/NodeCatalog.h"
#include "Project.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

namespace quantum::editor {

    namespace {

        constexpr float kHeaderHeight = 26.0f;
        constexpr float kRowHeight = 20.0f;
        constexpr float kBottomPadding = 8.0f;
        constexpr float kPinRadius = 4.0f;
        constexpr size_t kMaxUndo = 64;

        GraphEditorWindow* g_graphEditorWindow = nullptr;

        float distanceSquared(const ImVec2& a, const ImVec2& b) {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            return dx * dx + dy * dy;
        }

        float distanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float lengthSquared = dx * dx + dy * dy;
            if (lengthSquared <= 0.0001f) return std::sqrt(distanceSquared(p, a));
            float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared;
            t = std::clamp(t, 0.0f, 1.0f);
            const ImVec2 closest(a.x + dx * t, a.y + dy * t);
            return std::sqrt(distanceSquared(p, closest));
        }

        ImVec2 bezierPoint(const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, float t) {
            const float u = 1.0f - t;
            const float uu = u * u;
            const float uuu = uu * u;
            const float tt = t * t;
            const float ttt = tt * t;
            return ImVec2(uuu * p0.x + 3.0f * uu * t * p1.x + 3.0f * u * tt * p2.x + ttt * p3.x,
                          uuu * p0.y + 3.0f * uu * t * p1.y + 3.0f * u * tt * p2.y + ttt * p3.y);
        }

        ImU32 typeColor(graph::GraphValueType type, int alpha = 255) {
            float hue = 0.0f;
            switch (type) {
                case graph::GraphValueType::Bool: hue = 0.00f; break;
                case graph::GraphValueType::Int: hue = 0.56f; break;
                case graph::GraphValueType::Float: hue = 0.33f; break;
                case graph::GraphValueType::String: hue = 0.83f; break;
                case graph::GraphValueType::Vector2:
                case graph::GraphValueType::Vector3:
                case graph::GraphValueType::Vector4:
                case graph::GraphValueType::Quaternion: hue = 0.13f; break;
                case graph::GraphValueType::Color3:
                case graph::GraphValueType::Color4: hue = 0.05f; break;
                case graph::GraphValueType::Rect: hue = 0.47f; break;
                case graph::GraphValueType::Entity: hue = 0.75f; break;
                default: hue = 0.0f; break;
            }
            float r = 0.8f;
            float g = 0.8f;
            float b = 0.8f;
            ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.92f, r, g, b);
            return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f),
                            static_cast<int>(b * 255.0f), alpha);
        }

        ImU32 categoryColor(const std::string& category, float alpha = 1.0f) {
            if (category == "Event") {
                const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
                return ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, accent.w * alpha));
            }
            std::size_t hash = 2166136261u;
            for (char c : category) {
                hash ^= static_cast<unsigned char>(c);
                hash *= 16777619u;
            }
            const float hue = static_cast<float>(hash % 360u) / 360.0f;
            float r = 0.5f;
            float g = 0.5f;
            float b = 0.5f;
            ImGui::ColorConvertHSVtoRGB(hue, 0.38f, 0.62f, r, g, b);
            return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f),
                            static_cast<int>(b * 255.0f), static_cast<int>(255.0f * alpha));
        }

        ImU32 execColor(int alpha = 255) {
            const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            return ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, text.w * (alpha / 255.0f)));
        }

        std::string toLower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool matchesQuery(const std::string& haystack, const std::string& query) {
            if (query.empty()) return true;
            const std::string lowered = toLower(haystack);
            std::size_t start = 0;
            while (start < query.size()) {
                std::size_t end = query.find(' ', start);
                if (end == std::string::npos) end = query.size();
                if (end > start) {
                    const std::string token = query.substr(start, end - start);
                    if (lowered.find(token) == std::string::npos) return false;
                }
                start = end + 1;
            }
            return true;
        }

        std::string nodeKindEventId(const std::string& kind) {
            if (kind.compare(0, 6, graph::GRAPH_EVENT_PREFIX) != 0) return std::string();
            return kind.substr(6);
        }

        bool isPathExtension(const std::filesystem::path& path, const char* extension) {
            return toLower(path.extension().string()) == extension;
        }

    }

    void setGraphEditorWindow(GraphEditorWindow* window) {
        g_graphEditorWindow = window;
    }

    GraphEditorWindow* getGraphEditorWindow() {
        return g_graphEditorWindow;
    }

    void GraphEditorWindow::setOpen(bool open) {
        windowOpen = open;
    }

    void GraphEditorWindow::openFile(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::path authoring = path;
        if (isPathExtension(authoring, ".gdat")) {
            authoring.replace_extension(".graph");
        }

        if (dirty && !filePath.empty() && authoring != filePath) {
            pendingOpenPath = authoring;
            windowOpen = true;
            focusRequested = true;
            return;
        }

        pendingOpenPath.clear();

        if (!std::filesystem::exists(authoring, ec)) {
            const std::string message = "Graph source not found: " + authoring.string();
            Log::error("Graph Editor: %s", message.c_str());
            Backend::getApp().registerAlert("Error", message);
            setStatus(message, true);
            return;
        }

        graph::GraphAsset loaded;
        std::string error;
        if (!GraphSerializerYaml::loadFile(authoring.string(), loaded, error)) {
            const std::string message = "Failed to open graph: " + error;
            Log::error("Graph Editor: %s", message.c_str());
            Backend::getApp().registerAlert("Error", message);
            setStatus(message, true);
            return;
        }

        filePath = authoring;
        pendingOpenPath.clear();
        asset = std::move(loaded);
        if (asset.name.empty()) {
            asset.name = authoring.stem().string();
        }

        undoStack.clear();
        redoStack.clear();
        diagnostics.clear();
        diagnosticsStale = true;
        dirty = false;
        selectedNode = 0;
        selectedComment = -1;
        selectedEdge = -1;
        draggingNode = 0;
        draggingComment = -1;
        linking = false;
        zoom = 1.0f;
        scroll = ImVec2(40.0f, 40.0f);
        updateEventBindings();
        setStatus("Opened " + authoring.filename().string());
        windowOpen = true;
        focusRequested = true;
    }

    bool GraphEditorWindow::save() {
        if (filePath.empty()) {
            setStatus("Graph has no file path", true);
            return false;
        }

        std::string error;
        if (!GraphSerializerYaml::saveFile(filePath.string(), asset, error)) {
            const std::string message = "Save failed: " + error;
            Log::error("Graph Editor: %s", message.c_str());
            setStatus(message, true);
            return false;
        }

        dirty = false;
        setStatus("Saved " + filePath.filename().string());
        return true;
    }

    bool GraphEditorWindow::compile() {
        if (!save()) return false;

        refreshDiagnostics();
        if (graph::GraphSchema::hasErrors(diagnostics)) {
            setStatus("Compile blocked: graph has validation errors", true);
            return false;
        }

        std::string error;
        std::ifstream input(filePath, std::ios::binary);
        if (!input) {
            setStatus("Compile failed: cannot read " + filePath.filename().string(), true);
            return false;
        }
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const uint64_t hash = graph::graphSourceHash(source.data(), source.size());

        std::filesystem::path outputPath = filePath;
        outputPath.replace_extension(".gdat");
        if (!graph::GraphCompiler::writeFile(outputPath.string(), asset, hash, error)) {
            const std::string message = "Compile failed: " + error;
            Log::error("Graph Editor: %s", message.c_str());
            setStatus(message, true);
            return false;
        }

        if (Project* project = Backend::getApp().getProject()) {
            GraphScriptProperties::refreshAll(*project, outputPath);
        }

        setStatus("Compiled " + outputPath.filename().string());
        return true;
    }

    void GraphEditorWindow::show() {
        if (!windowOpen) return;

        if (focusRequested) {
            ImGui::SetNextWindowFocus();
            focusRequested = false;
        }

        ImGui::SetNextWindowSize(ImVec2(1180.0f, 720.0f), ImGuiCond_FirstUseEver);
        std::string windowLabel = filePath.empty() ? std::string(WINDOW_NAME) : filePath.filename().string();
        if (dirty) windowLabel += "*";
        windowLabel += "###";
        windowLabel += WINDOW_NAME;
        if (!ImGui::Begin(windowLabel.c_str(), &windowOpen)) {
            ImGui::End();
            return;
        }

        drawToolbar();
        ImGui::Separator();

        if (!pendingOpenPath.empty()) {
            ImGui::OpenPopup("Unsaved Graph");
        }
        if (ImGui::BeginPopupModal("Unsaved Graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Save changes to %s before opening %s?",
                        filePath.filename().string().c_str(),
                        pendingOpenPath.filename().string().c_str());
            ImGui::Spacing();
            if (ImGui::Button("Save")) {
                if (save()) {
                    const std::filesystem::path next = pendingOpenPath;
                    pendingOpenPath.clear();
                    ImGui::CloseCurrentPopup();
                    openFile(next);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard")) {
                const std::filesystem::path next = pendingOpenPath;
                pendingOpenPath.clear();
                dirty = false;
                ImGui::CloseCurrentPopup();
                openFile(next);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                pendingOpenPath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float diagnosticsHeight = showDiagnostics ? 132.0f : 0.0f;
        const float bodyHeight = std::max(120.0f, available.y - (showDiagnostics ? diagnosticsHeight + style.ItemSpacing.y : 0.0f));
        const float variablesWidth = showVariables ? std::clamp(available.x * 0.18f, 180.0f, 320.0f) : 0.0f;
        const float detailsWidth = showDetails ? std::clamp(available.x * 0.24f, 250.0f, 420.0f) : 0.0f;
        const float canvasWidth = std::max(80.0f, available.x
            - (showVariables ? variablesWidth + style.ItemSpacing.x : 0.0f)
            - (showDetails ? detailsWidth + style.ItemSpacing.x : 0.0f));

        if (showVariables) {
            ImGui::BeginChild("##graph_variables", ImVec2(variablesWidth, bodyHeight), ImGuiChildFlags_Borders);
            drawVariablesPanel();
            ImGui::EndChild();
            ImGui::SameLine();
        }

        ImGui::BeginChild("##graph_canvas", ImVec2(canvasWidth, bodyHeight), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawCanvas(ImVec2(canvasWidth, bodyHeight));
        ImGui::EndChild();

        if (showDetails) {
            ImGui::SameLine();
            ImGui::BeginChild("##graph_details", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_Borders);
            drawDetailsPanel();
            ImGui::EndChild();
        }

        if (showDiagnostics) {
            ImGui::BeginChild("##graph_diagnostics", ImVec2(0.0f, diagnosticsHeight), ImGuiChildFlags_Borders);
            drawDiagnosticsPanel();
            ImGui::EndChild();
        }

        ImGui::End();
    }

    void GraphEditorWindow::drawToolbar() {
        const std::string title = filePath.empty() ? std::string("(no graph)") : filePath.filename().string();
        ImGui::TextUnformatted(title.c_str());
        if (!filePath.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", filePath.string().c_str());
        }
        if (dirty) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(Theme::Colors::WarningText, "*");
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(filePath.empty());
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save")) {
            save();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the .graph source (Ctrl+S)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_GEARS " Compile")) {
            compile();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save and write the runtime .gdat next to the .graph");
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Validate")) {
            diagnosticsStale = true;
            refreshDiagnostics();
            setStatusf(false, "%zu issue(s)", diagnostics.size());
        }

        ImGui::SameLine();
        if (ImGui::Button("Frame All")) {
            frameAllRequested = true;
        }

        ImGui::SameLine();
        ImGui::Checkbox("Variables", &showVariables);
        ImGui::SameLine();
        ImGui::Checkbox("Details", &showDetails);
        ImGui::SameLine();
        ImGui::Checkbox("Diagnostics", &showDiagnostics);

        if (statusMessage[0] != '\0' && ImGui::GetTime() - statusTime < 8.0) {
            ImGui::SameLine();
            ImGui::TextColored(statusError ? Theme::Colors::DangerText : Theme::Colors::SuccessText,
                               "%s", statusMessage);
        }
    }

    void GraphEditorWindow::buildNodeViews() {
        nodeViews.clear();
        nodeViews.reserve(asset.nodes.size());

        const graph::NodeCatalog& catalog = graph::NodeCatalog::core();
        for (std::size_t index = 0; index < asset.nodes.size(); ++index) {
            const graph::GraphNode& node = asset.nodes[index];
            const graph::GraphNodeDef* def = catalog.find(node.kind);
            const std::string title = node.title.empty()
                ? (def ? def->title : node.kind)
                : node.title;
            const std::vector<graph::GraphPinDef> pins = catalog.resolvePins(node, &asset);

            float width = std::max(150.0f, ImGui::CalcTextSize(title.c_str()).x + 28.0f);
            std::size_t inputCount = 0;
            std::size_t outputCount = 0;
            for (const graph::GraphPinDef& pin : pins) {
                if (pin.dir == graph::GraphPinDir::Input) ++inputCount;
                else ++outputCount;
                if (pin.kind == graph::GraphPinKind::Data) {
                    width = std::max(width, ImGui::CalcTextSize(pin.name.c_str()).x + 60.0f);
                }
            }
            const std::size_t rows = std::max(inputCount, outputCount);
            const float height = kHeaderHeight + kRowHeight * static_cast<float>(rows) + kBottomPadding;

            NodeView view;
            view.index = index;
            view.id = node.id;
            view.min = worldToScreen(node.x, node.y);
            view.max = worldToScreen(node.x + width, node.y + height);
            view.headerMax = worldToScreen(node.x + width, node.y + kHeaderHeight);

            std::size_t inputRow = 0;
            std::size_t outputRow = 0;
            for (const graph::GraphPinDef& pin : pins) {
                PinView pinView;
                pinView.name = pin.name;
                pinView.output = pin.dir == graph::GraphPinDir::Output;
                pinView.exec = pin.kind == graph::GraphPinKind::Exec;
                pinView.type = pin.type;

                std::size_t row = 0;
                float x = node.x;
                if (pinView.output) {
                    row = outputRow++;
                    x = node.x + width;
                } else {
                    row = inputRow++;
                }
                const float y = node.y + kHeaderHeight + kRowHeight * static_cast<float>(row) + kRowHeight * 0.5f + 4.0f;
                pinView.pos = worldToScreen(x, y);

                if (pinView.output) view.outputs.push_back(std::move(pinView));
                else view.inputs.push_back(std::move(pinView));
            }

            nodeViews.push_back(std::move(view));
        }
    }

    ImVec2 GraphEditorWindow::worldToScreen(float x, float y) const {
        return ImVec2(canvasOrigin.x + (x - scroll.x) * zoom,
                      canvasOrigin.y + (y - scroll.y) * zoom);
    }

    ImVec2 GraphEditorWindow::screenToWorld(const ImVec2& point) const {
        return ImVec2(scroll.x + (point.x - canvasOrigin.x) / zoom,
                      scroll.y + (point.y - canvasOrigin.y) / zoom);
    }

    const GraphEditorWindow::NodeView* GraphEditorWindow::findNodeView(uint32_t id) const {
        for (const NodeView& view : nodeViews) {
            if (view.id == id) return &view;
        }
        return nullptr;
    }

    const GraphEditorWindow::PinView* GraphEditorWindow::findPinView(uint32_t nodeId, const std::string& pin,
                                                                    bool output) const {
        const NodeView* node = findNodeView(nodeId);
        if (!node) return nullptr;
        const std::vector<PinView>& pins = output ? node->outputs : node->inputs;
        for (const PinView& pinView : pins) {
            if (pinView.name == pin) return &pinView;
        }
        return nullptr;
    }

    const GraphEditorWindow::PinView* GraphEditorWindow::hitTestPin(const ImVec2& mouse, PinRef& out) const {
        const float radius = std::max(4.0f, kPinRadius * zoom) + 5.0f;
        const float radiusSquared = radius * radius;
        for (const NodeView& node : nodeViews) {
            for (const PinView& pin : node.inputs) {
                if (distanceSquared(mouse, pin.pos) <= radiusSquared) {
                    out.nodeId = node.id;
                    out.pin = pin.name;
                    out.output = false;
                    out.exec = pin.exec;
                    out.type = pin.type;
                    return &pin;
                }
            }
            for (const PinView& pin : node.outputs) {
                if (distanceSquared(mouse, pin.pos) <= radiusSquared) {
                    out.nodeId = node.id;
                    out.pin = pin.name;
                    out.output = true;
                    out.exec = pin.exec;
                    out.type = pin.type;
                    return &pin;
                }
            }
        }
        return nullptr;
    }

    int GraphEditorWindow::hitTestNode(const ImVec2& mouse) const {
        for (std::size_t i = nodeViews.size(); i > 0; --i) {
            const NodeView& node = nodeViews[i - 1];
            if (mouse.x >= node.min.x && mouse.x <= node.max.x &&
                mouse.y >= node.min.y && mouse.y <= node.max.y) {
                return static_cast<int>(i - 1);
            }
        }
        return -1;
    }

    int GraphEditorWindow::hitTestComment(const ImVec2& mouse) const {
        for (std::size_t i = asset.comments.size(); i > 0; --i) {
            const graph::GraphComment& comment = asset.comments[i - 1];
            const ImVec2 min = worldToScreen(comment.x, comment.y);
            const ImVec2 max = worldToScreen(comment.x + comment.width, comment.y + comment.height);
            if (mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y) {
                return static_cast<int>(i - 1);
            }
        }
        return -1;
    }

    int GraphEditorWindow::hitTestEdge(const ImVec2& mouse) const {
        const float threshold = 6.0f + zoom;
        for (std::size_t i = 0; i < asset.edges.size(); ++i) {
            const graph::GraphEdge& edge = asset.edges[i];
            const PinView* from = findPinView(edge.fromNode, edge.fromPin, true);
            const PinView* to = findPinView(edge.toNode, edge.toPin, false);
            if (!from || !to) continue;

            const float tangent = std::max(40.0f, std::abs(to->pos.x - from->pos.x) * 0.5f);
            const ImVec2 c0(from->pos.x + tangent, from->pos.y);
            const ImVec2 c1(to->pos.x - tangent, to->pos.y);

            ImVec2 previous = from->pos;
            for (int step = 1; step <= 16; ++step) {
                const ImVec2 current = bezierPoint(from->pos, c0, c1, to->pos, static_cast<float>(step) / 16.0f);
                if (distanceToSegment(mouse, previous, current) <= threshold) {
                    return static_cast<int>(i);
                }
                previous = current;
            }
        }
        return -1;
    }

    void GraphEditorWindow::drawGrid(ImDrawList* drawList) const {
        const float spacing = 32.0f * zoom;
        if (spacing < 8.0f) return;

        const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_Text, 0.055f);
        const ImU32 majorColor = ImGui::GetColorU32(ImGuiCol_Text, 0.10f);
        const ImVec2 max(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y);

        float offsetX = std::fmod((-scroll.x * zoom), spacing);
        if (offsetX < 0.0f) offsetX += spacing;
        float offsetY = std::fmod((-scroll.y * zoom), spacing);
        if (offsetY < 0.0f) offsetY += spacing;

        int indexX = static_cast<int>(std::floor(scroll.x / 32.0f));
        for (float x = canvasOrigin.x + offsetX; x < max.x; x += spacing, ++indexX) {
            drawList->AddLine(ImVec2(x, canvasOrigin.y), ImVec2(x, max.y),
                              (indexX % 4 == 0) ? majorColor : lineColor);
        }

        int indexY = static_cast<int>(std::floor(scroll.y / 32.0f));
        for (float y = canvasOrigin.y + offsetY; y < max.y; y += spacing, ++indexY) {
            drawList->AddLine(ImVec2(canvasOrigin.x, y), ImVec2(max.x, y),
                              (indexY % 4 == 0) ? majorColor : lineColor);
        }
    }

    void GraphEditorWindow::drawEdges(ImDrawList* drawList) const {
        for (std::size_t i = 0; i < asset.edges.size(); ++i) {
            const graph::GraphEdge& edge = asset.edges[i];
            const PinView* from = findPinView(edge.fromNode, edge.fromPin, true);
            const PinView* to = findPinView(edge.toNode, edge.toPin, false);
            if (!from || !to) continue;

            const bool selected = selectedEdge == static_cast<int>(i);
            ImU32 color = from->exec ? execColor(180) : typeColor(from->type, 200);
            float thickness = std::max(1.5f, 2.0f * zoom);
            if (selected) {
                color = ImGui::GetColorU32(Theme::Colors::SubSelectionText);
                thickness = std::max(2.0f, 3.0f * zoom);
            }

            const float tangent = std::max(40.0f, std::abs(to->pos.x - from->pos.x) * 0.5f);
            drawList->AddBezierCubic(from->pos,
                                     ImVec2(from->pos.x + tangent, from->pos.y),
                                     ImVec2(to->pos.x - tangent, to->pos.y),
                                     to->pos, color, thickness);
        }
    }

    void GraphEditorWindow::drawNodes(ImDrawList* drawList) const {
        const graph::NodeCatalog& catalog = graph::NodeCatalog::core();
        const float fontSize = ImGui::GetFontSize() * zoom;
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 subtleColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImU32 bodyColor = ImGui::GetColorU32(ImGuiCol_ChildBg);
        const ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Border);

        for (const NodeView& view : nodeViews) {
            const graph::GraphNode& node = asset.nodes[view.index];
            const graph::GraphNodeDef* def = catalog.find(node.kind);
            const std::string title = node.title.empty()
                ? (def ? def->title : node.kind)
                : node.title;
            const std::string category = def ? def->category : std::string("Unknown");

            const bool selected = node.id == selectedNode;

            drawList->AddRectFilled(view.min, view.max, bodyColor, 4.0f * zoom);
            drawList->AddRectFilled(view.min, view.headerMax, categoryColor(category, 0.85f), 4.0f * zoom,
                                    ImDrawFlags_RoundCornersTop);
            drawList->AddRect(view.min, view.max, borderColor, 4.0f * zoom);

            drawList->AddText(ImGui::GetFont(), fontSize,
                              ImVec2(view.min.x + 10.0f * zoom, view.min.y + (kHeaderHeight * zoom - fontSize) * 0.5f),
                              textColor, title.c_str());

            for (const PinView& pin : view.inputs) {
                const ImU32 color = pin.exec ? execColor() : typeColor(pin.type);
                if (pin.exec) {
                    const float size = std::max(4.0f, kPinRadius * zoom);
                    drawList->AddTriangleFilled(ImVec2(pin.pos.x - size, pin.pos.y - size),
                                                ImVec2(pin.pos.x + size, pin.pos.y),
                                                ImVec2(pin.pos.x - size, pin.pos.y + size), color);
                } else {
                    drawList->AddCircleFilled(pin.pos, std::max(3.0f, kPinRadius * zoom), color);
                }
                drawList->AddText(ImGui::GetFont(), fontSize,
                                  ImVec2(pin.pos.x + 10.0f * zoom, pin.pos.y - fontSize * 0.5f),
                                  subtleColor, pin.name.c_str());
            }

            for (const PinView& pin : view.outputs) {
                const ImU32 color = pin.exec ? execColor() : typeColor(pin.type);
                if (pin.exec) {
                    const float size = std::max(4.0f, kPinRadius * zoom);
                    drawList->AddTriangleFilled(ImVec2(pin.pos.x + size, pin.pos.y - size),
                                                ImVec2(pin.pos.x + size, pin.pos.y + size),
                                                ImVec2(pin.pos.x - size, pin.pos.y), color);
                } else {
                    drawList->AddCircleFilled(pin.pos, std::max(3.0f, kPinRadius * zoom), color);
                }
                const float textWidth = ImGui::CalcTextSize(pin.name.c_str()).x * zoom;
                drawList->AddText(ImGui::GetFont(), fontSize,
                                  ImVec2(pin.pos.x - 10.0f * zoom - textWidth, pin.pos.y - fontSize * 0.5f),
                                  subtleColor, pin.name.c_str());
            }

            if (selected) {
                drawList->AddRect(ImVec2(view.min.x - 1.0f, view.min.y - 1.0f),
                                  ImVec2(view.max.x + 1.0f, view.max.y + 1.0f),
                                  ImGui::GetColorU32(Theme::Colors::SubSelectionText),
                                  5.0f * zoom, 0, std::max(2.0f, 2.0f * zoom));
            }
        }
    }

    void GraphEditorWindow::drawLinkPreview(ImDrawList* drawList) const {
        if (!linking) return;
        const PinView* from = findPinView(linkFrom.nodeId, linkFrom.pin, true);
        if (!from) return;
        const ImVec2 target = ImGui::GetIO().MousePos;
        const ImU32 color = linkFrom.exec ? execColor(200) : typeColor(linkFrom.type, 230);
        const float tangent = std::max(40.0f, std::abs(target.x - from->pos.x) * 0.5f);
        drawList->AddBezierCubic(from->pos,
                                 ImVec2(from->pos.x + tangent, from->pos.y),
                                 ImVec2(target.x - tangent, target.y),
                                 target, color, std::max(1.5f, 2.0f * zoom));
    }

    void GraphEditorWindow::drawCanvas(ImVec2 size) {
        (void)size;
        const ImVec2 available = ImGui::GetContentRegionAvail();
        canvasOrigin = ImGui::GetCursorScreenPos();
        canvasSize = available;

        ImGui::InvisibleButton("##graph_canvas_area", available);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool hovered = ImGui::IsItemHovered() && !popupOpen;

        if (hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                const ImVec2 before = screenToWorld(mouse);
                zoom = std::clamp(zoom * std::pow(1.15f, wheel), 0.2f, 2.5f);
                const ImVec2 after = screenToWorld(mouse);
                scroll.x += before.x - after.x;
                scroll.y += before.y - after.y;
            }
        }

        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            scroll.x -= delta.x / zoom;
            scroll.y -= delta.y / zoom;
        }

        buildNodeViews();
        handleCanvasInput(available);
        buildNodeViews();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(canvasOrigin, ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y), true);
        drawGrid(drawList);

        const ImU32 commentBorder = ImGui::GetColorU32(ImGuiCol_Border);
        for (std::size_t i = 0; i < asset.comments.size(); ++i) {
            const graph::GraphComment& comment = asset.comments[i];
            const ImVec2 min = worldToScreen(comment.x, comment.y);
            const ImVec2 max = worldToScreen(comment.x + comment.width, comment.y + comment.height);
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_PopupBg, 0.55f), 4.0f * zoom);
            drawList->AddRect(min, max, selectedComment == static_cast<int>(i)
                ? ImGui::GetColorU32(Theme::Colors::SubSelectionText)
                : commentBorder, 4.0f * zoom);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * zoom,
                              ImVec2(min.x + 8.0f * zoom, min.y + 6.0f * zoom),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), comment.text.c_str());
        }

        drawEdges(drawList);
        drawNodes(drawList);
        drawLinkPreview(drawList);
        drawList->PopClipRect();

        if (paletteRequested) {
            ImGui::OpenPopup("##graph_palette");
            paletteRequested = false;
            paletteFocus = true;
            paletteSearch[0] = '\0';
        }
        drawPalettePopup();

        if (ImGui::BeginPopup("##graph_node_menu")) {
            const graph::GraphNode* node = asset.findNode(selectedNode);
            if (node) {
                ImGui::TextDisabled("%s", node->kind.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate")) duplicateSelection();
                if (ImGui::MenuItem("Delete")) deleteSelection();
                ImGui::Separator();
                if (ImGui::MenuItem("Disconnect Links")) {
                    pushUndo();
                    asset.edges.erase(std::remove_if(asset.edges.begin(), asset.edges.end(),
                        [this](const graph::GraphEdge& edge) {
                            return edge.fromNode == selectedNode || edge.toNode == selectedNode;
                        }), asset.edges.end());
                    markDirty();
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("##graph_edge_menu")) {
            if (selectedEdge >= 0 && selectedEdge < static_cast<int>(asset.edges.size())) {
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete Link")) {
                    pushUndo();
                    asset.edges.erase(asset.edges.begin() + selectedEdge);
                    selectedEdge = -1;
                    markDirty();
                }
            }
            ImGui::EndPopup();
        }
    }

    void GraphEditorWindow::handleCanvasInput(ImVec2 size) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool hovered = ImGui::IsItemHovered() && !popupOpen;

        if (frameAllRequested) {
            frameAllRequested = false;
            frameAll();
        }

        if (linking && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            PinRef target;
            const PinView* pin = hitTestPin(mouse, target);
            if (pin && !target.output) {
                std::string error;
                if (!connectLink(linkFrom, target, error)) {
                    setStatus(error, true);
                }
            }
            linking = false;
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !linking) {
            PinRef pinRef;
            const PinView* pin = hitTestPin(mouse, pinRef);
            if (pin && pinRef.output) {
                linking = true;
                linkFrom = pinRef;
            } else if (pin) {
                setStatus("Drag from an output pin to an input pin", false);
            } else {
                const int nodeIndex = hitTestNode(mouse);
                if (nodeIndex >= 0) {
                    const NodeView& view = nodeViews[static_cast<std::size_t>(nodeIndex)];
                    const graph::GraphNode* node = asset.findNode(view.id);
                    if (node) {
                        selectNode(view.id);
                        draggingNode = view.id;
                        draggingComment = -1;
                        const ImVec2 world = screenToWorld(mouse);
                        dragOffset = ImVec2(world.x - node->x, world.y - node->y);
                        dragMoved = false;
                        dragUndoPending = true;
                        dragStartAsset = asset;
                    }
                } else {
                    const int commentIndex = hitTestComment(mouse);
                    if (commentIndex >= 0) {
                        selectedNode = 0;
                        selectedEdge = -1;
                        selectedComment = commentIndex;
                        draggingNode = 0;
                        draggingComment = commentIndex;
                        const ImVec2 world = screenToWorld(mouse);
                        dragOffset = ImVec2(world.x - asset.comments[commentIndex].x,
                                            world.y - asset.comments[commentIndex].y);
                        dragMoved = false;
                        dragUndoPending = true;
                        dragStartAsset = asset;
                    } else {
                        const int edgeIndex = hitTestEdge(mouse);
                        if (edgeIndex >= 0) {
                            selectedNode = 0;
                            selectedComment = -1;
                            selectedEdge = edgeIndex;
                        } else {
                            selectedNode = 0;
                            selectedComment = -1;
                            selectedEdge = -1;
                        }
                    }
                }
            }
        }

        if (draggingNode != 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 world = screenToWorld(mouse);
                const float newX = world.x - dragOffset.x;
                const float newY = world.y - dragOffset.y;
                graph::GraphNode* node = asset.findNode(draggingNode);
                if (node && (node->x != newX || node->y != newY)) {
                    if (dragUndoPending) {
                        asset = dragStartAsset;
                        dragUndoPending = false;
                        pushUndo();
                        node = asset.findNode(draggingNode);
                    }
                    if (node) {
                        node->x = newX;
                        node->y = newY;
                        dragMoved = true;
                        markDirty();
                    }
                }
            } else {
                draggingNode = 0;
                dragUndoPending = false;
            }
        }

        if (draggingComment >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 world = screenToWorld(mouse);
                if (draggingComment < static_cast<int>(asset.comments.size())) {
                    graph::GraphComment& comment = asset.comments[draggingComment];
                    const float newX = world.x - dragOffset.x;
                    const float newY = world.y - dragOffset.y;
                    if (comment.x != newX || comment.y != newY) {
                        if (dragUndoPending) {
                            asset = dragStartAsset;
                            dragUndoPending = false;
                            pushUndo();
                        }
                        if (draggingComment < static_cast<int>(asset.comments.size())) {
                            asset.comments[draggingComment].x = newX;
                            asset.comments[draggingComment].y = newY;
                            dragMoved = true;
                            markDirty();
                        }
                    }
                }
            } else {
                draggingComment = -1;
                dragUndoPending = false;
            }
        }

        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            const int nodeIndex = hitTestNode(mouse);
            if (nodeIndex >= 0) {
                selectNode(nodeViews[static_cast<std::size_t>(nodeIndex)].id);
                ImGui::OpenPopup("##graph_node_menu");
            } else {
                const int edgeIndex = hitTestEdge(mouse);
                if (edgeIndex >= 0) {
                    selectedNode = 0;
                    selectedComment = -1;
                    selectedEdge = edgeIndex;
                    ImGui::OpenPopup("##graph_edge_menu");
                } else {
                    openPalette(screenToWorld(mouse));
                }
            }
        }

        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput && !popupOpen) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                deleteSelection();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
                duplicateSelection();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (io.KeyShift) redo();
                else undo();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
                redo();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
                save();
            }
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
                frameAllRequested = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                linking = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                openPalette(screenToWorld(mouse));
            }
        }

        (void)size;
    }

    void GraphEditorWindow::openPalette(const ImVec2& worldPos) {
        paletteWorldPos = worldPos;
        paletteRequested = true;
    }

    void GraphEditorWindow::drawPalettePopup() {
        ImGui::SetNextWindowPos(worldToScreen(paletteWorldPos.x, paletteWorldPos.y), ImGuiCond_Appearing);
        if (!ImGui::BeginPopup("##graph_palette")) return;

        ImGui::TextUnformatted("Add Node");
        ImGui::Separator();
        if (paletteFocus) {
            ImGui::SetKeyboardFocusHere();
            paletteFocus = false;
        }
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##palette_search", "Search nodes...", paletteSearch, sizeof(paletteSearch));

        ImGui::BeginChild("##palette_list", ImVec2(340.0f, 330.0f));
        const std::string query = toLower(paletteSearch);

        if (matchesQuery("comment note layout", query)) {
            if (ImGui::Selectable(ICON_FA_COMMENT " Comment")) {
                addComment(paletteWorldPos);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Separator();

        const graph::NodeCatalog& catalog = graph::NodeCatalog::core();
        const std::vector<graph::GraphNodeDef>& definitions = catalog.getDefinitions();
        std::string lastCategory;
        for (const graph::GraphNodeDef& def : definitions) {
            if (!matchesQuery(def.title + " " + def.kind + " " + def.category, query)) continue;

            if (def.category != lastCategory) {
                if (!lastCategory.empty()) ImGui::Spacing();
                ImGui::TextDisabled("%s", def.category.c_str());
                lastCategory = def.category;
            }

            bool disabled = false;
            std::string hint;
            if (!nodeKindEventId(def.kind).empty() && eventKindUsed(def.kind)) {
                disabled = true;
                hint = "This event already has an entry node";
            }
            if ((def.kind == "var.get" || def.kind == "var.set") && asset.variables.empty()) {
                disabled = true;
                hint = "Add a variable first";
            }

            ImGui::BeginDisabled(disabled);
            if (ImGui::Selectable((def.title + "##" + def.kind).c_str())) {
                addNode(def.kind, paletteWorldPos);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", hint.c_str());
            }
            if (ImGui::IsItemHovered() && !disabled) {
                ImGui::SetTooltip("%s", def.kind.c_str());
            }
        }

        ImGui::EndChild();
        ImGui::EndPopup();
    }

    void GraphEditorWindow::addNode(const std::string& kind, const ImVec2& worldPos) {
        const graph::NodeCatalog& catalog = graph::NodeCatalog::core();
        const graph::GraphNodeDef* def = catalog.find(kind);
        if (!def) {
            setStatus("Unknown node kind: " + kind, true);
            return;
        }

        const std::string eventId = nodeKindEventId(kind);
        if (!eventId.empty() && eventKindUsed(kind)) {
            setStatus("Event node already exists", true);
            return;
        }

        if ((kind == "var.get" || kind == "var.set") && asset.variables.empty()) {
            setStatus("Add a variable before using variable nodes", true);
            return;
        }

        pushUndo();

        graph::GraphNode node;
        node.id = asset.nextNodeId();
        node.kind = kind;
        node.x = worldPos.x;
        node.y = worldPos.y;
        if (kind == "var.get" || kind == "var.set") {
            node.setParam("variable", graph::GraphValue::makeString(asset.variables.front().name));
        }
        asset.nodes.push_back(node);

        if (!eventId.empty()) {
            graph::GraphEventBinding binding;
            binding.id = eventId;
            binding.entryNode = node.id;
            asset.events.push_back(binding);
        }

        selectNode(node.id);
        markDirty();
    }

    void GraphEditorWindow::addComment(const ImVec2& worldPos) {
        pushUndo();
        graph::GraphComment comment;
        comment.text = "Comment";
        comment.x = worldPos.x;
        comment.y = worldPos.y;
        asset.comments.push_back(comment);
        selectedNode = 0;
        selectedEdge = -1;
        selectedComment = static_cast<int>(asset.comments.size()) - 1;
        markDirty();
    }

    void GraphEditorWindow::removeEdgesToInput(uint32_t nodeId, const std::string& pin) {
        asset.edges.erase(std::remove_if(asset.edges.begin(), asset.edges.end(),
            [nodeId, &pin](const graph::GraphEdge& edge) {
                return edge.toNode == nodeId && edge.toPin == pin;
            }), asset.edges.end());
        selectedEdge = -1;
    }

    bool GraphEditorWindow::connectLink(const PinRef& from, const PinRef& to, std::string& error) {
        if (from.nodeId == to.nodeId) {
            error = "Cannot connect a node to itself";
            return false;
        }
        if (!from.output || to.output) {
            error = "Links go from an output to an input";
            return false;
        }
        if (from.exec != to.exec) {
            error = "Cannot mix execution and data pins";
            return false;
        }
        if (!from.exec && !graph::GraphSchema::typesCompatible(from.type, to.type)) {
            error = std::string("Cannot connect ") + graphValueTypeName(from.type) + " to "
                + graphValueTypeName(to.type);
            return false;
        }

        for (const graph::GraphEdge& edge : asset.edges) {
            if (edge.fromNode == from.nodeId && edge.fromPin == from.pin &&
                edge.toNode == to.nodeId && edge.toPin == to.pin) {
                error = "This link already exists";
                return false;
            }
        }

        pushUndo();
        removeEdgesToInput(to.nodeId, to.pin);
        graph::GraphEdge edge;
        edge.fromNode = from.nodeId;
        edge.fromPin = from.pin;
        edge.toNode = to.nodeId;
        edge.toPin = to.pin;
        asset.edges.push_back(edge);
        markDirty();
        return true;
    }

    void GraphEditorWindow::selectNode(uint32_t id) {
        selectedNode = id;
        selectedComment = -1;
        selectedEdge = -1;
    }

    graph::GraphNode* GraphEditorWindow::selectedNodePtr() {
        return selectedNode != 0 ? asset.findNode(selectedNode) : nullptr;
    }

    const graph::GraphNodeDef* GraphEditorWindow::nodeDef(const graph::GraphNode& node) const {
        return graph::NodeCatalog::core().find(node.kind);
    }

    std::vector<graph::GraphPinDef> GraphEditorWindow::nodePins(const graph::GraphNode& node) const {
        return graph::NodeCatalog::core().resolvePins(node, &asset);
    }

    bool GraphEditorWindow::isInputConnected(uint32_t nodeId, const std::string& pin) const {
        for (const graph::GraphEdge& edge : asset.edges) {
            if (edge.toNode == nodeId && edge.toPin == pin) return true;
        }
        return false;
    }

    bool GraphEditorWindow::eventKindUsed(const std::string& kind) const {
        for (const graph::GraphNode& node : asset.nodes) {
            if (node.kind == kind) return true;
        }
        const std::string eventId = nodeKindEventId(kind);
        if (!eventId.empty()) {
            for (const graph::GraphEventBinding& binding : asset.events) {
                if (binding.id == eventId) return true;
            }
        }
        return false;
    }

    void GraphEditorWindow::updateEventBindings() {
        asset.events.erase(std::remove_if(asset.events.begin(), asset.events.end(),
            [this](const graph::GraphEventBinding& binding) {
                const graph::GraphNode* node = asset.findNode(binding.entryNode);
                return !node || nodeKindEventId(node->kind) != binding.id;
            }), asset.events.end());

        for (const graph::GraphNode& node : asset.nodes) {
            const std::string eventId = nodeKindEventId(node.kind);
            if (eventId.empty()) continue;
            const bool exists = std::any_of(asset.events.begin(), asset.events.end(),
                [&eventId](const graph::GraphEventBinding& binding) { return binding.id == eventId; });
            if (!exists) {
                graph::GraphEventBinding binding;
                binding.id = eventId;
                binding.entryNode = node.id;
                asset.events.push_back(binding);
            }
        }
    }

    void GraphEditorWindow::deleteSelection() {
        if (selectedNode != 0) {
            pushUndo();
            const uint32_t id = selectedNode;
            asset.nodes.erase(std::remove_if(asset.nodes.begin(), asset.nodes.end(),
                [id](const graph::GraphNode& node) { return node.id == id; }), asset.nodes.end());
            asset.edges.erase(std::remove_if(asset.edges.begin(), asset.edges.end(),
                [id](const graph::GraphEdge& edge) { return edge.fromNode == id || edge.toNode == id; }),
                asset.edges.end());
            asset.events.erase(std::remove_if(asset.events.begin(), asset.events.end(),
                [id](const graph::GraphEventBinding& binding) { return binding.entryNode == id; }),
                asset.events.end());
            selectedNode = 0;
            updateEventBindings();
            markDirty();
        } else if (selectedComment >= 0 && selectedComment < static_cast<int>(asset.comments.size())) {
            pushUndo();
            asset.comments.erase(asset.comments.begin() + selectedComment);
            selectedComment = -1;
            markDirty();
        } else if (selectedEdge >= 0 && selectedEdge < static_cast<int>(asset.edges.size())) {
            pushUndo();
            asset.edges.erase(asset.edges.begin() + selectedEdge);
            selectedEdge = -1;
            markDirty();
        }
    }

    void GraphEditorWindow::duplicateSelection() {
        if (selectedNode != 0) {
            const graph::GraphNode* source = asset.findNode(selectedNode);
            if (!source) return;
            if (!nodeKindEventId(source->kind).empty()) {
                setStatus("Event nodes cannot be duplicated", true);
                return;
            }
            pushUndo();
            graph::GraphNode copy = *source;
            copy.id = asset.nextNodeId();
            copy.x += 30.0f;
            copy.y += 30.0f;
            asset.nodes.push_back(copy);
            selectNode(copy.id);
            markDirty();
        } else if (selectedComment >= 0 && selectedComment < static_cast<int>(asset.comments.size())) {
            pushUndo();
            graph::GraphComment copy = asset.comments[selectedComment];
            copy.x += 30.0f;
            copy.y += 30.0f;
            asset.comments.push_back(copy);
            selectedComment = static_cast<int>(asset.comments.size()) - 1;
            markDirty();
        }
    }

    void GraphEditorWindow::frameAll() {
        if (asset.nodes.empty() && asset.comments.empty()) {
            scroll = ImVec2(40.0f, 40.0f);
            zoom = 1.0f;
            return;
        }

        float minX = FLT_MAX;
        float minY = FLT_MAX;
        float maxX = -FLT_MAX;
        float maxY = -FLT_MAX;

        for (const graph::GraphNode& node : asset.nodes) {
            minX = std::min(minX, node.x);
            minY = std::min(minY, node.y);
            maxX = std::max(maxX, node.x + 200.0f);
            maxY = std::max(maxY, node.y + 120.0f);
        }
        for (const graph::GraphComment& comment : asset.comments) {
            minX = std::min(minX, comment.x);
            minY = std::min(minY, comment.y);
            maxX = std::max(maxX, comment.x + comment.width);
            maxY = std::max(maxY, comment.y + comment.height);
        }

        const float width = std::max(1.0f, maxX - minX);
        const float height = std::max(1.0f, maxY - minY);
        const float availableWidth = std::max(64.0f, canvasSize.x - 80.0f);
        const float availableHeight = std::max(64.0f, canvasSize.y - 80.0f);
        zoom = std::clamp(std::min(availableWidth / width, availableHeight / height), 0.2f, 1.25f);
        scroll.x = minX + width * 0.5f - canvasSize.x / (2.0f * zoom);
        scroll.y = minY + height * 0.5f - canvasSize.y / (2.0f * zoom);
    }

    void GraphEditorWindow::drawVariablesPanel() {
        ImGui::TextUnformatted(ICON_FA_CUBE " Variables");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PLUS "##add_variable")) {
            addVariable();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add variable");
        ImGui::Separator();

        if (asset.variables.empty()) {
            ImGui::TextDisabled("No variables yet.");
            return;
        }

        int removeIndex = -1;
        for (int index = 0; index < static_cast<int>(asset.variables.size()); ++index) {
            graph::GraphVariable& variable = asset.variables[static_cast<std::size_t>(index)];
            ImGui::PushID(index);

            const std::string label = variable.displayName.empty() ? variable.name : variable.displayName;
            if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                const std::string oldName = variable.name;
                std::string name = variable.name;
                ImGui::SetNextItemWidth(-1.0f);
                textInput("##variable_name", name, true);
                if (ImGui::IsItemActivated()) pushUndo();
                if (name != oldName) {
                    if (name.empty()) {
                        setStatus("Variable name cannot be empty", true);
                    } else if (asset.findVariable(name) != nullptr) {
                        setStatus("A variable named '" + name + "' already exists", true);
                    } else {
                        variable.name = name;
                        renameVariable(index, oldName, name);
                        markDirty();
                    }
                }

                std::string displayName = variable.displayName;
                ImGui::SetNextItemWidth(-1.0f);
                textInput("##variable_display", displayName, true);
                if (ImGui::IsItemActivated()) pushUndo();
                if (displayName != variable.displayName) {
                    variable.displayName = displayName;
                    markDirty();
                }

                static const graph::GraphValueType kVariableTypes[] = {
                    graph::GraphValueType::Bool,
                    graph::GraphValueType::Int,
                    graph::GraphValueType::Float,
                    graph::GraphValueType::String,
                    graph::GraphValueType::Vector2,
                    graph::GraphValueType::Vector3,
                    graph::GraphValueType::Vector4,
                    graph::GraphValueType::Color3,
                    graph::GraphValueType::Color4,
                    graph::GraphValueType::Entity
                };
                int currentType = 0;
                for (int t = 0; t < static_cast<int>(std::size(kVariableTypes)); ++t) {
                    if (kVariableTypes[t] == variable.type) {
                        currentType = t;
                        break;
                    }
                }

                std::vector<const char*> typeNames;
                for (graph::GraphValueType type : kVariableTypes) {
                    typeNames.push_back(graphValueTypeName(type));
                }

                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##variable_type", &currentType, typeNames.data(), static_cast<int>(typeNames.size()))) {
                    pushUndo();
                    variable.type = kVariableTypes[currentType];
                    variable.defaultValue = defaultForType(variable.type);
                    markDirty();
                }

                bool exposed = variable.exposed;
                if (ImGui::Checkbox("Exposed to Inspector", &exposed)) {
                    pushUndo();
                    variable.exposed = exposed;
                    markDirty();
                }

                ImGui::TextDisabled("Default");
                graph::GraphValue nextDefault;
                const bool changed = valueWidget("##variable_default", variable.defaultValue,
                                                 variable.type, true, nextDefault);
                if (ImGui::IsItemActivated()) pushUndo();
                if (changed) {
                    if (!ImGui::IsItemActive() && !ImGui::IsItemActivated()
                        && variable.type != graph::GraphValueType::String) {
                        pushUndo();
                    }
                    variable.defaultValue = nextDefault;
                    markDirty();
                }

                if (variableUsed(variable.name)) {
                    ImGui::TextDisabled("Used by graph nodes");
                }
                ImGui::BeginDisabled(variableUsed(variable.name));
                if (ImGui::Button(ICON_FA_TRASH " Remove")) {
                    removeIndex = index;
                }
                ImGui::EndDisabled();
            }

            ImGui::PopID();
        }

        if (removeIndex >= 0) {
            removeVariable(removeIndex);
        }
    }

    bool GraphEditorWindow::variableUsed(const std::string& name) const {
        for (const graph::GraphNode& node : asset.nodes) {
            if (node.kind != "var.get" && node.kind != "var.set") continue;
            if (node.getStringParam("variable") == name) return true;
        }
        return false;
    }

    void GraphEditorWindow::renameVariable(int index, const std::string& oldName, const std::string& newName) {
        (void)index;
        for (graph::GraphNode& node : asset.nodes) {
            if (node.kind != "var.get" && node.kind != "var.set") continue;
            if (node.getStringParam("variable") == oldName) {
                node.setParam("variable", graph::GraphValue::makeString(newName));
            }
        }
    }

    void GraphEditorWindow::addVariable() {
        pushUndo();
        std::string name = "NewVariable";
        int suffix = 1;
        while (asset.findVariable(name) != nullptr) {
            name = "NewVariable" + std::to_string(suffix++);
        }
        graph::GraphVariable variable;
        variable.name = name;
        variable.displayName = name;
        variable.type = graph::GraphValueType::Float;
        variable.defaultValue = defaultForType(variable.type);
        variable.exposed = true;
        asset.variables.push_back(variable);
        markDirty();
    }

    void GraphEditorWindow::removeVariable(int index) {
        if (index < 0 || index >= static_cast<int>(asset.variables.size())) return;
        if (variableUsed(asset.variables[static_cast<std::size_t>(index)].name)) return;
        pushUndo();
        asset.variables.erase(asset.variables.begin() + index);
        markDirty();
    }

    graph::GraphValue GraphEditorWindow::defaultForType(graph::GraphValueType type) {
        switch (type) {
            case graph::GraphValueType::Bool: return graph::GraphValue::makeBool(false);
            case graph::GraphValueType::Int: return graph::GraphValue::makeInt(0);
            case graph::GraphValueType::Float: return graph::GraphValue::makeFloat(0.0);
            case graph::GraphValueType::String: return graph::GraphValue::makeString(std::string());
            case graph::GraphValueType::Vector2: return graph::GraphValue::makeVector2(Vector2(0.0f, 0.0f));
            case graph::GraphValueType::Vector3: return graph::GraphValue::makeVector3(Vector3(0.0f, 0.0f, 0.0f));
            case graph::GraphValueType::Vector4: return graph::GraphValue::makeVector4(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            case graph::GraphValueType::Color3: return graph::GraphValue::makeColor3(Vector3(1.0f, 1.0f, 1.0f));
            case graph::GraphValueType::Color4: return graph::GraphValue::makeColor4(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
            case graph::GraphValueType::Entity: return graph::GraphValue::makeEntity(NULL_ENTITY);
            default: return graph::GraphValue::makeNone();
        }
    }

    void GraphEditorWindow::drawDetailsPanel() {
        ImGui::TextUnformatted(ICON_FA_SLIDERS " Details");
        ImGui::Separator();

        if (graph::GraphNode* node = selectedNodePtr()) {
            const graph::GraphNodeDef* def = nodeDef(*node);
            ImGui::TextDisabled("%s", node->kind.c_str());
            if (def) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", def->category.c_str());
            }

            std::string title = node->title;
            if (textInput("##node_title", title, true)) {
                node->title = title;
                markDirty();
            }
            if (ImGui::IsItemActivated()) pushUndo();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave empty to use the default title");

            if (def && def->pure) {
                ImGui::TextDisabled("Pure node (evaluated on demand)");
            }

            ImGui::SeparatorText("Parameters");
            editNodeParams(*node);

            ImGui::Separator();
            if (ImGui::Button(ICON_FA_COPY " Duplicate")) duplicateSelection();
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Delete")) deleteSelection();
        } else if (selectedComment >= 0 && selectedComment < static_cast<int>(asset.comments.size())) {
            graph::GraphComment& comment = asset.comments[selectedComment];
            std::string text = comment.text;
            ImGui::SetNextItemWidth(-1.0f);
            if (textInput("##comment_text", text)) {
                comment.text = text;
                markDirty();
            }
            if (ImGui::IsItemActivated()) pushUndo();

            ImGui::TextDisabled("Size");
            float size[2] = {comment.width, comment.height};
            if (ImGui::InputFloat2("##comment_size", size)) {
                comment.width = std::max(60.0f, size[0]);
                comment.height = std::max(40.0f, size[1]);
                markDirty();
            }
            if (ImGui::IsItemActivated()) pushUndo();

            ImGui::Separator();
            if (ImGui::Button(ICON_FA_TRASH " Delete")) deleteSelection();
        } else if (selectedEdge >= 0 && selectedEdge < static_cast<int>(asset.edges.size())) {
            const graph::GraphEdge& edge = asset.edges[selectedEdge];
            ImGui::Text("From: %u.%s", edge.fromNode, edge.fromPin.c_str());
            ImGui::Text("To:   %u.%s", edge.toNode, edge.toPin.c_str());
            ImGui::Separator();
            if (ImGui::Button(ICON_FA_TRASH " Delete Link")) deleteSelection();
        } else {
            ImGui::TextDisabled("Select a node, link or comment.");
            if (!asset.nodes.empty()) {
                ImGui::SeparatorText("Graph");
                ImGui::Text("Nodes: %zu", asset.nodes.size());
                ImGui::Text("Links: %zu", asset.edges.size());
                ImGui::Text("Variables: %zu", asset.variables.size());
                ImGui::Text("Events: %zu", asset.events.size());
            }
        }
    }

    void GraphEditorWindow::editNodeParams(graph::GraphNode& node) {
        bool hasLiteral = false;

        if (node.kind == "var.get" || node.kind == "var.set") {
            if (asset.variables.empty()) {
                ImGui::TextDisabled("No variables. Add one on the left.");
            } else {
                std::string current = node.getStringParam("variable");
                int currentIndex = 0;
                std::vector<const char*> names;
                for (int i = 0; i < static_cast<int>(asset.variables.size()); ++i) {
                    names.push_back(asset.variables[static_cast<std::size_t>(i)].name.c_str());
                    if (asset.variables[static_cast<std::size_t>(i)].name == current) currentIndex = i;
                }
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##variable_ref", &currentIndex, names.data(), static_cast<int>(names.size()))) {
                    pushUndo();
                    node.setParam("variable", graph::GraphValue::makeString(names[currentIndex]));
                    markDirty();
                }
                hasLiteral = true;
            }
        }

        if (node.kind == "method.call") {
            ImGui::TextDisabled("Reflected Lua method");

            std::string className = node.getStringParam("class");
            ImGui::Text("Class");
            ImGui::SetNextItemWidth(-1.0f);
            if (textInput("##reflected_class", className, true)) {
                if (ImGui::IsItemActivated()) pushUndo();
                node.setParam("class", graph::GraphValue::makeString(className));
                markDirty();
            }

            std::string methodName = node.getStringParam("method");
            ImGui::Text("Method");
            ImGui::SetNextItemWidth(-1.0f);
            if (textInput("##reflected_method", methodName, true)) {
                if (ImGui::IsItemActivated()) pushUndo();
                node.setParam("method", graph::GraphValue::makeString(methodName));
                markDirty();
            }

            int argumentCount = 0;
            if (const graph::GraphParam* argumentParam = node.findParam("args")) {
                argumentCount = static_cast<int>(argumentParam->value.asInt());
            }
            argumentCount = std::clamp(argumentCount, 0, 8);
            ImGui::Text("Arguments");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputInt("##reflected_args", &argumentCount)) {
                if (ImGui::IsItemActivated()) pushUndo();
                node.setParam("args", graph::GraphValue::makeInt(std::clamp(argumentCount, 0, 8)));
                markDirty();
            }
            hasLiteral = true;
        }

        const std::vector<graph::GraphPinDef> pins = nodePins(node);
        for (const graph::GraphPinDef& pin : pins) {
            if (pin.dir != graph::GraphPinDir::Input || pin.kind == graph::GraphPinKind::Exec) continue;
            if (isInputConnected(node.id, pin.name)) {
                ImGui::TextDisabled("%s: linked", pin.name.c_str());
                hasLiteral = true;
                continue;
            }

            graph::GraphValue current = pin.defaultValue;
            if (const graph::GraphParam* param = node.findParam(pin.name)) {
                if (param->value.type == pin.type) {
                    current = param->value;
                } else if (!param->value.convertTo(pin.type, current)) {
                    current = param->value;
                }
            }

            graph::GraphValue next;
            const bool changed = valueWidget(pin.name.c_str(), current, pin.type, true, next);
            if (ImGui::IsItemActivated()) pushUndo();
            if (changed) {
                if (!ImGui::IsItemActive() && !ImGui::IsItemActivated()
                    && pin.type != graph::GraphValueType::String) {
                    pushUndo();
                }
                node.setParam(pin.name, next);
                markDirty();
            }
            hasLiteral = true;
        }

        if (!hasLiteral) {
            ImGui::TextDisabled("No editable inputs.");
        }
    }

    bool GraphEditorWindow::valueWidget(const char* label, const graph::GraphValue& current,
                                        graph::GraphValueType type, bool commitText,
                                        graph::GraphValue& out) {
        switch (type) {
            case graph::GraphValueType::Bool: {
                bool value = current.asBool();
                if (ImGui::Checkbox(label, &value)) {
                    out = graph::GraphValue::makeBool(value);
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Int: {
                int value = static_cast<int>(current.asInt());
                if (ImGui::InputInt(label, &value)) {
                    out = graph::GraphValue::makeInt(value);
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Float: {
                float value = static_cast<float>(current.asFloat());
                if (ImGui::InputFloat(label, &value, 0.0f, 0.0f, "%.3f")) {
                    out = graph::GraphValue::makeFloat(static_cast<double>(value));
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::String: {
                std::string value = current.asString();
                if (textInput(label, value, commitText)) {
                    out = graph::GraphValue::makeString(value);
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Vector2: {
                const Vector2 value = current.asVector2();
                float components[2] = {value.x, value.y};
                if (ImGui::InputFloat2(label, components, "%.3f")) {
                    out = graph::GraphValue::makeVector2(Vector2(components[0], components[1]));
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Vector3: {
                const Vector3 value = current.asVector3();
                float components[3] = {value.x, value.y, value.z};
                if (ImGui::InputFloat3(label, components, "%.3f")) {
                    out = graph::GraphValue::makeVector3(Vector3(components[0], components[1], components[2]));
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Vector4: {
                const Vector4 value = current.asVector4();
                float components[4] = {value.x, value.y, value.z, value.w};
                if (ImGui::InputFloat4(label, components, "%.3f")) {
                    out = graph::GraphValue::makeVector4(Vector4(components[0], components[1], components[2], components[3]));
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Color3: {
                const Vector3 value = current.asVector3();
                float components[3] = {value.x, value.y, value.z};
                if (ImGui::ColorEdit3(label, components)) {
                    out = graph::GraphValue::makeColor3(Vector3(components[0], components[1], components[2]));
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Color4: {
                const Vector4 value = current.asVector4();
                float components[4] = {value.x, value.y, value.z, value.w};
                if (ImGui::ColorEdit4(label, components)) {
                    out = graph::GraphValue::makeColor4(Vector4(components[0], components[1], components[2], components[3]));
                    return true;
                }
                return false;
            }
            case graph::GraphValueType::Entity: {
                int value = static_cast<int>(current.asEntity());
                if (ImGui::InputInt(label, &value)) {
                    out = graph::GraphValue::makeEntity(static_cast<Entity>(std::max(0, value)));
                    return true;
                }
                return false;
            }
            default:
                ImGui::TextDisabled("%s: unsupported type", label);
                return false;
        }
    }

    bool GraphEditorWindow::textInput(const char* label, std::string& value, bool commitOnDeactivate) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        const bool changed = ImGui::InputText(label, buffer, sizeof(buffer));
        if (commitOnDeactivate) {
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                value = buffer;
                return true;
            }
            return false;
        }
        if (changed) {
            value = buffer;
            return true;
        }
        return false;
    }

    void GraphEditorWindow::drawDiagnosticsPanel() {
        refreshDiagnostics();

        ImGui::TextUnformatted(ICON_FA_BUG " Diagnostics");
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu)", diagnostics.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_RIGHT " Refresh")) {
            diagnosticsStale = true;
            refreshDiagnostics();
        }
        ImGui::Separator();

        if (diagnostics.empty()) {
            ImGui::TextColored(Theme::Colors::SuccessText, "No issues found.");
            return;
        }

        for (const graph::GraphDiagnostic& diagnostic : diagnostics) {
            const bool isError = diagnostic.severity == graph::GraphDiagnosticSeverity::Error;
            const std::string text = (diagnostic.nodeId != 0
                ? "Node " + std::to_string(diagnostic.nodeId) + ": "
                : std::string()) + diagnostic.message;
            ImGui::PushStyleColor(ImGuiCol_Text, isError ? Theme::Colors::DangerText : Theme::Colors::WarningText);
            if (ImGui::Selectable(text.c_str())) {
                if (diagnostic.nodeId != 0 && asset.findNode(diagnostic.nodeId)) {
                    selectNode(diagnostic.nodeId);
                }
            }
            ImGui::PopStyleColor();
        }
    }

    void GraphEditorWindow::refreshDiagnostics() {
        if (!diagnosticsStale) return;
        diagnostics.clear();
        graph::GraphSchema::validate(asset, diagnostics);
        diagnosticsStale = false;
    }

    bool GraphEditorWindow::hasValidationErrors() {
        refreshDiagnostics();
        return graph::GraphSchema::hasErrors(diagnostics);
    }

    void GraphEditorWindow::pushUndo() {
        undoStack.push_back(asset);
        if (undoStack.size() > kMaxUndo) {
            undoStack.erase(undoStack.begin());
        }
        redoStack.clear();
    }

    void GraphEditorWindow::undo() {
        if (undoStack.empty()) return;
        redoStack.push_back(asset);
        asset = std::move(undoStack.back());
        undoStack.pop_back();
        selectedNode = 0;
        selectedComment = -1;
        selectedEdge = -1;
        diagnosticsStale = true;
        dirty = true;
        setStatus("Undo");
    }

    void GraphEditorWindow::redo() {
        if (redoStack.empty()) return;
        undoStack.push_back(asset);
        asset = std::move(redoStack.back());
        redoStack.pop_back();
        selectedNode = 0;
        selectedComment = -1;
        selectedEdge = -1;
        diagnosticsStale = true;
        dirty = true;
        setStatus("Redo");
    }

    void GraphEditorWindow::markDirty() {
        dirty = true;
        diagnosticsStale = true;
    }

    void GraphEditorWindow::setStatus(const std::string& message, bool error) {
        std::snprintf(statusMessage, sizeof(statusMessage), "%s", message.c_str());
        statusError = error;
        statusTime = ImGui::GetTime();
        if (error) {
            Log::error("Graph Editor: %s", message.c_str());
        }
    }

    void GraphEditorWindow::setStatusf(bool error, const char* format, ...) {
        char buffer[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        setStatus(buffer, error);
    }

}
