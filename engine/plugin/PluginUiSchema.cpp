// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginUiSchema.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace quantum;
using namespace quantum::plugin;

UiValue UiValue::fromBool(bool value) {
    UiValue out;
    out.kind = Kind::Bool;
    out.boolValue = value;
    return out;
}

UiValue UiValue::fromInt(long long value) {
    UiValue out;
    out.kind = Kind::Int;
    out.intValue = value;
    out.floatValue = static_cast<double>(value);
    return out;
}

UiValue UiValue::fromFloat(double value) {
    UiValue out;
    out.kind = Kind::Float;
    out.floatValue = value;
    out.intValue = static_cast<long long>(value);
    return out;
}

UiValue UiValue::fromString(const std::string& value) {
    UiValue out;
    out.kind = Kind::String;
    out.stringValue = value;
    return out;
}

bool UiValue::toBool(bool fallback) const {
    switch (kind) {
        case Kind::Bool: return boolValue;
        case Kind::Int: return intValue != 0;
        case Kind::Float: return floatValue != 0.0;
        case Kind::String:
            return stringValue == "true" || stringValue == "1" || stringValue == "yes" ||
                   stringValue == "on";
        default: return fallback;
    }
}

long long UiValue::toInt(long long fallback) const {
    switch (kind) {
        case Kind::Int: return intValue;
        case Kind::Float: return static_cast<long long>(floatValue);
        case Kind::Bool: return boolValue ? 1 : 0;
        case Kind::String: {
            if (stringValue.empty()) return fallback;
            char* end = nullptr;
            const long long parsed = std::strtoll(stringValue.c_str(), &end, 10);
            return end == stringValue.c_str() ? fallback : parsed;
        }
        default: return fallback;
    }
}

double UiValue::toFloat(double fallback) const {
    switch (kind) {
        case Kind::Float: return floatValue;
        case Kind::Int: return static_cast<double>(intValue);
        case Kind::Bool: return boolValue ? 1.0 : 0.0;
        case Kind::String: {
            if (stringValue.empty()) return fallback;
            char* end = nullptr;
            const double parsed = std::strtod(stringValue.c_str(), &end);
            return end == stringValue.c_str() ? fallback : parsed;
        }
        default: return fallback;
    }
}

std::string UiValue::toString(const std::string& fallback) const {
    switch (kind) {
        case Kind::String: return stringValue;
        case Kind::Bool: return boolValue ? "true" : "false";
        case Kind::Int: return std::to_string(intValue);
        case Kind::Float: {
            // Trimmed rather than printf("%f"): these strings land in settings files and in the
            // editor's text rows, where "0.500000" reads as a bug.
            std::string text = std::to_string(floatValue);
            const size_t dot = text.find('.');
            if (dot != std::string::npos) {
                text.erase(text.find_last_not_of('0') + 1);
                if (!text.empty() && text.back() == '.') text.pop_back();
            }
            return text;
        }
        default: return fallback;
    }
}

UiField* UiSchema::findField(const std::string& fieldId) {
    for (UiGroup& group : groups) {
        for (UiField& field : group.fields) {
            if (field.id == fieldId) return &field;
        }
    }
    return nullptr;
}

const UiField* UiSchema::findField(const std::string& fieldId) const {
    for (const UiGroup& group : groups) {
        for (const UiField& field : group.fields) {
            if (field.id == fieldId) return &field;
        }
    }
    return nullptr;
}

void UiSchema::clear() {
    groups.clear();
    title.clear();
}

UiSchemaBuilder::UiSchemaBuilder(UiSchema& target): schema(target) {
}

void UiSchemaBuilder::title(const std::string& text) {
    schema.title = text;
}

void UiSchemaBuilder::group(const std::string& text) {
    schema.groups.push_back(UiGroup{});
    schema.groups.back().title = text;
    current = &schema.groups.back();
}

UiField& UiSchemaBuilder::add(const std::string& id, const std::string& text, UiControlType type) {
    // A plugin that forgets group() still gets a usable panel instead of dropping the field.
    if (!current) group(std::string());
    current->fields.push_back(UiField{});
    UiField& field = current->fields.back();
    field.id = id;
    field.label = text;
    field.type = type;
    return field;
}

// Lets a plugin decorate the control it just added (tooltip, enabled) without threading the
// UiField reference back out of the builder.
UiField& UiSchemaBuilder::lastField() {
    if (!current || current->fields.empty()) {
        return add(std::string(), std::string(), UiControlType::Label);
    }
    return current->fields.back();
}

void UiSchemaBuilder::label(const std::string& text) {
    add(std::string(), text, UiControlType::Label);
}

