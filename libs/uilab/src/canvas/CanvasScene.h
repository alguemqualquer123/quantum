// CanvasScene.h - a small "Canvas / RectTransform" scene used by the UMG and
// uGUI demo pages (and by the Hierarchy panel).
//
// The math follows the uGUI conventions the lab demonstrates: anchor min/max,
// pivot, anchored position and size delta, plus simple layout groups and a
// conceptual content size fitter.
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace uilab {

struct SceneRect {
    ImVec2 min = ImVec2(0, 0);
    ImVec2 max = ImVec2(0, 0);

    ImVec2 Size() const { return ImVec2(max.x - min.x, max.y - min.y); }
    ImVec2 Center() const { return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f); }
    float Width() const { return max.x - min.x; }
    float Height() const { return max.y - min.y; }
    bool Contains(const ImVec2& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
};

enum class CanvasNodeType : int {
    Panel = 0,   // plain rectangle (background / container visual)
    Button,      // clickable, reports clicks
    Label,       // text
    ProgressBar, // value driven bar
    Image,       // coloured sprite / icon
    InventorySlot,
    HealthBar,
    ScrollView,  // clipped scrollable area
    Count
};

const char* CanvasNodeTypeName(CanvasNodeType t);
const char* CanvasNodeTypeIcon(CanvasNodeType t);

enum class LayoutGroupMode : int { None = 0, Horizontal, Vertical, Grid, Count };
const char* LayoutGroupModeName(LayoutGroupMode m);

struct CanvasNode {
    int id = -1;
    std::string name = "Widget";
    CanvasNodeType type = CanvasNodeType::Panel;
    int parent = -1;
    std::vector<int> children;

    // RectTransform (uGUI style)
    ImVec2 anchorMin = ImVec2(0.0f, 0.0f);
    ImVec2 anchorMax = ImVec2(0.0f, 0.0f);
    ImVec2 pivot = ImVec2(0.5f, 0.5f);
    ImVec2 anchoredPosition = ImVec2(24.0f, 24.0f);
    ImVec2 sizeDelta = ImVec2(180.0f, 40.0f);
    float rotation = 0.0f;               // degrees, clockwise
    ImVec2 scale = ImVec2(1.0f, 1.0f);
    float opacity = 1.0f;
    int zOrder = 0;

    // appearance
    ImVec4 color = ImVec4(0.20f, 0.24f, 0.32f, 1.00f);
    ImVec4 textColor = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    std::string text;
    float value = 0.5f;          // progress bars / health bars
    int iconIndex = 0;

    // layout group (auto positioning of children when != None)
    LayoutGroupMode layoutMode = LayoutGroupMode::None;
    float spacing = 10.0f;
    float paddingX = 10.0f;
    float paddingY = 10.0f;
    int align = 0;               // 0 start, 1 center, 2 end
    bool forceExpand = false;    // children expand to fill the cross axis
    bool contentFitterH = false; // conceptual content size fitter
    bool contentFitterV = false;

    // computed every frame by ComputeLayout()
    SceneRect computed;
    SceneRect anchoredRect;      // the anchor rectangle inside the parent
    bool layoutDriven = false;   // rect was written by a parent layout group

    bool stretchX = false;
    bool stretchY = false;
};

class CanvasScene {
public:
    CanvasScene();

    void Clear();
    void BuildDefaultScene();     // default UMG/uGUI sample content

    int AddNode(CanvasNode node);
    bool RemoveNode(int id);
    int DuplicateNode(int id);
    bool SetParent(int childId, int parentId);
    void MoveZOrder(int id, int delta);
    void SortChildrenByZ(int id);

    CanvasNode* Find(int id);
    const CanvasNode* Find(int id) const;
    int IndexOf(int id) const;

    std::vector<CanvasNode>& Nodes() { return nodes_; }
    const std::vector<CanvasNode>& Nodes() const { return nodes_; }
    int RootId() const { return rootId_; }

    // Recomputes every rect for a canvas of the given size (already zoomed).
    void ComputeLayout(const ImVec2& canvasSize);

    // Top-most node under 'point' (canvas space). Returns -1 when nothing hits.
    int HitTest(const ImVec2& point) const;

    // Children ordered back-to-front (drawing order).
    std::vector<int> DrawOrder() const;

    int CountOfType(CanvasNodeType t) const;
    int Count() const { return static_cast<int>(nodes_.size()); }

    // Manually position a node inside a parent rect (used by layout groups).
    void ApplyLayoutToChild(CanvasNode& child, const SceneRect& parentRect, const ImVec2& slotMin,
                            const ImVec2& slotSize, bool controlCrossAxis);

    static ImVec2 AnchorPoint(const CanvasNode& n, const SceneRect& parentRect, bool useMax,
                              const SceneRect& anchoredRect);
    static SceneRect ComputeAnchoredRect(const CanvasNode& n, const SceneRect& parentRect);

private:
    std::vector<CanvasNode> nodes_;
    int rootId_ = -1;
    int nextId_ = 1;

    void ComputeNode(int id, const SceneRect& parentRect, bool useAnchorMath);
    void LayoutChildren(CanvasNode& parent);
    void RemoveRecursive(int id);
    int CloneRecursive(int srcId, int newParent, bool offset);
};

} // namespace uilab
