// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "component/AnimatorControllerComponent.h"
#include "imgui.h"

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor{

    // Editor window for authoring AnimatorController state machines. It mirrors
    // the Unity Animator window: Entry / Exit / Any State nodes plus user states,
    // with directed transitions guarded by parameter conditions. Each state owns
    // animation clips, each clip can carry an audio entity and animation events.
    class AnimatorWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Animator";

        AnimatorWindow(Project* project);
        ~AnimatorWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        // Open a controller asset for editing. `entity` is the entity that owns
        // an AnimatorControllerComponent; `sceneId` its scene.
        void openForEntity(Entity entity, uint32_t sceneId);
        void openAsset(const std::filesystem::path& path);

        bool save();
        bool hasUnsavedChanges() const { return dirty; }

        Entity getTargetEntity() const { return targetEntity; }
        uint32_t getTargetSceneId() const { return targetSceneId; }

    private:
        struct PinView{
            std::string name;
            bool output = false;
            bool exec = false;
            ImVec2 pos;
        };

        struct NodeView{
            uint32_t id = 0;
            std::string kind; // "state", "entry", "exit", "anyState"
            std::string title;
            ImVec2 min;
            ImVec2 max;
            ImVec2 headerMax;
            std::vector<PinView> inputs;
            std::vector<PinView> outputs;
        };

        struct PinRef{
            uint32_t nodeId = 0;
            std::string pin;
            bool output = false;
        };

        struct TransitionView{
            uint32_t id = 0;
            uint32_t fromNode = 0;
            uint32_t toNode = 0;
            // bezier control points for drawing
            ImVec2 p0, p1, p2, p3;
        };

        void drawToolbar();
        void drawCanvas(ImVec2 size);
        void drawGrid(ImDrawList* drawList) const;
        void drawNodes(ImDrawList* drawList) const;
        void drawTransitions(ImDrawList* drawList) const;
        void drawLinkPreview(ImDrawList* drawList) const;
        void handleCanvasInput(ImVec2 size);

        void drawDetailsPanel();
        void drawStateProperties(AnimatorState& state);
        void drawClipProperties(AnimatorClip& clip);
        void drawTransitionProperties(AnimatorTransition& transition);
        void drawParameterProperties();

        void buildNodeViews();
        void openPalette(const ImVec2& worldPos);
        void addState(const ImVec2& worldPos);
        void addTransition(const PinRef& from, const PinRef& to, std::string& error);
        void removeEdgesToInput(uint32_t nodeId, const std::string& pin);
        bool connectLink(const PinRef& from, const PinRef& to, std::string& error);

        void selectNode(uint32_t id);
        void selectTransition(uint32_t id);
        void deleteSelection();
        void duplicateSelection();
        void frameAll();

        const NodeView* findNodeView(uint32_t id) const;
        const PinView* findPinView(uint32_t nodeId, const std::string& pin, bool output) const;
        const PinView* hitTestPin(const ImVec2& mouse, PinRef& out) const;
        int hitTestNode(const ImVec2& mouse) const;
        int hitTestTransition(const ImVec2& mouse) const;

        AnimatorState* findState(uint32_t id);
        const AnimatorState* findState(uint32_t id) const;
        AnimatorTransition* findTransition(uint32_t id);
        const AnimatorTransition* findTransition(uint32_t id) const;

        void pushUndo();
        void undo();
        void redo();
        void markDirty();
        void setStatus(const std::string& message, bool error = false);

        Project* project;

        bool windowOpen = false;
        bool focusRequested = false;
        bool dirty = false;
        bool linking = false;
        bool paletteRequested = false;
        bool showDetails = true;

        Entity targetEntity = NULL_ENTITY;
        uint32_t targetSceneId = NULL_PROJECT_SCENE;
        AnimatorControllerComponent* controller = nullptr;

        std::vector<NodeView> nodeViews;
        std::vector<TransitionView> transitionViews;
        std::vector<AnimatorControllerComponent> undoStack;
        std::vector<AnimatorControllerComponent> redoStack;

        ImVec2 canvasOrigin;
        ImVec2 canvasSize;
        ImVec2 scroll{40.0f, 40.0f};
        float zoom = 1.0f;

        uint32_t selectedNode = 0;
        uint32_t selectedTransition = 0;
        bool hasSelectedTransition = false;

        uint32_t draggingNode = 0;
        int draggingComment = -1;
        ImVec2 dragOffset;
        bool dragMoved = false;
        bool dragUndoPending = false;
        AnimatorControllerComponent dragStartController;

        PinRef linkFrom;
        ImVec2 paletteWorldPos;

        char paletteSearch[128] = {};
        char statusMessage[256] = {};
        bool statusError = false;
        double statusTime = 0.0;

        int nextStateId = 1;
        int nextTransitionId = 1;
        int nextClipId = 1;
    };

}
