// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/core/UXML.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "tinyxml2.h"
#include "slate/core/Widget.h"
#include "slate/layout/Containers.h"
#include "slate/widgets/Button.h"
#include "slate/widgets/CheckBox.h"
#include "slate/widgets/Label.h"
#include "slate/widgets/RadioButton.h"
#include "slate/widgets/Slider.h"

namespace quantum::slate {
namespace {

void report(std::vector<UXMLDiagnostic>* diagnostics, const tinyxml2::XMLElement* element,
            const std::string& message) {
    if (!diagnostics) return;
    diagnostics->push_back({element ? element->GetLineNum() : 0, message});
}

bool booleanAttribute(const tinyxml2::XMLElement& element, const char* name, bool& value,
                     std::vector<UXMLDiagnostic>* diagnostics) {
    const char* text = element.Attribute(name);
    if (!text) return true;
    std::string normalized(text);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "true" || normalized == "1") value = true;
    else if (normalized == "false" || normalized == "0") value = false;
    else {
        report(diagnostics, &element, "invalid boolean attribute: " + std::string(name));
        return false;
    }
    return true;
}

bool floatAttribute(const tinyxml2::XMLElement& element, const char* name, float& value,
                    std::vector<UXMLDiagnostic>* diagnostics) {
    const char* text = element.Attribute(name);
    if (!text) return true;
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0') {
        report(diagnostics, &element, "invalid number attribute: " + std::string(name));
        return false;
    }
    value = parsed;
    return true;
}

void applyCommonAttributes(Widget& widget, const tinyxml2::XMLElement& element,
                           std::vector<UXMLDiagnostic>* diagnostics) {
    if (const char* id = element.Attribute("id")) widget.setId(id);
    if (const char* classes = element.Attribute("class")) {
        std::istringstream stream(classes);
        std::string className;
        while (stream >> className) widget.addStyleClass(className);
    }
    if (const char* style = element.Attribute("style")) widget.setStyleName(style);

    bool enabled = true;
    if (booleanAttribute(element, "enabled", enabled, diagnostics)) widget.setEnabled(enabled);
    bool focusable = false;
    if (booleanAttribute(element, "focusable", focusable, diagnostics)) widget.setFocusable(focusable);
}

std::unique_ptr<Widget> createWidget(const tinyxml2::XMLElement& element,
                                     std::vector<UXMLDiagnostic>* diagnostics) {
    const std::string tag = element.Name() ? element.Name() : std::string();
    const char* id = element.Attribute("id");
    const std::string widgetId = id ? id : std::string();
    std::unique_ptr<Widget> widget;
    if (tag == "Widget") widget = std::make_unique<Widget>(widgetId);
    else if (tag == "Label") widget = std::make_unique<Label>(widgetId);
    else if (tag == "Button") widget = std::make_unique<Button>(widgetId);
    else if (tag == "CheckBox") widget = std::make_unique<CheckBox>(widgetId);
    else if (tag == "RadioButton") widget = std::make_unique<RadioButton>(widgetId);
    else if (tag == "Slider") widget = std::make_unique<Slider>(widgetId);
    else if (tag == "Box" || tag == "HorizontalBox" || tag == "VerticalBox") {
        Orientation orientation = Orientation::Horizontal;
        if (tag == "VerticalBox") orientation = Orientation::Vertical;
        if (const char* value = element.Attribute("orientation")) {
            if (std::string(value) == "vertical") orientation = Orientation::Vertical;
            else if (std::string(value) != "horizontal")
                report(diagnostics, &element, "invalid Box orientation");
        }
        widget = std::make_unique<Box>(orientation, widgetId);
    } else if (tag == "Overlay") widget = std::make_unique<Overlay>(widgetId);
    else if (tag == "Border") widget = std::make_unique<Border>(widgetId);
    else if (tag == "CanvasPanel") widget = std::make_unique<CanvasPanel>(widgetId);
    else if (tag == "ScrollBox") widget = std::make_unique<ScrollBox>(widgetId);
    else {
        report(diagnostics, &element, "unknown UXML widget: " + tag);
        return nullptr;
    }

    applyCommonAttributes(*widget, element, diagnostics);
    const char* text = element.Attribute("text");
    if (!text) text = element.GetText();
    if (text) {
        if (auto* label = dynamic_cast<Label*>(widget.get())) label->setText(text);
        else if (auto* button = dynamic_cast<Button*>(widget.get())) button->setText(text);
        else if (auto* checkBox = dynamic_cast<CheckBox*>(widget.get())) checkBox->setText(text);
        else if (auto* radio = dynamic_cast<RadioButton*>(widget.get())) radio->setText(text);
    }
    if (auto* checkBox = dynamic_cast<CheckBox*>(widget.get())) {
        bool checked = false;
        if (booleanAttribute(element, "checked", checked, diagnostics)) checkBox->setChecked(checked);
    }
    if (auto* slider = dynamic_cast<Slider*>(widget.get())) {
        float minValue = slider->getMinValue();
        float maxValue = slider->getMaxValue();
        float step = slider->getStep();
        float value = slider->getValue();
        const bool valid = floatAttribute(element, "min", minValue, diagnostics) &&
            floatAttribute(element, "max", maxValue, diagnostics) &&
            floatAttribute(element, "step", step, diagnostics) &&
            floatAttribute(element, "value", value, diagnostics);
        if (valid) slider->setRange(minValue, maxValue).setStep(step).setValue(value);
    }
    if (auto* radio = dynamic_cast<RadioButton*>(widget.get())) {
        bool checked = false;
        if (booleanAttribute(element, "checked", checked, diagnostics)) radio->setChecked(checked);
        if (const char* group = element.Attribute("group")) radio->setGroup(group);
    }
    return widget;
}

std::unique_ptr<Widget> buildTree(const tinyxml2::XMLElement& element,
                                  std::vector<UXMLDiagnostic>* diagnostics) {
    std::unique_ptr<Widget> widget = createWidget(element, diagnostics);
    if (!widget) return nullptr;
    for (const tinyxml2::XMLElement* child = element.FirstChildElement(); child;
         child = child->NextSiblingElement()) {
        std::unique_ptr<Widget> childWidget = buildTree(*child, diagnostics);
        if (!childWidget) return nullptr;
        widget->addChild(std::move(childWidget));
    }
    return widget;
}

}

std::unique_ptr<Widget> UXMLParser::parse(const std::string& source,
                                          std::vector<UXMLDiagnostic>* diagnostics) {
    tinyxml2::XMLDocument document;
    const tinyxml2::XMLError result = document.Parse(source.c_str(), source.size());
    if (result != tinyxml2::XML_SUCCESS) {
        if (diagnostics) diagnostics->push_back({document.ErrorLineNum(), document.ErrorStr()});
        return nullptr;
    }
    const tinyxml2::XMLElement* root = document.RootElement();
    if (!root) {
        if (diagnostics) diagnostics->push_back({0, "UXML has no root element"});
        return nullptr;
    }
    return buildTree(*root, diagnostics);
}

}
