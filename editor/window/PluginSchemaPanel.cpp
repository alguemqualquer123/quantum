// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "window/PluginSchemaPanel.h"

#include "PluginManager.h"
#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    struct TextInput {
        std::vector<char> buffer;
        // The value this buffer was seeded from. Comparing the two is what tells a value the plugin
        // pushed apart from text the user is typing, so a schema rebuild cannot eat a keystroke.
        std::string synced;
    };

    // A text control needs a char array that outlives the frame: ImGui reads and writes it in place,
    // and the string inside UiValue moves every time the schema is rebuilt.
    std::map<std::string, TextInput>& textInputs() {
        static std::map<std::string, TextInput> inputs;
        return inputs;
    }

    // Keeps the array big enough for the current value plus room to type into it.
    void ensureRoom(TextInput& input, std::size_t minimum) {
        const std::size_t wanted = minimum + 64;
        if (input.buffer.size() >= wanted) return;
        input.buffer.resize(wanted, '\0');
    }

    bool colorFieldMatchesText(UiField& field, float* out);

    bool drawTextControl(const char* label, UiField& field, const std::string& key, bool multiline) {
        TextInput& input = textInputs()[key];
        const std::string current = field.value.toString();

        if (input.synced != current) {
            input.buffer.assign(current.begin(), current.end());
            input.buffer.push_back('\0');
            input.synced = current;
        }
        if (input.buffer.empty()) {
            input.buffer.assign(2, '\0');
        }
        ensureRoom(input, input.synced.size());

        bool edited = false;
        if (multiline) {
            const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 6.0f);
            edited = ImGui::InputTextMultiline(label, input.buffer.data(), input.buffer.size(), size);
        } else {
            edited = ImGui::InputText(label, input.buffer.data(), input.buffer.size());
        }

        if (edited) {
            // ImGui guarantees a terminated buffer; the length is whatever the user left in it.
            const std::size_t length = std::strlen(input.buffer.data());
            input.synced.assign(input.buffer.data(), length);
            field.value = UiValue::fromString(input.synced);
            field.changed = true;
        }
        return edited;
    }

    bool drawNumberControl(const char* label, UiField& field, bool slider) {
        if (field.type == UiControlType::IntSlider || field.type == UiControlType::IntInput) {
            int value = static_cast<int>(field.value.toInt());
            const int minimum = static_cast<int>(field.minValue);
            const int maximum = static_cast<int>(field.maxValue);
            const int step = field.stepValue >= 1.0 ? static_cast<int>(field.stepValue) : 1;

            const bool edited = slider
                ? ImGui::SliderInt(label, &value, minimum, maximum)
                : ImGui::InputInt(label, &value, step);
            if (edited) {
                field.value = UiValue::fromInt(value);
                field.changed = true;
            }
            return edited;
        }

        float value = static_cast<float>(field.value.toFloat());
        const float minimum = static_cast<float>(field.minValue);
        const float maximum = static_cast<float>(field.maxValue);

        const bool edited = slider
            ? ImGui::SliderFloat(label, &value, minimum, maximum)
            : ImGui::InputFloat(label, &value, 0.0f, 0.0f, "%g");
        if (edited) {
            field.value = UiValue::fromFloat(value);
            field.changed = true;
        }
        return edited;
    }

    bool drawEnumControl(const char* label, UiField& field) {
        if (field.options.empty()) return false;

        // ImGui takes one block of C strings separated by '\0' and terminated by an empty one.
        std::string entries;
        for (const std::string& option : field.options) {
            entries += option;
            entries += '\0';
        }
        entries += '\0';

        int index = static_cast<int>(field.value.toInt(field.optionIndex));
        if (index < 0 || index >= static_cast<int>(field.options.size())) index = 0;

        if (!ImGui::Combo(label, &index, entries.c_str())) return false;

        field.optionIndex = index;
        // The entry text, not the ordinal: the builder seeds the same spelling, so a plugin reads a
        // string before and after the user touches the combo, and a saved setting survives the
        // plugin inserting an option in the middle of its own list.
        field.value = UiValue::fromString(field.options[index]);
        field.changed = true;
        return true;
    }

    bool drawColorControl(const char* label, UiField& field) {
        float fromText[4];
        if (colorFieldMatchesText(field, fromText)) {
            // The plugin wrote a color the panel does not show yet.
            std::memcpy(field.color, fromText, sizeof(field.color));
        }

        if (!ImGui::ColorEdit4(label, field.color, ImGuiColorEditFlags_NoInputs)) return false;

        field.value = UiValue::fromString(editor::PluginSchemaPanel::colorToText(field.color));
        field.changed = true;
        return true;
    }

    bool colorFieldMatchesText(UiField& field, float* out) {
        if (!editor::PluginSchemaPanel::colorFromText(field.value.toString(), out)) return false;
        for (int i = 0; i < 4; i++) {
            if (std::fabs(out[i] - field.color[i]) > 0.0001f) return true;
        }
        return false;
    }

}

