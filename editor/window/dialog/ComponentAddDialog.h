// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include "imgui.h"
#include "Catalog.h"
#include "util/ReadyMadeComponents.h"

namespace quantum {
namespace editor {

struct ComponentEntry {
    ComponentType type;
    const char* icon;
    const char* name;
    const char* description;
    ImVec4 color;
};

struct ComponentCategory {
    const char* name;
    const char* icon;
    ImVec4 color;
    std::vector<ComponentEntry> entries;
};

class ComponentAddDialog {
private:
    bool m_isOpen = false;
    char m_searchBuffer[128] = "";
    int m_hoveredIndex = -1;
    bool m_justOpened = false;
    
    std::vector<ComponentCategory> m_categories;
    std::function<void(ComponentType)> m_onAdd;
    std::function<void()> m_onCancel;
    std::function<bool(ComponentType)> m_canAddComponent;

    std::vector<ReadyMadeCategory> m_readyMade;
    std::function<void(const std::string&)> m_onAddReadyMade;
    std::function<bool(const std::string&)> m_canAddReadyMade;

    void initializeCategories();

public:
    ComponentAddDialog();
    ~ComponentAddDialog() = default;

    void open(
        std::function<void(ComponentType)> onAdd,
        std::function<bool(ComponentType)> canAddComponent,
        std::function<void()> onCancel = nullptr
    );

    // Set before open(): the library is read when the dialog opens, so a component installed
    // since the project was loaded shows up without a restart.
    void setReadyMadeComponents(
        std::vector<ReadyMadeCategory> categories,
        std::function<void(const std::string&)> onAdd,
        std::function<bool(const std::string&)> canAdd
    );

    void show();
    bool isOpen() const { return m_isOpen; }
    void close() { m_isOpen = false; }
};

} // namespace editor
} // namespace quantum