void UiSchemaBuilder::separator() {
    add(std::string(), std::string(), UiControlType::Separator);
}

void UiSchemaBuilder::boolean(const std::string& id, const std::string& text, bool value) {
    UiField& field = add(id, text, UiControlType::Bool);
    field.value = UiValue::fromBool(value);
}

void UiSchemaBuilder::intSlider(const std::string& id, const std::string& text, long long value,
                                long long minValue, long long maxValue, long long stepValue) {
    UiField& field = add(id, text, UiControlType::IntSlider);
    field.value = UiValue::fromInt(value);
    field.minValue = static_cast<double>(minValue);
    field.maxValue = static_cast<double>(maxValue);
    field.stepValue = static_cast<double>(stepValue);
}

void UiSchemaBuilder::intInput(const std::string& id, const std::string& text, long long value,
                               long long minValue, long long maxValue) {
    UiField& field = add(id, text, UiControlType::IntInput);
    field.value = UiValue::fromInt(value);
    field.minValue = static_cast<double>(minValue);
    field.maxValue = static_cast<double>(maxValue);
    field.stepValue = 1.0;
}

void UiSchemaBuilder::floatSlider(const std::string& id, const std::string& text, double value,
                                  double minValue, double maxValue, double stepValue) {
    UiField& field = add(id, text, UiControlType::FloatSlider);
    field.value = UiValue::fromFloat(value);
    field.minValue = minValue;
    field.maxValue = maxValue;
    field.stepValue = stepValue;
}

void UiSchemaBuilder::floatInput(const std::string& id, const std::string& text, double value,
                                 double minValue, double maxValue) {
    UiField& field = add(id, text, UiControlType::FloatInput);
    field.value = UiValue::fromFloat(value);
    field.minValue = minValue;
    field.maxValue = maxValue;
}

void UiSchemaBuilder::enumeration(const std::string& id, const std::string& text, int selectedIndex,
                                  const std::vector<std::string>& entries) {
    UiField& field = add(id, text, UiControlType::Enum);
    field.options = entries;
    field.optionIndex = selectedIndex;
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size())) {
        field.value = UiValue::fromString(entries[selectedIndex]);
    }
}

void UiSchemaBuilder::color(const std::string& id, const std::string& text, float red, float green,
                            float blue, float alpha) {
    UiField& field = add(id, text, UiControlType::Color);
    field.color[0] = red;
    field.color[1] = green;
    field.color[2] = blue;
    field.color[3] = alpha;
    // The whole colour travels, not one channel: the editor panel writes this same spelling back on
    // every edit, so a plugin never has to guess which of the two produced the value.
    field.value = UiValue::fromString(uiColorToText(field.color));
}

void UiSchemaBuilder::button(const std::string& id, const std::string& text) {
    add(id, text, UiControlType::Button);
}

void UiSchemaBuilder::text(const std::string& id, const std::string& text,
                           const std::string& value) {
    UiField& field = add(id, text, UiControlType::MultilineText);
    field.value = UiValue::fromString(value);
}

UiValueMap quantum::plugin::collectSchemaValues(const UiSchema& source) {
    UiValueMap out;
    for (const UiGroup& group : source.groups) {
        for (const UiField& field : group.fields) {
            if (field.id.empty()) continue;
            if (field.type == UiControlType::Button || field.type == UiControlType::Label ||
                field.type == UiControlType::Separator) {
                continue;
            }
            out[field.id] = field.value;
        }
    }
    return out;
}

namespace {

    unsigned int hexComponent(char first, char second) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int high = nibble(first);
        const int low = nibble(second);
        if (high < 0 || low < 0) return 0xFFFFFFFFu;
        return static_cast<unsigned int>(high * 16 + low);
    }

}

std::string quantum::plugin::uiColorToText(const float components[4]) {
    char text[16];
    auto byte = [](float value) -> int {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<int>(std::lround(clamped * 255.0f));
    };
    std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", byte(components[0]), byte(components[1]),
                  byte(components[2]), components[3] >= 0.9999f ? 255 : byte(components[3]));
    return std::string(text);
}

bool quantum::plugin::uiColorFromText(const std::string& text, float components[4]) {
    std::string digits = text;
    if (!digits.empty() && digits[0] == '#') digits.erase(digits.begin());

    if (digits.size() == 6) digits += "ff";
    if (digits.size() != 8) return false;

    for (int i = 0; i < 4; i++) {
        const unsigned int value = hexComponent(digits[i * 2], digits[i * 2 + 1]);
        if (value == 0xFFFFFFFFu) return false;
        components[i] = static_cast<float>(value) / 255.0f;
    }
    return true;
}
