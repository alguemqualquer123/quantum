// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "PluginUiSchema.h"

#include <string>
#include <vector>

namespace quantum {
namespace plugin {
    struct PluginRecord;
}
}

namespace quantum::editor {

    // A plugin cannot draw: ImGui is linked into the editor executables and the engine does not link
    // it at all, so IPlugin::buildUiSchema describes controls and this renders them with the editor's
    // own context. A new UiControlType needs one case in draw() and nothing else.
    class PluginSchemaPanel {
    public:
        // What the user did to a panel during one frame.
        struct Touched {
            std::vector<std::string> edited;   // field ids whose value changed
            std::vector<std::string> pressed;  // button ids pressed

            bool any() const { return !edited.empty() || !pressed.empty(); }
        };

        // Draws groups and fields, writing edits into `schema` (field.value and field.changed).
        // `keyPrefix` namespaces the text buffers, so two plugins with the same field id do not share
        // one input box. Returns true when anything was touched.
        static bool draw(const std::string& keyPrefix, quantum::plugin::UiSchema& schema, Touched& touched);

        // Writes the edited fields into the plugin's settings and calls onSettingChanged for each,
        // then saves the file once. Returns false with the reason when the write fails.
        static bool applyEdits(quantum::plugin::PluginRecord& record,
                               const quantum::plugin::UiSchema& schema,
                               const std::vector<std::string>& edited,
                               std::string& error);

        // A color control has no UiValue kind, so it travels as "#rrggbbaa" with components in
        // [0,1]. Both halves are here because a plugin writing a default and the panel reading it
        // have to agree byte for byte.
        static std::string colorToText(const float components[4]);
        static bool colorFromText(const std::string& text, float components[4]);
    };

}
