// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_UI_SCHEMA_H
#define QUANTUM_PLUGIN_UI_SCHEMA_H

#include "Export.h"

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace quantum {
namespace plugin {

    // ImGui is compiled into each editor executable (CMakeLists.txt:533-565) and the engine does
    // not link it at all, so a plugin cannot draw. It describes instead, and the editor renders
    // these controls with its own ImGui context. Adding a control here means adding one draw
    // case in the editor, not a new plugin API.
    enum class UiControlType : uint8_t {
        Label,
        Separator,
        Bool,
        IntSlider,
        IntInput,
        FloatSlider,
        FloatInput,
        Enum,
        Color,
        Button,
        MultilineText
    };

    struct QUANTUM_API UiValue {
        enum class Kind : uint8_t { None = 0, Bool, Int, Float, String };

        Kind kind = Kind::None;
        bool boolValue = false;
        long long intValue = 0;
        double floatValue = 0.0;
        std::string stringValue;

        UiValue() = default;

        static UiValue fromBool(bool value);
        static UiValue fromInt(long long value);
        static UiValue fromFloat(double value);
        static UiValue fromString(const std::string& value);

        // Coercing reads: a manifest default is parsed as a string, and the plugin asking for a
        // float out of it should not have to know which of the two wrote it.
        bool toBool(bool fallback = false) const;
        long long toInt(long long fallback = 0) const;
        double toFloat(double fallback = 0.0) const;
        std::string toString(const std::string& fallback = std::string()) const;

        bool isNumeric() const { return kind == Kind::Int || kind == Kind::Float; }
    };

    using UiValueMap = std::map<std::string, UiValue>;

    struct UiField {
        std::string id;
        std::string label;
        UiControlType type = UiControlType::Label;

        double minValue = 0.0;
        double maxValue = 1.0;
        double stepValue = 0.01;

        UiValue value;

        // Enum controls only: the entries and which one is selected.
        std::vector<std::string> options;
        int optionIndex = -1;

        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

        bool enabled = true;
        bool changed = false;
        std::string tooltip;
    };

    struct UiGroup {
        std::string title;
        std::vector<UiField> fields;
    };

    struct QUANTUM_API UiSchema {
        std::string pluginId;
        std::string title;
        std::vector<UiGroup> groups;

        UiField* findField(const std::string& fieldId);
        const UiField* findField(const std::string& fieldId) const;
        void clear();
    };

    // Fluent writer handed to IPlugin::buildUiSchema. The plugin owns the layout, the editor owns
    // the pixels; nothing else in the boundary needs to know about widgets.
    class QUANTUM_API UiSchemaBuilder {
    public:
        explicit UiSchemaBuilder(UiSchema& schema);

        void title(const std::string& text);
        void group(const std::string& text);

        void label(const std::string& text);
        void separator();

        void boolean(const std::string& id, const std::string& text, bool value);
        void intSlider(const std::string& id, const std::string& text, long long value,
                       long long minValue, long long maxValue, long long stepValue = 1);
        void intInput(const std::string& id, const std::string& text, long long value,
                      long long minValue, long long maxValue);
        void floatSlider(const std::string& id, const std::string& text, double value,
                         double minValue, double maxValue, double stepValue = 0.01);
        void floatInput(const std::string& id, const std::string& text, double value,
                        double minValue, double maxValue);
        void enumeration(const std::string& id, const std::string& text, int selectedIndex,
                         const std::vector<std::string>& entries);
        void color(const std::string& id, const std::string& text, float red, float green,
                   float blue, float alpha = 1.0f);
        void button(const std::string& id, const std::string& text);
        void text(const std::string& id, const std::string& text, const std::string& value);

        UiField& lastField();

    private:
        UiField& add(const std::string& id, const std::string& text, UiControlType type);

        UiSchema& schema;
        UiGroup* current = nullptr;
    };

    // Turns the fields the user edited back into values the plugin can read, skipping the
    // controls that carry no data.
    QUANTUM_API UiValueMap collectSchemaValues(const UiSchema& schema);

    // The wire spelling of a colour field: lowercase "#rrggbbaa". Both sides of the boundary use it -
    // the builder seeds a colour's value with it and the editor panel writes it back on every edit -
    // so a plugin reads the same shape before and after the user touches the swatch. `fromText`
    // accepts "#rrggbb" too and fills in an opaque alpha.
    QUANTUM_API std::string uiColorToText(const float components[4]);
    QUANTUM_API bool uiColorFromText(const std::string& text, float components[4]);

}
}

#endif // QUANTUM_PLUGIN_UI_SCHEMA_H
