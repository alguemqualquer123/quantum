// Registry.h - the component registry: every widget shown in the tree owns a
// WidgetInstance with its editable WidgetState.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

namespace uilab {

class WidgetRegistry {
public:
    WidgetInstance& Add(WidgetInstance instance);
    WidgetInstance* Find(std::string_view id) const;
    bool Remove(std::string_view id);
    bool Rename(std::string_view id, std::string newDisplayName, std::string& errorOut);

    const std::vector<std::unique_ptr<WidgetInstance>>& All() const { return items_; }
    size_t Count() const { return items_.size(); }

    int CountCategory(Category c) const;
    int CountLive() const;
    std::string MakeUniqueId(const std::string& base) const;

    // Ordered list of group names inside a category (registration order).
    std::vector<std::string> GroupsOf(Category c) const;

    void ClearState(); // restores "visited" bookkeeping only (used by self-test)

private:
    std::vector<std::unique_ptr<WidgetInstance>> items_;
    std::unordered_map<std::string, WidgetInstance*> byId_;
};

// Creates the default set of components demonstrated by the lab.
void BuildDefaultRegistry(WidgetRegistry& registry);

// Initial state must not be modified, used by the "Reset properties" command.
WidgetState DefaultStateOf(WidgetKind kind);

} // namespace uilab