bool editor::PluginSchemaPanel::draw(const std::string& keyPrefix, UiSchema& schema, Touched& touched) {
    touched = Touched();

    for (UiGroup& group : schema.groups) {
        if (!group.title.empty()) {
            ImGui::SeparatorText(group.title.c_str());
        }

        for (UiField& field : group.fields) {
            ImGui::PushID(field.id.empty() ? field.label.c_str() : field.id.c_str());
            const std::string key = keyPrefix + "/" + field.id;
            const char* label = field.label.empty() ? " " : field.label.c_str();
            bool edited = false;

            if (!field.enabled) ImGui::BeginDisabled();
            switch (field.type) {
                case UiControlType::Label:
                    ImGui::TextWrapped("%s", field.label.c_str());
                    break;
                case UiControlType::Separator:
                    ImGui::Separator();
                    break;
                case UiControlType::Bool: {
                    bool value = field.value.toBool();
                    if (ImGui::Checkbox(label, &value)) {
                        field.value = UiValue::fromBool(value);
                        field.changed = true;
                        edited = true;
                    }
                    break;
                }
                case UiControlType::IntSlider:
                case UiControlType::FloatSlider:
                    edited = drawNumberControl(label, field, true);
                    break;
                case UiControlType::IntInput:
                case UiControlType::FloatInput:
                    edited = drawNumberControl(label, field, false);
                    break;
                case UiControlType::Enum:
                    edited = drawEnumControl(label, field);
                    break;
                case UiControlType::Color:
                    edited = drawColorControl(label, field);
                    break;
                case UiControlType::MultilineText:
                    edited = drawTextControl(label, field, key, true);
                    break;
                case UiControlType::Button:
                    // A press is not an edit: it goes to `pressed` only, so it never reaches the
                    // settings file and never fires onSettingChanged for a field that has no value.
                    if (ImGui::Button(label) && !field.id.empty()) {
                        touched.pressed.push_back(field.id);
                    }
                    break;
            }

            if (!field.enabled) ImGui::EndDisabled();

            if (!field.tooltip.empty() && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(field.tooltip.c_str());
                ImGui::EndTooltip();
            }

            if (edited) touched.edited.push_back(field.id);
            ImGui::PopID();
        }
    }

    return touched.any();
}

bool editor::PluginSchemaPanel::applyEdits(PluginRecord& record, const UiSchema& schema,
                                           const std::vector<std::string>& edited, std::string& error) {
    error.clear();

    for (const std::string& id : edited) {
        if (id.empty()) continue;
        const UiField* field = schema.findField(id);
        if (!field) continue;

        record.settings.set(id, field->value);
        // Only an instance that is actually up hears about it; a disabled plugin keeps the value in
        // its file and reads it on the next activate().
        if (record.instance) record.instance->onSettingChanged(id, field->value);
    }

    // One write for the whole frame: a slider drag edits every frame it moves.
    if (!edited.empty() && !record.settings.save()) {
        error = record.settings.lastError().empty() ? "cannot write " + record.settings.path()
                                                   : record.settings.lastError();
        return false;
    }
    return true;
}

std::string editor::PluginSchemaPanel::colorToText(const float components[4]) {
    return quantum::plugin::uiColorToText(components);
}

bool editor::PluginSchemaPanel::colorFromText(const std::string& text, float components[4]) {
    return quantum::plugin::uiColorFromText(text, components);
}
