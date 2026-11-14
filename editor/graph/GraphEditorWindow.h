// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "graph/Graph.h"
#include "graph/GraphSchema.h"

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    // Authoring window for visual-script graphs. It edits the YAML source of
    // truth (.graph) and, on demand, compiles the DGR1 blob (.gdat) the runtime
    // loads from the asset directory.
    class GraphEditorWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Graph Editor";

        GraphEditorWindow() = default;

        // Accepts both the authoring .graph and a compiled .gdat (the authoring
        // file is resolved from it), so double-clicking either in Resources opens
        // the right document.
        void openFile(const std::filesystem::path& path);

        void show();

        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        bool save();
        bool compile();
        bool hasUnsavedChanges() const { return dirty; }
        const std::filesystem::path& getFilePath() const { return filePath; }

    private:
        struct PinView {
            std::string name;
            bool output = false;
            bool exec = false;
            graph::GraphValueType type = graph::GraphValueType::None;
            ImVec2 pos;
        };

        struct NodeView {
            size_t index = 0;
            uint32_t id = 0;
            ImVec2 min;
            ImVec2 max;
            ImVec2 headerMax;
            std::vector<PinView> inputs;
            std::vector<PinView> outputs;
        };

        struct PinRef {
            uint32_t nodeId = 0;
            std::string pin;
            bool output = false;
            bool exec = false;
            graph::GraphValueType type = graph::GraphValueType::None;
        };

        void drawToolbar();
        void drawCanvas(ImVec2 size);
        void drawVariablesPanel();
        void drawDetailsPanel();
        void drawDiagnosticsPanel();
        void drawPalettePopup();

        void buildNodeViews();
        void drawGrid(ImDrawList* drawList) const;
        void drawEdges(ImDrawList* drawList) const;
        void drawNodes(ImDrawList* drawList) const;
        void handleCanvasInput(ImVec2 size);
        void drawLinkPreview(ImDrawList* drawList) const;

        void openPalette(const ImVec2& worldPos);
        void addNode(const std::string& kind, const ImVec2& worldPos);
        void addComment(const ImVec2& worldPos);
        void removeEdgesToInput(uint32_t nodeId, const std::string& pin);
        bool connectLink(const PinRef& from, const PinRef& to, std::string& error);

        void selectNode(uint32_t id);
        void deleteSelection();
        void duplicateSelection();
        void frameAll();
        void updateEventBindings();

        const GraphEditorWindow::NodeView* findNodeView(uint32_t id) const;
        const PinView* findPinView(uint32_t nodeId, const std::string& pin, bool output) const;
        const PinView* hitTestPin(const ImVec2& mouse, PinRef& out) const;
        int hitTestNode(const ImVec2& mouse) const;
        int hitTestComment(const ImVec2& mouse) const;
        int hitTestEdge(const ImVec2& mouse) const;

        ImVec2 worldToScreen(float x, float y) const;
        ImVec2 screenToWorld(const ImVec2& p) const;

        graph::GraphNode* selectedNodePtr();
        const graph::GraphNodeDef* nodeDef(const graph::GraphNode& node) const;
        std::vector<graph::GraphPinDef> nodePins(const graph::GraphNode& node) const;
        bool isInputConnected(uint32_t nodeId, const std::string& pin) const;
        bool eventKindUsed(const std::string& kind) const;

        void editNodeParams(graph::GraphNode& node);
        void editVariable(int index);
        void renameVariable(int index, const std::string& oldName, const std::string& newName);
        bool variableUsed(const std::string& name) const;
        void addVariable();
        void removeVariable(int index);

        bool valueWidget(const char* label, const graph::GraphValue& current,
                         graph::GraphValueType type, bool commitText,
                         graph::GraphValue& out);
        bool textInput(const char* label, std::string& value, bool commitOnDeactivate = false);
        static graph::GraphValue defaultForType(graph::GraphValueType type);

        void refreshDiagnostics();
        bool hasValidationErrors();
        void pushUndo();
        void undo();
        void redo();
        void markDirty();
        void setStatus(const std::string& message, bool error = false);
        void setStatusf(bool error, const char* format, ...);

        bool windowOpen = false;
        bool focusRequested = false;
        bool dirty = false;
        bool diagnosticsStale = true;
        bool linking = false;
        bool paletteRequested = false;
        bool paletteFocus = false;
        bool showVariables = true;
        bool showDetails = true;
        bool showDiagnostics = true;
        bool frameAllRequested = false;

        std::filesystem::path filePath;
    std::filesystem::path pendingOpenPath;
        graph::GraphAsset asset;
        std::vector<NodeView> nodeViews;
        std::vector<graph::GraphDiagnostic> diagnostics;
        std::vector<graph::GraphAsset> undoStack;
        std::vector<graph::GraphAsset> redoStack;

        ImVec2 canvasOrigin;
        ImVec2 canvasSize;
        ImVec2 scroll{40.0f, 40.0f};
        float zoom = 1.0f;

        uint32_t selectedNode = 0;
        int selectedComment = -1;
        int selectedEdge = -1;

        uint32_t draggingNode = 0;
        int draggingComment = -1;
        ImVec2 dragOffset;
        bool dragMoved = false;
        bool dragUndoPending = false;
        graph::GraphAsset dragStartAsset;

        PinRef linkFrom;
        ImVec2 paletteWorldPos;

        char paletteSearch[128] = {};
        char statusMessage[256] = {};
        bool statusError = false;
        double statusTime = 0.0;
    };

    // The editor App owns the single instance; Properties and Resources reach it
    // through this accessor instead of widening the frozen App.h surface.
    void setGraphEditorWindow(GraphEditorWindow* window);
    GraphEditorWindow* getGraphEditorWindow();

}
