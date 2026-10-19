#include "canvas/CanvasScene.h"

#include <algorithm>
#include <cmath>

namespace uilab {

const char* CanvasNodeTypeName(CanvasNodeType t) {
    switch (t) {
    case CanvasNodeType::Panel:         return "Panel";
    case CanvasNodeType::Button:        return "Button";
    case CanvasNodeType::Label:         return "Label";
    case CanvasNodeType::ProgressBar:   return "ProgressBar";
    case CanvasNodeType::Image:         return "Image";
    case CanvasNodeType::InventorySlot: return "InventorySlot";
    case CanvasNodeType::HealthBar:     return "HealthBar";
    case CanvasNodeType::ScrollView:    return "ScrollView";
    default:                            return "?";
    }
}

const char* CanvasNodeTypeIcon(CanvasNodeType t) {
    switch (t) {
    case CanvasNodeType::Panel:         return "[ ]";
    case CanvasNodeType::Button:        return "(B)";
    case CanvasNodeType::Label:         return "T";
    case CanvasNodeType::ProgressBar:   return "===";
    case CanvasNodeType::Image:         return "[#]";
    case CanvasNodeType::InventorySlot: return "[S]";
    case CanvasNodeType::HealthBar:     return "<3";
    case CanvasNodeType::ScrollView:    return "[^]";
    default:                            return "?";
    }
}

const char* LayoutGroupModeName(LayoutGroupMode m) {
    switch (m) {
    case LayoutGroupMode::None:       return "None";
    case LayoutGroupMode::Horizontal: return "Horizontal";
    case LayoutGroupMode::Vertical:   return "Vertical";
    case LayoutGroupMode::Grid:       return "Grid";
    default:                          return "?";
    }
}

CanvasScene::CanvasScene() { BuildDefaultScene(); }

void CanvasScene::Clear() {
    nodes_.clear();
    rootId_ = -1;
    nextId_ = 1;
}

int CanvasScene::AddNode(CanvasNode node) {
    node.id = nextId_++;
    if (node.parent < 0 && rootId_ >= 0)
        node.parent = rootId_;
    const int parent = node.parent;
    nodes_.push_back(std::move(node));
    const int index = static_cast<int>(nodes_.size()) - 1;
    if (parent >= 0) {
        CanvasNode* p = Find(parent);
        if (p)
            p->children.push_back(nodes_[index].id);
        else
            nodes_[index].parent = -1;
    }
    if (rootId_ < 0)
        rootId_ = nodes_[index].id;
    return nodes_[index].id;
}

int CanvasScene::IndexOf(int id) const {
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

CanvasNode* CanvasScene::Find(int id) {
    const int idx = IndexOf(id);
    return idx < 0 ? nullptr : &nodes_[static_cast<size_t>(idx)];
}

const CanvasNode* CanvasScene::Find(int id) const {
    const int idx = IndexOf(id);
    return idx < 0 ? nullptr : &nodes_[static_cast<size_t>(idx)];
}

void CanvasScene::RemoveRecursive(int id) {
    CanvasNode* n = Find(id);
    if (!n)
        return;
    std::vector<int> children = n->children;
    // Detach from parent first.
    if (n->parent >= 0) {
        CanvasNode* p = Find(n->parent);
        if (p) {
            p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
        }
    }
    for (int c : children)
        RemoveRecursive(c);
    const int idx = IndexOf(id);
    if (idx >= 0)
        nodes_.erase(nodes_.begin() + idx);
    if (rootId_ == id)
        rootId_ = nodes_.empty() ? -1 : nodes_.front().id;
}

bool CanvasScene::RemoveNode(int id) {
    if (id == rootId_ || id < 0)
        return false;
    if (!Find(id))
        return false;
    RemoveRecursive(id);
    return true;
}

int CanvasScene::CloneRecursive(int srcId, int newParent, bool offset) {
    const CanvasNode* src = Find(srcId);
    if (!src)
        return -1;
    CanvasNode copy = *src;
    copy.parent = newParent;
    copy.children.clear();
    if (offset) {
        copy.anchoredPosition.x += 16.0f;
        copy.anchoredPosition.y += 16.0f;
        copy.zOrder += 1;
    }
    const std::vector<int> childIds = src->children;
    const int newId = AddNode(std::move(copy));
    for (int c : childIds)
        CloneRecursive(c, newId, false);
    return newId;
}

int CanvasScene::DuplicateNode(int id) {
    CanvasNode* src = Find(id);
    if (!src || id == rootId_)
        return -1;
    const int newId = CloneRecursive(id, src->parent, true);
    if (CanvasNode* created = Find(newId))
        created->name += " Copy";
    return newId;
}

bool CanvasScene::SetParent(int childId, int parentId) {
    if (childId == parentId || childId == rootId_)
        return false;
    CanvasNode* child = Find(childId);
    CanvasNode* parent = Find(parentId);
    if (!child || !parent)
        return false;
    // Reject cycles: parent must not be a descendant of child.
    for (int p = parentId; p >= 0;) {
        if (p == childId)
            return false;
        const CanvasNode* node = Find(p);
        p = node ? node->parent : -1;
    }
    if (child->parent >= 0) {
        CanvasNode* old = Find(child->parent);
        if (old)
            old->children.erase(std::remove(old->children.begin(), old->children.end(), childId),
                                old->children.end());
    }
    child->parent = parentId;
    parent->children.push_back(childId);
    return true;
}

void CanvasScene::SortChildrenByZ(int id) {
    CanvasNode* n = Find(id);
    if (!n)
        return;
    std::stable_sort(n->children.begin(), n->children.end(), [this](int a, int b) {
        const CanvasNode* na = Find(a);
        const CanvasNode* nb = Find(b);
        if (!na || !nb)
            return false;
        return na->zOrder < nb->zOrder;
    });
}

void CanvasScene::MoveZOrder(int id, int delta) {
    CanvasNode* n = Find(id);
    if (!n)
        return;
    n->zOrder = std::max(0, n->zOrder + delta);
    if (n->parent >= 0)
        SortChildrenByZ(n->parent);
}

SceneRect CanvasScene::ComputeAnchoredRect(const CanvasNode& n, const SceneRect& parentRect) {
    const ImVec2 pSize = parentRect.Size();
    const float ax0 = parentRect.min.x + n.anchorMin.x * pSize.x;
    const float ay0 = parentRect.min.y + n.anchorMin.y * pSize.y;
    const float ax1 = parentRect.min.x + n.anchorMax.x * pSize.x;
    const float ay1 = parentRect.min.y + n.anchorMax.y * pSize.y;

    SceneRect anchor;
    anchor.min = ImVec2(std::min(ax0, ax1), std::min(ay0, ay1));
    anchor.max = ImVec2(std::max(ax0, ax1), std::max(ay0, ay1));

    const ImVec2 anchorSize = anchor.Size();
    const bool stretchX = anchorSize.x > 1.0f;
    const bool stretchY = anchorSize.y > 1.0f;

    ImVec2 size;
    size.x = std::max(1.0f, stretchX ? anchorSize.x + n.sizeDelta.x : n.sizeDelta.x);
    size.y = std::max(1.0f, stretchY ? anchorSize.y + n.sizeDelta.y : n.sizeDelta.y);

    SceneRect r;
    if (stretchX) {
        // Stretched axis: sizeDelta insets the anchor rectangle, the pivot has
        // no effect on the horizontal placement (uGUI behaviour).
        r.min.x = anchor.min.x + n.anchoredPosition.x;
        r.max.x = r.min.x + size.x;
    } else {
        const float anchorX = (anchor.min.x + anchor.max.x) * 0.5f;
        r.min.x = anchorX + n.anchoredPosition.x - n.pivot.x * size.x;
        r.max.x = r.min.x + size.x;
    }
    if (stretchY) {
        r.min.y = anchor.min.y + n.anchoredPosition.y;
        r.max.y = r.min.y + size.y;
    } else {
        const float anchorY = (anchor.min.y + anchor.max.y) * 0.5f;
        r.min.y = anchorY + n.anchoredPosition.y - n.pivot.y * size.y;
        r.max.y = r.min.y + size.y;
    }
    return r;
}

ImVec2 CanvasScene::AnchorPoint(const CanvasNode& n, const SceneRect& parentRect, bool useMax,
                                const SceneRect& anchoredRect) {
    (void)anchoredRect;
    const ImVec2 pSize = parentRect.Size();
    const ImVec2 a = useMax ? n.anchorMax : n.anchorMin;
    return ImVec2(parentRect.min.x + a.x * pSize.x, parentRect.min.y + a.y * pSize.y);
}

void CanvasScene::ApplyLayoutToChild(CanvasNode& child, const SceneRect& parentRect, const ImVec2& slotMin,
                                     const ImVec2& slotSize, bool controlCrossAxis) {
    child.layoutDriven = true;
    child.anchorMin = ImVec2(0.0f, 0.0f);
    child.anchorMax = ImVec2(0.0f, 0.0f);
    child.pivot = ImVec2(0.0f, 0.0f);
    child.anchoredPosition = ImVec2(slotMin.x - parentRect.min.x, slotMin.y - parentRect.min.y);
    if (controlCrossAxis)
        child.sizeDelta = slotSize;
    else
        child.sizeDelta = ImVec2(slotSize.x, child.sizeDelta.y);
    child.computed = ComputeAnchoredRect(child, parentRect);
}

void CanvasScene::LayoutChildren(CanvasNode& parent) {
    SortChildrenByZ(parent.id);
    const SceneRect r = parent.computed;
    const float px = parent.paddingX;
    const float py = parent.paddingY;
    const ImVec2 innerMin = ImVec2(r.min.x + px, r.min.y + py);
    const ImVec2 innerSize = ImVec2(std::max(1.0f, r.Width() - px * 2.0f),
                                    std::max(1.0f, r.Height() - py * 2.0f));

    const int count = static_cast<int>(parent.children.size());
    if (count == 0) {
        if (parent.contentFitterH)
            parent.computed.max.x = parent.computed.min.x + px * 2.0f;
        if (parent.contentFitterV)
            parent.computed.max.y = parent.computed.min.y + py * 2.0f;
        return;
    }

    if (parent.layoutMode == LayoutGroupMode::Horizontal || parent.layoutMode == LayoutGroupMode::Vertical) {
        const bool horizontal = parent.layoutMode == LayoutGroupMode::Horizontal;
        const float totalSpacing = parent.spacing * static_cast<float>(count - 1);
        const float available = (horizontal ? innerSize.x : innerSize.y) - totalSpacing;
        const float slot = parent.forceExpand ? std::max(1.0f, available / static_cast<float>(count)) : 0.0f;
        float cursor = (horizontal ? innerMin.x : innerMin.y);
        float maxCross = 0.0f;
        for (int childId : parent.children) {
            CanvasNode* child = Find(childId);
            if (!child)
                continue;
            const float main = parent.forceExpand ? slot : (horizontal ? child->sizeDelta.x : child->sizeDelta.y);
            const float crossMax = (horizontal ? innerSize.y : innerSize.x);
            const float cross = parent.forceExpand ? crossMax : (horizontal ? child->sizeDelta.y : child->sizeDelta.x);
            float offset = 0.0f;
            if (parent.align == 1)
                offset = (crossMax - cross) * 0.5f;
            else if (parent.align == 2)
                offset = crossMax - cross;

            ImVec2 slotMin;
            ImVec2 slotSize;
            if (horizontal) {
                slotMin = ImVec2(cursor, innerMin.y + offset);
                slotSize = ImVec2(main, cross);
            } else {
                slotMin = ImVec2(innerMin.x + offset, cursor);
                slotSize = ImVec2(cross, main);
            }
            ApplyLayoutToChild(*child, r, slotMin, slotSize, true);
            cursor += main + parent.spacing;
            maxCross = std::max(maxCross, cross);
        }
        if (parent.contentFitterH)
            parent.computed.max.x = parent.computed.min.x + px * 2.0f + (horizontal ? (cursor - innerMin.x - parent.spacing) : maxCross);
        if (parent.contentFitterV)
            parent.computed.max.y = parent.computed.min.y + py * 2.0f + (horizontal ? maxCross : (cursor - innerMin.y - parent.spacing));
    } else if (parent.layoutMode == LayoutGroupMode::Grid) {
        const int columns = std::max(1, static_cast<int>(std::floor(innerSize.x / 64.0f)));
        const float cellW = (innerSize.x - parent.spacing * (columns - 1)) / static_cast<float>(columns);
        float cellH = 0.0f;
        for (int childId : parent.children) {
            const CanvasNode* child = Find(childId);
            if (child)
                cellH = std::max(cellH, child->sizeDelta.y);
        }
        int index = 0;
        for (int childId : parent.children) {
            CanvasNode* child = Find(childId);
            if (!child)
                continue;
            const int col = index % columns;
            const int row = index / columns;
            const ImVec2 slotMin(innerMin.x + col * (cellW + parent.spacing),
                                 innerMin.y + row * (cellH + parent.spacing));
            ApplyLayoutToChild(*child, r, slotMin, ImVec2(cellW, cellH), true);
            ++index;
        }
    }
}

void CanvasScene::ComputeNode(int id, const SceneRect& parentRect, bool useAnchorMath) {
    CanvasNode* n = Find(id);
    if (!n)
        return;
    if (useAnchorMath)
        n->computed = ComputeAnchoredRect(*n, parentRect);
    n->anchoredRect = n->computed;
    if (n->layoutMode != LayoutGroupMode::None) {
        LayoutChildren(*n);
    }
    const std::vector<int> children = n->children;
    for (int c : children)
        ComputeNode(c, n->computed, n->layoutMode == LayoutGroupMode::None);
}

void CanvasScene::ComputeLayout(const ImVec2& canvasSize) {
    if (rootId_ < 0)
        return;
    CanvasNode* root = Find(rootId_);
    if (!root)
        return;
    root->computed.min = ImVec2(0.0f, 0.0f);
    root->computed.max = canvasSize;
    root->layoutDriven = false;
    ComputeNode(rootId_, root->computed, false);
}

int CanvasScene::HitTest(const ImVec2& point) const {
    // Walks the tree back-to-front; children are drawn after their parent, so
    // they win the hit test. Fully transparent nodes are ignored.
    int hit = -1;
    if (rootId_ < 0)
        return -1;

    struct Walker {
        const CanvasScene* scene;
        ImVec2 p;
        int* out;
        void Visit(int id) const {
            const CanvasNode* n = scene->Find(id);
            if (!n)
                return;
            const bool visible = n->opacity > 0.05f && n->color.w > 0.05f;
            if (visible && n->computed.Contains(p))
                *out = id;
            for (int c : n->children)
                Visit(c);
        }
    } walker{ this, point, &hit };
    walker.Visit(rootId_);
    return hit;
}

std::vector<int> CanvasScene::DrawOrder() const {
    std::vector<int> out;
    struct Walker {
        const CanvasScene* scene;
        std::vector<int>* out;
        void Visit(int id) const {
            const CanvasNode* n = scene->Find(id);
            if (!n)
                return;
            out->push_back(id);
            std::vector<int> kids = n->children;
            std::stable_sort(kids.begin(), kids.end(), [this](int a, int b) {
                const CanvasNode* na = scene->Find(a);
                const CanvasNode* nb = scene->Find(b);
                if (!na || !nb)
                    return false;
                return na->zOrder < nb->zOrder;
            });
            for (int c : kids)
                Visit(c);
        }
    } walker{ this, &out };
    if (rootId_ >= 0)
        walker.Visit(rootId_);
    return out;
}

int CanvasScene::CountOfType(CanvasNodeType t) const {
    int n = 0;
    for (const CanvasNode& node : nodes_)
        if (node.type == t)
            ++n;
    return n;
}

void CanvasScene::BuildDefaultScene() {
    Clear();

    CanvasNode root;
    root.name = "Canvas";
    root.type = CanvasNodeType::Panel;
    root.parent = -1;
    root.anchorMin = ImVec2(0.0f, 0.0f);
    root.anchorMax = ImVec2(0.0f, 0.0f);
    root.pivot = ImVec2(0.0f, 0.0f);
    root.anchoredPosition = ImVec2(0.0f, 0.0f);
    root.sizeDelta = ImVec2(760.0f, 440.0f);
    root.color = ImVec4(0.075f, 0.085f, 0.105f, 1.00f);
    root.zOrder = 0;
    const int canvasId = AddNode(root);

    auto child = [&](const char* name, CanvasNodeType type, int parent, ImVec2 anchor, ImVec2 pivot,
                     ImVec2 pos, ImVec2 size, ImVec4 color) {
        CanvasNode n;
        n.name = name;
        n.type = type;
        n.parent = parent;
        n.anchorMin = anchor;
        n.anchorMax = anchor;
        n.pivot = pivot;
        n.anchoredPosition = pos;
        n.sizeDelta = size;
        n.color = color;
        return n;
    };

    // Top bar with score + wave
    {
        CanvasNode n = child("TopBar", CanvasNodeType::Panel, canvasId, ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f),
                             ImVec2(0.0f, 0.0f), ImVec2(760.0f, 44.0f), ImVec4(0.11f, 0.13f, 0.17f, 0.95f));
        n.anchorMax = ImVec2(1.0f, 0.0f);
        n.sizeDelta = ImVec2(0.0f, 44.0f);
        const int barId = AddNode(n);
        CanvasNode label = child("ScoreLabel", CanvasNodeType::Label, barId, ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.5f),
                                 ImVec2(16.0f, 0.0f), ImVec2(260.0f, 24.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        label.text = "SCORE 1250   WAVE 3";
        label.textColor = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
        AddNode(label);

        CanvasNode rightBtn = child("OptionsButton", CanvasNodeType::Button, barId, ImVec2(1.0f, 0.5f),
                                    ImVec2(1.0f, 0.5f), ImVec2(-16.0f, 0.0f), ImVec2(120.0f, 28.0f),
                                    ImVec4(0.24f, 0.28f, 0.36f, 1.0f));
        rightBtn.text = "Options";
        AddNode(rightBtn);
    }

    // Health + energy bars (bottom-left container, vertical layout group)
    {
        CanvasNode container = child("StatusContainer", CanvasNodeType::Panel, canvasId, ImVec2(0.0f, 1.0f),
                                     ImVec2(0.0f, 1.0f), ImVec2(18.0f, -96.0f), ImVec2(300.0f, 84.0f),
                                     ImVec4(0.10f, 0.11f, 0.14f, 0.85f));
        container.layoutMode = LayoutGroupMode::Vertical;
        container.spacing = 8.0f;
        container.paddingX = 10.0f;
        container.paddingY = 10.0f;
        container.forceExpand = true;
        container.contentFitterH = true;
        container.contentFitterV = true;
        const int containerId = AddNode(container);

        CanvasNode health = child("HealthBar", CanvasNodeType::HealthBar, containerId, ImVec2(0.0f, 0.0f),
                                  ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), ImVec2(280.0f, 18.0f),
                                  ImVec4(0.28f, 0.78f, 0.40f, 1.0f));
        health.text = "Health";
        health.value = 0.72f;
        AddNode(health);

        CanvasNode energy = child("EnergyBar", CanvasNodeType::ProgressBar, containerId, ImVec2(0.0f, 0.0f),
                                  ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), ImVec2(280.0f, 14.0f),
                                  ImVec4(0.32f, 0.62f, 0.96f, 1.0f));
        energy.text = "Energy";
        energy.value = 0.45f;
        AddNode(energy);
    }

