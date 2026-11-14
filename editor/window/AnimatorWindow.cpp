// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorWindow.h"
#include "Catalog.h"
#include "Stream.h"
#include "App.h"
#include "Scene.h"
#include "SceneProject.h"
#include "component/AnimationComponent.h"
#include "component/SoundComponent.h"
#include "util/Color.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace quantum::editor{

    namespace{

        const ImVec4 COLOR_ENTRY{0.20f, 0.68f, 0.35f, 1.00f};
        const ImVec4 COLOR_EXIT{0.85f, 0.20f, 0.20f, 1.00f};
        const ImVec4 COLOR_ANY{0.20f, 0.60f, 0.85f, 1.00f};
        const ImVec4 COLOR_STATE{0.42f, 0.42f, 0.48f, 1.00f};
        const ImVec4 COLOR_TRANSITION{0.70f, 0.70f, 0.75f, 1.00f};
        const ImVec4 COLOR_SELECTED{1.00f, 0.85f, 0.20f, 1.00f};

        ImVec4 typeColor(AnimatorParameter::Type t){
            switch(t){
                case AnimatorParameter::Type::Float: return ImVec4(0.35f, 0.75f, 1.00f, 1.00f);
                case AnimatorParameter::Type::Int:   return ImVec4(0.75f, 0.45f, 1.00f, 1.00f);
                case AnimatorParameter::Type::Bool:  return ImVec4(0.35f, 1.00f, 0.55f, 1.00f);
                case AnimatorParameter::Type::Trigger: return ImVec4(1.00f, 0.55f, 0.35f, 1.00f);
            }
            return ImVec4(1,1,1,1);
        }

        // Entities in the scene that carry T, for the clip pickers.
        template<typename T>
        void collectEntityCandidates(Scene* scene, std::vector<Entity>& out){
            auto comps = scene->getComponentArray<T>();
            for(int i = 0; i < (int)comps->size(); i++){
                Entity e = comps->getEntity(i);
                if(scene->isEntityCreated(e))
                    out.push_back(e);
            }
        }

        // "<none>" first, then one entry per candidate entity by name.
        bool entityCombo(const char* label, Entity& value, const std::vector<Entity>& candidates, Scene* scene){
            int idx = -1;
            for(int i = 0; i < (int)candidates.size(); i++)
                if(candidates[i] == value){ idx = i; break; }

            std::vector<std::string> names;
            names.push_back("<none>");
            for(Entity e : candidates){
                std::string n = scene->getEntityName(e);
                if(n.empty())
                    n = "Entity " + std::to_string((uint32_t)e);
                names.push_back(n);
            }
            std::vector<const char*> items;
            for(const std::string& n : names)
                items.push_back(n.c_str());

            int display = idx + 1;
            if(ImGui::Combo(label, &display, items.data(), (int)items.size())){
                value = display == 0 ? NULL_ENTITY : candidates[display - 1];
                return true;
            }
            return false;
        }

    }

    AnimatorWindow::AnimatorWindow(Project* project): project(project){}

    AnimatorWindow::~AnimatorWindow() = default;

    void AnimatorWindow::openForEntity(Entity entity, uint32_t sceneId){
        targetEntity = entity;
        targetSceneId = sceneId;
        SceneProject* sp = project->getScene(sceneId);
        Scene* scene = sp ? sp->scene : nullptr;
        if(!scene) return;
        controller = scene->findComponent<AnimatorControllerComponent>(entity);
        if(!controller) return;
        windowOpen = true;
        focusRequested = true;
        buildNodeViews();
        dirty = false;
    }

    void AnimatorWindow::openAsset(const std::filesystem::path& path){
        // Asset path opening is left for future asset-browser integration; the
        // entity-driven path is the primary authoring flow.
        (void)path;
    }

    bool AnimatorWindow::save(){
        if(!controller || targetEntity == NULL_ENTITY) return false;
        SceneProject* sp = project->getScene(targetSceneId);
        Scene* scene = sp ? sp->scene : nullptr;
        if(!scene) return false;
        scene->getComponent<AnimatorControllerComponent>(targetEntity) = *controller;
        dirty = false;
        setStatus("Animator controller saved.");
        return true;
    }

    void AnimatorWindow::show(){
        if(!windowOpen) return;
        if(focusRequested){ ImGui::SetNextWindowFocus(); focusRequested = false; }

        ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse;
        if(!ImGui::Begin(WINDOW_NAME, &windowOpen, flags)){
            ImGui::End();
            return;
        }

        drawToolbar();

        ImGui::BeginChild("split", ImVec2(0,0), true);
        {
            float width = ImGui::GetContentRegionAvail().x;
            float leftWidth = width * 0.62f;

            ImGui::BeginChild("canvas", ImVec2(leftWidth, 0), true, ImGuiWindowFlags_NoScrollbar);
            {
                ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                ImVec2 canvasSize = ImGui::GetContentRegionAvail();
                canvasSize.y = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - 8.0f;

                drawToolbar(); // mini-toolbar over canvas would go here; kept in menubar
                drawCanvas(canvasSize);

                if(ImGui::BeginPopup("animator_palette")){
                    if(ImGui::MenuItem("Add State")){
                        addState(paletteWorldPos);
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("details", ImVec2(0,0), true);
            {
                drawDetailsPanel();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::End();
    }

    void AnimatorWindow::drawToolbar(){
        if(ImGui::BeginMenuBar()){
            if(ImGui::BeginMenu("File")){
                if(ImGui::MenuItem("Save", "Ctrl+S", nullptr, controller != nullptr)) save();
                if(ImGui::MenuItem("Close")) windowOpen = false;
                ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("Node")){
                bool canAdd = controller != nullptr;
                if(ImGui::MenuItem("Add State", nullptr, nullptr, canAdd)) addState(ImVec2(200,200));
                if(ImGui::MenuItem("Entry", nullptr, nullptr, false)){}
                if(ImGui::MenuItem("Exit", nullptr, nullptr, false)){}
                ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("View")){
                ImGui::MenuItem("Fit All", "F", nullptr, true);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        if(controller){
            ImGui::Text("Controller: %s", controller->controllerAsset.empty() ? "(inline)" : controller->controllerAsset.c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1,1,0,1), "%s", dirty ? "unsaved" : "");
        }else{
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Select an entity with an AnimatorControllerComponent.");
        }
    }

    void AnimatorWindow::drawCanvas(ImVec2 size){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        canvasOrigin = origin;
        canvasSize = size;

        // interaction area
        ImGui::InvisibleButton("canvas", size);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 canvasMouse = {mouse.x - origin.x, mouse.y - origin.y};

        handleCanvasInput(size);

        drawGrid(dl);
        drawTransitions(dl);
        drawNodes(dl);
        drawLinkPreview(dl);

        if(statusTime > 0.0f && ImGui::GetTime() - statusTime < 3.0){
            ImGui::SetCursorScreenPos({origin.x + 10, origin.y + size.y - 24});
            ImGui::PushStyleColor(ImGuiCol_Text, statusError ? ImVec4(1,0.4f,0.4f,1) : ImVec4(0.8f,0.8f,0.8f,1));
            ImGui::Text("%s", statusMessage);
            ImGui::PopStyleColor();
        }
    }

    void AnimatorWindow::drawGrid(ImDrawList* dl) const{
        const float gridSize = 40.0f * zoom;
        ImVec2 origin = canvasOrigin;
        ImVec2 size = canvasSize;

        ImU32 color = ImGui::GetColorU32(ImGuiCol_Border) & 0x44FFFFFF;
        for(float x = fmodf(-scroll.x, gridSize); x < size.x; x += gridSize)
            dl->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, color, 1.0f);
        for(float y = fmodf(-scroll.y, gridSize); y < size.y; y += gridSize)
            dl->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, color, 1.0f);
    }

    void AnimatorWindow::buildNodeViews(){
        nodeViews.clear();
        transitionViews.clear();

        if(!controller) return;

        // Special nodes first
        auto addSpecial = [&](uint32_t id, const char* title, ImVec4 color, float x, float y){
            NodeView nv;
            nv.id = id;
            nv.kind = title;
            nv.title = title;
            nv.min = {x, y};
            nv.max = {x + 140.0f, y + 44.0f};
            nv.headerMax = {x + 140.0f, y + 22.0f};
            PinView p; p.name = title; p.output = true; p.pos = {nv.max.x, (nv.min.y+nv.max.y)*0.5f}; nv.outputs.push_back(p);
            PinView p2; p2.name = title; p2.output = false; p2.pos = {nv.min.x, (nv.min.y+nv.max.y)*0.5f}; nv.inputs.push_back(p2);
            nodeViews.push_back(nv);
        };

        float cx = 60.0f;
        addSpecial(ANIMATOR_ENTRY_STATE, "Entry", COLOR_ENTRY, cx, 60.0f);
        addSpecial(ANIMATOR_EXIT_STATE, "Exit", COLOR_EXIT, cx + 260.0f, 60.0f);
        addSpecial(ANIMATOR_ANY_STATE, "Any State", COLOR_ANY, cx + 520.0f, 60.0f);

        // User states
        for(const auto& st : controller->states){
            NodeView nv;
            nv.id = st.id;
            nv.kind = "state";
            nv.title = st.name.empty() ? "State" : st.name;
            nv.min = {st.x, st.y};
            nv.max = {st.x + 160.0f, st.y + 110.0f};
            nv.headerMax = {st.x + 160.0f, st.y + 26.0f};
            // clips as output pins
            for(const auto& clip : st.clips){
                PinView p; p.name = clip.name.empty() ? "Clip" : clip.name; p.output = true; p.pos = {nv.max.x, nv.min.y + 34.0f + 20.0f * (size_t)(&clip - st.clips.data())}; nv.outputs.push_back(p);
            }
            PinView p; p.name = nv.title; p.output = false; p.pos = {nv.min.x, (nv.min.y+nv.max.y)*0.5f}; nv.inputs.push_back(p);
            nodeViews.push_back(nv);
            nextStateId = std::max(nextStateId, (int)st.id + 1);
        }

        // Transitions
        for(const auto& tr : controller->transitions){
            TransitionView tv;
            tv.id = tr.id;
            tv.fromNode = tr.fromState;
            tv.toNode = tr.toState;

            // Cubic bezier from the source node's right edge to the target's left
            // edge, in the same world space the nodes are drawn in.
            const NodeView* from = nullptr;
            const NodeView* to = nullptr;
            for(const auto& nv : nodeViews){
                if(nv.id == tr.fromState) from = &nv;
                if(nv.id == tr.toState) to = &nv;
            }
            if(from && to){
                tv.p0 = {from->max.x, (from->min.y + from->max.y) * 0.5f};
                tv.p3 = {to->min.x, (to->min.y + to->max.y) * 0.5f};
                float dx = std::max(40.0f, std::abs(tv.p3.x - tv.p0.x) * 0.5f);
                tv.p1 = {tv.p0.x + dx, tv.p0.y};
                tv.p2 = {tv.p3.x - dx, tv.p3.y};
            }
            transitionViews.push_back(tv);
            nextTransitionId = std::max(nextTransitionId, (int)tr.id + 1);
        }
    }

    const AnimatorWindow::NodeView* AnimatorWindow::findNodeView(uint32_t id) const{
        for(const auto& nv : nodeViews) if(nv.id == id) return &nv;
        return nullptr;
    }

    const AnimatorWindow::PinView* AnimatorWindow::findPinView(uint32_t nodeId, const std::string& pin, bool output) const{
        const NodeView* nv = findNodeView(nodeId);
        if(!nv) return nullptr;
        const auto& pins = output ? nv->outputs : nv->inputs;
        for(const auto& p : pins) if(p.name == pin) return &p;
        return nullptr;
    }

    int AnimatorWindow::hitTestNode(const ImVec2& mouse) const{
        for(const auto& nv : nodeViews){
            if(mouse.x >= nv.min.x && mouse.x <= nv.max.x && mouse.y >= nv.min.y && mouse.y <= nv.max.y)
                return (int)nv.id;
        }
        return -1;
    }

    int AnimatorWindow::hitTestTransition(const ImVec2& mouse) const{
        // Simple hit test against transition bezier bounding boxes
        for(size_t i = 0; i < transitionViews.size(); ++i){
            const auto& tv = transitionViews[i];
            float minx = std::min({tv.p0.x, tv.p1.x, tv.p2.x, tv.p3.x});
            float maxx = std::max({tv.p0.x, tv.p1.x, tv.p2.x, tv.p3.x});
            float miny = std::min({tv.p0.y, tv.p1.y, tv.p2.y, tv.p3.y});
            float maxy = std::max({tv.p0.y, tv.p1.y, tv.p2.y, tv.p3.y});
            if(mouse.x >= minx - 10 && mouse.x <= maxx + 10 && mouse.y >= miny - 10 && mouse.y <= maxy + 10)
                return (int)i;
        }
        return -1;
    }

    const AnimatorWindow::PinView* AnimatorWindow::hitTestPin(const ImVec2& mouse, PinRef& out) const{
        const float hit = 8.0f;
        for(const auto& nv : nodeViews){
            for(const auto& p : nv.outputs){
                ImVec2 wp = {canvasOrigin.x + p.pos.x, canvasOrigin.y + p.pos.y};
                if(ImGui::IsMouseHoveringRect({wp.x-hit, wp.y-hit}, {wp.x+hit, wp.y+hit}, true)){
                    out = {nv.id, p.name, true}; return &p;
                }
            }
            for(const auto& p : nv.inputs){
                ImVec2 wp = {canvasOrigin.x + p.pos.x, canvasOrigin.y + p.pos.y};
                if(ImGui::IsMouseHoveringRect({wp.x-hit, wp.y-hit}, {wp.x+hit, wp.y+hit}, true)){
                    out = {nv.id, p.name, false}; return &p;
                }
            }
        }
        return nullptr;
    }

    AnimatorState* AnimatorWindow::findState(uint32_t id){
        if(id >= 1000) return nullptr; // special nodes have no state data
        for(auto& st : controller->states) if(st.id == id) return &st;
        return nullptr;
    }

    const AnimatorState* AnimatorWindow::findState(uint32_t id) const{
        if(id >= 1000) return nullptr; // special nodes have no state data
        for(const auto& st : controller->states) if(st.id == id) return &st;
        return nullptr;
    }

    AnimatorTransition* AnimatorWindow::findTransition(uint32_t id){
        for(auto& tr : controller->transitions) if(tr.id == id) return &tr;
        return nullptr;
    }

    const AnimatorTransition* AnimatorWindow::findTransition(uint32_t id) const{
        for(const auto& tr : controller->transitions) if(tr.id == id) return &tr;
        return nullptr;
    }

    void AnimatorWindow::drawNodes(ImDrawList* dl) const{
        ImVec2 origin = canvasOrigin;
        for(const auto& nv : nodeViews){
            ImVec4 fill = COLOR_STATE;
            if(nv.kind == "Entry") fill = COLOR_ENTRY;
            else if(nv.kind == "Exit") fill = COLOR_EXIT;
            else if(nv.kind == "Any State") fill = COLOR_ANY;

            bool selected = (selectedNode == nv.id);
            ImU32 fillCol = ImGui::GetColorU32(fill);
            ImU32 borderCol = selected ? ImGui::GetColorU32(COLOR_SELECTED) : ImGui::GetColorU32(ImGuiCol_Border);

            dl->AddRectFilled({origin.x + nv.min.x, origin.y + nv.min.y},
                              {origin.x + nv.max.x, origin.y + nv.max.y},
                              fillCol, 6.0f);
            dl->AddRect({origin.x + nv.min.x, origin.y + nv.min.y},
                        {origin.x + nv.max.x, origin.y + nv.max.y},
                        borderCol, 6.0f, 0, selected ? 2.0f : 1.0f);

            // header
            dl->AddRectFilled({origin.x + nv.min.x, origin.y + nv.min.y},
                              {origin.x + nv.max.x, origin.y + nv.headerMax.y},
                              fillCol, 6.0f, ImDrawFlags_RoundCornersTop);
            dl->AddText({origin.x + nv.min.x + 8.0f, origin.y + nv.min.y + 6.0f},
                        ImGui::GetColorU32(ImGuiCol_Text), nv.title.c_str());

            // pins
            for(const auto& p : nv.outputs){
                dl->AddCircleFilled({origin.x + p.pos.x, origin.y + p.pos.y}, 5.0f, ImGui::GetColorU32(ImGuiCol_PlotLines));
            }
            for(const auto& p : nv.inputs){
                dl->AddCircleFilled({origin.x + p.pos.x, origin.y + p.pos.y}, 5.0f, ImGui::GetColorU32(ImGuiCol_PlotLines));
            }
        }
    }

    void AnimatorWindow::drawTransitions(ImDrawList* dl) const{
        ImVec2 origin = canvasOrigin;
        for(size_t i = 0; i < transitionViews.size(); ++i){
            const auto& tv = transitionViews[i];
            const NodeView* from = findNodeView(tv.fromNode);
            const NodeView* to = findNodeView(tv.toNode);
            if(!from || !to) continue;

            ImVec2 p0 = {origin.x + tv.p0.x, origin.y + tv.p0.y};
            ImVec2 p1 = {origin.x + tv.p1.x, origin.y + tv.p1.y};
            ImVec2 p2 = {origin.x + tv.p2.x, origin.y + tv.p2.y};
            ImVec2 p3 = {origin.x + tv.p3.x, origin.y + tv.p3.y};

            bool selected = hasSelectedTransition && selectedTransition == tv.id;
            ImU32 col = selected ? ImGui::GetColorU32(COLOR_SELECTED) : ImGui::GetColorU32(COLOR_TRANSITION);
            dl->AddBezierCubic(p0, p1, p2, p3, col, selected ? 2.5f : 1.5f);

            // midpoint label
            ImVec2 mid = {(p0.x + p3.x) * 0.5f, (p0.y + p3.y) * 0.5f};
            const AnimatorTransition* tr = findTransition(tv.id);
            if(tr){
                ImGui::SetCursorScreenPos(mid);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.7f,0.7f,1));
                ImGui::TextUnformatted(tr->name.empty() ? "Transition" : tr->name.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

    void AnimatorWindow::drawLinkPreview(ImDrawList* dl) const{
        if(!linking) return;
        ImVec2 origin = canvasOrigin;
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 fromPin;
        const PinView* pv = nullptr;
        for(const auto& nv : nodeViews){
            for(const auto& p : nv.outputs){
                ImVec2 wp = {origin.x + p.pos.x, origin.y + p.pos.y};
                if(ImGui::IsMouseHoveringRect({wp.x-8,wp.y-8},{wp.x+8,wp.y+8},true)){ pv = &p; fromPin = wp; break; }
            }
            if(pv) break;
        }
        if(!pv) return;
        dl->AddBezierCubic(fromPin, {fromPin.x + 80, fromPin.y}, {mouse.x - 80, mouse.y}, mouse, ImGui::GetColorU32(COLOR_TRANSITION), 2.0f);
    }

    void AnimatorWindow::handleCanvasInput(ImVec2 size){
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 canvasMouse = {mouse.x - canvasOrigin.x, mouse.y - canvasOrigin.y};

        // panning
        if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)){
            scroll.x += ImGui::GetIO().MouseDelta.x;
            scroll.y += ImGui::GetIO().MouseDelta.y;
        }
        // zoom
        float wheel = ImGui::GetIO().MouseWheel;
        if(wheel != 0.0f && ImGui::IsWindowHovered()){
            float beforeZoom = zoom;
            zoom += wheel * 0.1f;
            if(zoom < 0.3f) zoom = 0.3f;
            if(zoom > 2.0f) zoom = 2.0f;
            // zoom toward mouse
            ImVec2 mouseBefore = {canvasMouse.x / beforeZoom + scroll.x, canvasMouse.y / beforeZoom + scroll.y};
            scroll.x = mouseBefore.x - canvasMouse.x / zoom;
            scroll.y = mouseBefore.y - canvasMouse.y / zoom;
        }

        ImVec2 worldMouse = {canvasMouse.x / zoom + scroll.x, canvasMouse.y / zoom + scroll.y};

        // node drag
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !linking){
            int hit = hitTestNode(worldMouse);
            if(hit >= 0){
                selectedNode = (uint32_t)hit;
                selectedTransition = 0; hasSelectedTransition = false;
                draggingNode = (uint32_t)hit;
                dragOffset = {worldMouse.x, worldMouse.y};
                // find node offset
                for(auto& nv : nodeViews) if(nv.id == draggingNode){
                    dragOffset.x -= nv.min.x; dragOffset.y -= nv.min.y; break;
                }
                dragMoved = false;
                dragStartController = *controller;
                dragUndoPending = true;
            }else{
                int thit = hitTestTransition(worldMouse);
                if(thit >= 0){
                    selectedTransition = (uint32_t)thit; hasSelectedTransition = true;
                    selectedNode = 0;
                }else{
                    selectedNode = 0; selectedTransition = 0; hasSelectedTransition = false;
                }
            }
        }

        if(ImGui::IsMouseDragging(ImGuiMouseButton_Left) && draggingNode != 0){
            dragMoved = true;
            for(auto& nv : nodeViews){
                if(nv.id == draggingNode){
                    nv.min.x = worldMouse.x + dragOffset.x; nv.min.y = worldMouse.y + dragOffset.y;
                    nv.max.x = nv.min.x + (nv.max.x - nv.min.x); // keep width
                    nv.headerMax.x = nv.max.x;
                    // persist to controller state
                    if(AnimatorState* st = findState(nv.id)){
                        st->x = nv.min.x; st->y = nv.min.y;
                    }
                    markDirty();
                    break;
                }
            }
        }

        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
            if(dragUndoPending && dragMoved && controller){
                pushUndo();
                dragUndoPending = false;
            }
            draggingNode = 0;
        }

        // link from pin
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !linking){
            PinRef pr;
            if(hitTestPin(worldMouse, pr) && pr.output){
                linkFrom = pr;
                linking = true;
            }
        }

        if(ImGui::IsMouseDragging(ImGuiMouseButton_Left) && linking){
            // visual preview handled in drawLinkPreview
        }

        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left) && linking){
            PinRef pr;
            if(hitTestPin(worldMouse, pr) && !pr.output){
                // allow connecting output -> input only; same node or special->state etc handled below
                if(pr.nodeId != linkFrom.nodeId || pr.pin != linkFrom.pin){
                    std::string err;
                    if(connectLink(linkFrom, pr, err)){
                        pushUndo();
                        markDirty();
                    }else{
                        setStatus(err, true);
                    }
                }
            }
            linking = false;
            linkFrom = {};
        }

        if(ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)){
            if(!ImGui::GetIO().WantTextInput)
                deleteSelection();
        }

        // right-click on empty canvas: create-state palette
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hitTestNode(worldMouse) < 0){
            openPalette(worldMouse);
            ImGui::OpenPopup("animator_palette");
        }

        if(ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput){
            if(ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
            if(ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
            if(ImGui::IsKeyPressed(ImGuiKey_S)) save();
        }
    }

    bool AnimatorWindow::connectLink(const PinRef& from, const PinRef& to, std::string& error){
        (void)error;
        // A link from a state/output or special node output to another node's input creates
        // a transition from the source node to the target node.
        uint32_t fromNode = from.nodeId;
        uint32_t toNode = to.nodeId;
        if(fromNode == toNode){
            error = "Cannot transition to self via link (use condition).";
            return false;
        }

        // avoid duplicates
        for(const auto& tr : controller->transitions)
            if(tr.fromState == fromNode && tr.toState == toNode){ return false; }

        AnimatorTransition tr;
        tr.id = (uint32_t)nextTransitionId++;
        tr.fromState = fromNode;
        tr.toState = toNode;
        tr.name = "Transition";
        controller->transitions.push_back(tr);
        return true;
    }

    void AnimatorWindow::addState(const ImVec2& worldPos){
        if(!controller) return;
        pushUndo();
        AnimatorState st;
        st.id = (uint32_t)nextStateId++;
        st.name = "New State";
        st.x = worldPos.x; st.y = worldPos.y;
        st.isDefault = controller->states.empty(); // first state is default
        controller->states.push_back(st);
        markDirty();
        buildNodeViews();
    }

    void AnimatorWindow::removeEdgesToInput(uint32_t nodeId, const std::string& pin){
        (void)pin;
        auto it = controller->transitions.begin();
        while(it != controller->transitions.end()){
            if(it->toState == nodeId) it = controller->transitions.erase(it);
            else ++it;
        }
    }

    void AnimatorWindow::selectNode(uint32_t id){
        selectedNode = id;
        selectedTransition = 0; hasSelectedTransition = false;
    }

    void AnimatorWindow::selectTransition(uint32_t id){
        selectedTransition = id; hasSelectedTransition = true;
        selectedNode = 0;
    }

    void AnimatorWindow::deleteSelection(){
        if(!controller) return;
        pushUndo();
        if(selectedNode != 0){
            // remove state and all transitions touching it
            controller->states.erase(std::remove_if(controller->states.begin(), controller->states.end(),
                [&](const AnimatorState& s){ return s.id == selectedNode; }), controller->states.end());
            controller->transitions.erase(std::remove_if(controller->transitions.begin(), controller->transitions.end(),
                [&](const AnimatorTransition& t){ return t.fromState == selectedNode || t.toState == selectedNode; }), controller->transitions.end());
            selectedNode = 0;
        }else if(hasSelectedTransition && selectedTransition != 0){
            controller->transitions.erase(std::remove_if(controller->transitions.begin(), controller->transitions.end(),
                [&](const AnimatorTransition& t){ return t.id == selectedTransition; }), controller->transitions.end());
            selectedTransition = 0; hasSelectedTransition = false;
        }
        markDirty();
        buildNodeViews();
    }

    void AnimatorWindow::duplicateSelection(){
        // not implemented yet
    }

    void AnimatorWindow::frameAll(){
        // center view on nodes
    }

    void AnimatorWindow::openPalette(const ImVec2& worldPos){
        (void)worldPos;
        paletteRequested = true;
        paletteWorldPos = worldPos;
    }

    void AnimatorWindow::pushUndo(){
        if(controller) undoStack.push_back(*controller);
        redoStack.clear();
        if(undoStack.size() > 50) undoStack.erase(undoStack.begin());
    }

    void AnimatorWindow::undo(){
        if(undoStack.empty()) return;
        redoStack.push_back(*controller);
        *controller = undoStack.back();
        undoStack.pop_back();
        markDirty();
        buildNodeViews();
    }

    void AnimatorWindow::redo(){
        if(redoStack.empty()) return;
        undoStack.push_back(*controller);
        *controller = redoStack.back();
        redoStack.pop_back();
        markDirty();
        buildNodeViews();
    }

    void AnimatorWindow::markDirty(){ dirty = true; }

    void AnimatorWindow::setStatus(const std::string& message, bool error){
        strncpy(statusMessage, message.c_str(), sizeof(statusMessage) - 1);
        statusMessage[sizeof(statusMessage) - 1] = 0;
        statusError = error;
        statusTime = ImGui::GetTime();
    }

    // ---------- Details panel ----------

    void AnimatorWindow::drawDetailsPanel(){
        if(!controller){
            ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "Open an entity with an AnimatorControllerComponent first.");
            return;
        }
        ImGui::Text("Parameters");
        if(ImGui::Button("Add Parameter", ImVec2(-1, 0))){
            pushUndo();
            AnimatorParameter p; p.name = "NewParam"; p.type = AnimatorParameter::Type::Float;
            controller->parameters.push_back(p);
            markDirty();
        }
        ImGui::SameLine();
        if(ImGui::Button("Add Trigger")){ pushUndo(); AnimatorParameter p; p.name = "NewTrigger"; p.type = AnimatorParameter::Type::Trigger; controller->parameters.push_back(p); markDirty(); }

        if(!controller->parameters.empty()){
            if(ImGui::BeginTable("params", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)){
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Default");
                ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24);
                ImGui::TableHeadersRow();
                for(size_t i = 0; i < controller->parameters.size(); ++i){
                    auto& p = controller->parameters[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char buf[128]; strncpy(buf, p.name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0;
                    if(ImGui::InputText(("##pname"+std::to_string(i)).c_str(), buf, sizeof(buf))) p.name = buf;
                    ImGui::TableNextColumn();
                    const char* types[] = {"Float","Int","Bool","Trigger"};
                    int ti = (int)p.type;
                    if(ImGui::Combo(("##ptype"+std::to_string(i)).c_str(), &ti, types, 4)) p.type = (AnimatorParameter::Type)ti;
                    ImGui::TableNextColumn();
                    if(p.type == AnimatorParameter::Type::Bool) ImGui::Checkbox(("##pval"+std::to_string(i)).c_str(), &p.boolValue);
                    else ImGui::DragFloat(("##pval"+std::to_string(i)).c_str(), &p.defaultValue, 0.01f);
                    ImGui::TableNextColumn();
                    if(ImGui::SmallButton("x")){ controller->parameters.erase(controller->parameters.begin() + (long long)i); markDirty(); --i; }
                }
                ImGui::EndTable();
            }
        }

        ImGui::Separator();
        ImGui::Text("States (%d)", (int)controller->states.size());
        if(selectedNode != 0){
            if(AnimatorState* st = findState(selectedNode)){
                drawStateProperties(*st);
            }
        }else if(hasSelectedTransition && selectedTransition != 0){
            if(AnimatorTransition* tr = findTransition(selectedTransition)){
                drawTransitionProperties(*tr);
            }
        }else{
            ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "Select a state or transition to edit.");
        }
    }

    void AnimatorWindow::drawStateProperties(AnimatorState& st){
        char buf[128]; strncpy(buf, st.name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        if(ImGui::InputText("Name", buf, sizeof(buf))) st.name = buf;
        ImGui::Checkbox("Default State", &st.isDefault);

        SceneProject* sp = project ? project->getScene(targetSceneId) : nullptr;
        Scene* scene = sp ? sp->scene : nullptr;
        std::vector<Entity> animCandidates, audioCandidates;
        if(scene){
            collectEntityCandidates<AnimationComponent>(scene, animCandidates);
            collectEntityCandidates<SoundComponent>(scene, audioCandidates);
        }

        ImGui::Separator();
        ImGui::Text("Clips");
        if(ImGui::Button("Add Clip", ImVec2(-1,0))){
            pushUndo();
            AnimatorClip clip; clip.name = "Clip " + std::to_string(st.clips.size()+1);
            st.clips.push_back(clip);
            markDirty();
        }

        for(size_t i = 0; i < st.clips.size(); ++i){
            AnimatorClip& clip = st.clips[i];
            ImGui::PushID((int)i);
            ImGui::Separator();
            ImGui::Text("Clip %d", (int)i+1);
            char cbuf[128]; strncpy(cbuf, clip.name.c_str(), sizeof(cbuf)-1); cbuf[sizeof(cbuf)-1]=0;
            ImGui::InputText("Name", cbuf, sizeof(cbuf)); clip.name = cbuf;

            // animation entity picker
            if(scene){
                if(entityCombo("Animation", clip.animation, animCandidates, scene)) markDirty();
            }else{
                ImGui::TextDisabled("Animation entity: %u (no scene)", (uint32_t)clip.animation);
            }

            ImGui::DragFloat("Speed", &clip.speed, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Offset", &clip.offset, 0.01f, 0.0f, 600.0f);
            ImGui::Checkbox("Loop", &clip.loop);

            if(scene){
                if(entityCombo("Audio", clip.audio, audioCandidates, scene)) markDirty();
            }else{
                ImGui::TextDisabled("Audio entity: %u (no scene)", (uint32_t)clip.audio);
            }

            ImGui::Separator();
            ImGui::Text("Events");
            if(ImGui::Button("Add Event", ImVec2(-1,0))){
                pushUndo();
                AnimatorClip::Event ev; ev.time = clip.speed > 0 ? 0.5f : 0.0f;
                clip.events.push_back(ev);
                markDirty();
            }
            for(size_t e = 0; e < clip.events.size(); ++e){
                auto& ev = clip.events[e];
                ImGui::PushID((int)e);
                ImGui::DragFloat("Time", &ev.time, 0.01f, 0.0f, 600.0f);
                ImGui::SameLine();
                char ebuf[128]; strncpy(ebuf, ev.functionName.c_str(), sizeof(ebuf)-1); ebuf[sizeof(ebuf)-1]=0;
                if(ImGui::InputText("Function", ebuf, sizeof(ebuf))) ev.functionName = ebuf;
                ImGui::SameLine();
                char pbuf[128]; strncpy(pbuf, ev.parameter.c_str(), sizeof(pbuf)-1); pbuf[sizeof(pbuf)-1]=0;
                if(ImGui::InputText("Param", pbuf, sizeof(pbuf))) ev.parameter = pbuf;
                ImGui::SameLine();
                if(ImGui::SmallButton("x")){ clip.events.erase(clip.events.begin() + (long long)e); markDirty(); --e; }
                ImGui::PopID();
            }

            if(ImGui::SmallButton("Remove clip")){ st.clips.erase(st.clips.begin() + (long long)i); markDirty(); --i; }
            ImGui::PopID();
        }
    }

    void AnimatorWindow::drawClipProperties(AnimatorClip& clip){
        (void)clip;
    }

    void AnimatorWindow::drawTransitionProperties(AnimatorTransition& tr){
        char buf[128]; strncpy(buf, tr.name.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        if(ImGui::InputText("Name", buf, sizeof(buf))) tr.name = buf;

        ImGui::Separator();
        ImGui::Checkbox("Has Exit Time", &tr.hasExitTime);
        if(tr.hasExitTime) ImGui::DragFloat("Exit Time", &tr.exitTime, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Duration", &tr.transitionDuration, 0.01f, 0.0f, 5.0f, "%.2f s");
        ImGui::DragFloat("Offset", &tr.offset, 0.01f, 0.0f, 1.0f);
        ImGui::Checkbox("Can Transition to Self", &tr.canTransitionToSelf);

        const char* interp[] = {"None","Current in Source","Next in Source","Current&Next in Source"};
        int is = (int)tr.interruptionSource;
        if(ImGui::Combo("Interruption Source", &is, interp, 4)) tr.interruptionSource = (AnimatorTransition::InterruptionSource)is;

        ImGui::Separator();
        ImGui::Text("Conditions");
        if(ImGui::Button("Add Condition", ImVec2(-1,0))){
            pushUndo();
            AnimatorTransition::Condition c;
            if(!controller->parameters.empty()) c.parameter = controller->parameters[0].name;
            tr.conditions.push_back(c);
            markDirty();
        }

        for(size_t i = 0; i < tr.conditions.size(); ++i){
            auto& c = tr.conditions[i];
            ImGui::PushID((int)i);
            ImGui::Separator();
            // parameter picker
            static int paramIdx = -1;
            paramIdx = -1;
            for(size_t pi = 0; pi < controller->parameters.size(); ++pi)
                if(controller->parameters[pi].name == c.parameter){ paramIdx = (int)pi; break; }
            const char* items[64]; int count = 0;
            for(const auto& p : controller->parameters){ items[count++] = p.name.c_str(); }
            if(count > 0 && ImGui::Combo("Parameter", &paramIdx, items, count)) c.parameter = controller->parameters[paramIdx].name;

            const char* modes[] = {">","<","==","!="};
            int mi = (int)c.mode;
            if(ImGui::Combo("Mode", &mi, modes, 4)) c.mode = (AnimatorTransition::Condition::Mode)mi;
            ImGui::SameLine();
            if(c.mode == AnimatorTransition::Condition::Mode::Equals || c.mode == AnimatorTransition::Condition::Mode::NotEqual){
                // bool
                ImGui::Checkbox("Value", &c.boolValue);
            }else{
                ImGui::DragFloat("Value", &c.value, 0.01f);
            }
            ImGui::SameLine();
            if(ImGui::SmallButton("x")){ tr.conditions.erase(tr.conditions.begin() + (long long)i); markDirty(); --i; }
            ImGui::PopID();
        }
    }

    void AnimatorWindow::drawParameterProperties(){
        // handled at top of drawDetailsPanel
    }

}
