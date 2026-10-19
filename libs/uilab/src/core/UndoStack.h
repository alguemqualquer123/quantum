// UndoStack.h - property edit history used by the Inspector and the Edit menu.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Types.h"

namespace uilab {

class WidgetRegistry;

struct UndoRecord {
    std::string instanceId;
    std::string widgetName;
    std::string label;      // what changed, eg. "Text" or "Multiple properties"
    WidgetState before;
    WidgetState after;
};

class UndoStack {
public:
    void Push(std::string instanceId, std::string widgetName, std::string label,
              const WidgetState& before, const WidgetState& after);
    void Clear();

    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    size_t UndoCount() const { return undo_.size(); }
    size_t RedoCount() const { return redo_.size(); }
    std::string UndoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string RedoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }

    // Applies the last change in the given direction. Returns false when there
    // is nothing to undo/redo or the target instance no longer exists.
    bool Undo(WidgetRegistry& registry, UndoRecord& appliedOut);
    bool Redo(WidgetRegistry& registry, UndoRecord& appliedOut);

    void SetLimit(size_t limit) { limit_ = limit > 0 ? limit : 1; }
    size_t Limit() const { return limit_; }

private:
    std::vector<UndoRecord> undo_;
    std::vector<UndoRecord> redo_;
    size_t limit_ = 200;
};

} // namespace uilab