    // Inventory bar (horizontal layout group at the bottom)
    {
        CanvasNode inv = child("InventoryBar", CanvasNodeType::Panel, canvasId, ImVec2(0.5f, 1.0f),
                               ImVec2(0.5f, 1.0f), ImVec2(0.0f, -18.0f), ImVec2(360.0f, 64.0f),
                               ImVec4(0.11f, 0.12f, 0.16f, 0.88f));
        inv.layoutMode = LayoutGroupMode::Horizontal;
        inv.spacing = 8.0f;
        inv.paddingX = 8.0f;
        inv.paddingY = 8.0f;
        inv.forceExpand = true;
        inv.contentFitterH = true;
        inv.contentFitterV = true;
        const int invId = AddNode(inv);

        for (int i = 0; i < 4; ++i) {
            CanvasNode slot = child("Slot", CanvasNodeType::InventorySlot, invId, ImVec2(0.0f, 0.0f),
                                    ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), ImVec2(72.0f, 48.0f),
                                    ImVec4(0.18f, 0.20f, 0.26f, 1.0f));
            slot.text = "Slot " + std::to_string(i + 1);
            slot.iconIndex = i;
            AddNode(slot);
        }
    }

    // Reward card in the middle (free transform, rotation/scale/opacity demo)
    {
        CanvasNode card = child("RewardCard", CanvasNodeType::Panel, canvasId, ImVec2(0.5f, 0.5f),
                                ImVec2(0.5f, 0.5f), ImVec2(0.0f, -24.0f), ImVec2(240.0f, 132.0f),
                                ImVec4(0.20f, 0.24f, 0.34f, 0.96f));
        card.text = "Mission Complete";
        card.zOrder = 10;
        const int cardId = AddNode(card);

        CanvasNode title = child("Title", CanvasNodeType::Label, cardId, ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f),
                                 ImVec2(16.0f, 16.0f), ImVec2(200.0f, 26.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        title.text = "Mission Complete";
        AddNode(title);

        CanvasNode reward = child("RewardBar", CanvasNodeType::ProgressBar, cardId, ImVec2(0.0f, 0.0f),
                                  ImVec2(0.0f, 0.0f), ImVec2(16.0f, 56.0f), ImVec2(208.0f, 16.0f),
                                  ImVec4(0.95f, 0.62f, 0.24f, 1.0f));
        reward.text = "Reward progress";
        reward.value = 0.68f;
        AddNode(reward);

        CanvasNode confirm = child("ConfirmButton", CanvasNodeType::Button, cardId, ImVec2(0.0f, 1.0f),
                                   ImVec2(0.0f, 1.0f), ImVec2(16.0f, -16.0f), ImVec2(140.0f, 30.0f),
                                   ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
        confirm.text = "Claim Reward";
        AddNode(confirm);
    }

    // Minimap-ish image in the top-right corner (anchored)
    {
        CanvasNode img = child("Minimap", CanvasNodeType::Image, canvasId, ImVec2(1.0f, 0.0f),
                               ImVec2(1.0f, 0.0f), ImVec2(-16.0f, 60.0f), ImVec2(140.0f, 100.0f),
                               ImVec4(0.16f, 0.22f, 0.20f, 1.0f));
        img.text = "Minimap";
        img.iconIndex = 1;
        AddNode(img);
    }

    SortChildrenByZ(canvasId);
}

} // namespace uilab
