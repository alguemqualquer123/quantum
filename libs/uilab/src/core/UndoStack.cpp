#include "core/UndoStack.h"

#include "core/Registry.h"

namespace uilab {

void UndoStack::Push(std::string instanceId, std::string widgetName, std::string label,
                     const WidgetState& before, const WidgetState& after) {
    if (undo_.size() >= limit_)
        undo_.erase(undo_.begin());
    UndoRecord r;
    r.instanceId = std::move(instanceId);
    r.widgetName = std::move(widgetName);
    r.label = std::move(label);
    r.before = before;
    r.after = after;
    undo_.push_back(std::move(r));
    redo_.clear();
}

void UndoStack::Clear() {
    undo_.clear();
    redo_.clear();
}

bool UndoStack::Undo(WidgetRegistry& registry, UndoRecord& appliedOut) {
    if (undo_.empty())
        return false;
    UndoRecord r = undo_.back();
    WidgetInstance* inst = registry.Find(r.instanceId);
    if (!inst)
        return false;
    inst->state = r.before;
    appliedOut = r;
    undo_.pop_back();
    if (redo_.size() >= limit_)
        redo_.erase(redo_.begin());
    redo_.push_back(std::move(r));
    return true;
}

bool UndoStack::Redo(WidgetRegistry& registry, UndoRecord& appliedOut) {
    if (redo_.empty())
        return false;
    UndoRecord r = redo_.back();
    WidgetInstance* inst = registry.Find(r.instanceId);
    if (!inst)
        return false;
    inst->state = r.after;
    appliedOut = r;
    redo_.pop_back();
    if (undo_.size() >= limit_)
        undo_.erase(undo_.begin());
    undo_.push_back(std::move(r));
    return true;
}

} // namespace uilab
