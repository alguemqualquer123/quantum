// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "../../libs/doctest/doctest.h"

#include <vector>

#include "Input.h"
#include "slate/core/EventRouter.h"
#include "slate/core/UXML.h"
#include "slate/layout/Containers.h"
#include "slate/widgets/Button.h"
#include "slate/widgets/CheckBox.h"
#include "slate/widgets/Label.h"
#include "slate/widgets/RadioButton.h"
#include "slate/widgets/Slider.h"

using namespace quantum;
using namespace quantum::slate;

TEST_CASE("slate::UXML builds a typed widget tree") {
    std::vector<UXMLDiagnostic> diagnostics;
    const auto root = UXMLParser::parse(
        "<VerticalBox id='hud' class='screen dark' focusable='true'>"
        "<Label id='title' text='Quantum'/>"
        "<Button id='play' class='primary' style='Hud.Button' text='Play'/>"
        "</VerticalBox>", &diagnostics);

    REQUIRE(root != nullptr);
    CHECK(diagnostics.empty());
    CHECK(root->getId() == "hud");
    CHECK(root->hasStyleClass("screen"));
    CHECK(root->isFocusable());
    REQUIRE(root->getChildCount() == 2u);
    CHECK(dynamic_cast<Label*>(root->getChildren()[0].get())->getText() == "Quantum");
    auto* button = dynamic_cast<Button*>(root->getChildren()[1].get());
    REQUIRE(button != nullptr);
    CHECK(button->getText() == "Play");
    CHECK(button->getStyleName() == "Hud.Button");
    CHECK(button->hasStyleClass("primary"));
}

TEST_CASE("slate::CheckBox is interactive and UXML-configurable") {
    std::vector<UXMLDiagnostic> diagnostics;
    const auto root = UXMLParser::parse(
        "<CheckBox id='sound' text='Sound' checked='true' focusable='true'/>", &diagnostics);
    REQUIRE(root != nullptr);
    CHECK(diagnostics.empty());
    auto* checkBox = dynamic_cast<CheckBox*>(root.get());
    REQUIRE(checkBox != nullptr);
    CHECK(checkBox->getText() == "Sound");
    CHECK(checkBox->isChecked());

    bool callbackCalled = false;
    bool changed = false;
    checkBox->setOnChanged([&callbackCalled, &changed](bool value) {
        callbackCalled = true;
        changed = value;
    });
    checkBox->setLayoutRect(Rect(0.0f, 0.0f, 200.0f, 40.0f));
    EventRouter::reset();
    CHECK(EventRouter::pointerDown(*checkBox, Vector2(10.0f, 10.0f), D_MOUSE_BUTTON_LEFT) == false);
    CHECK(EventRouter::pointerUp(*checkBox, Vector2(10.0f, 10.0f), D_MOUSE_BUTTON_LEFT));
    CHECK(!checkBox->isChecked());
    CHECK(callbackCalled);
    CHECK(!changed);
}

TEST_CASE("slate::UXML reports unknown widgets") {
    std::vector<UXMLDiagnostic> diagnostics;
    CHECK(UXMLParser::parse("<NotAWidget/>", &diagnostics) == nullptr);
    REQUIRE(diagnostics.size() == 1u);
    CHECK(diagnostics[0].message.find("unknown UXML widget") != std::string::npos);
}

TEST_CASE("slate::Slider supports pointer and keyboard adjustment") {
    std::vector<UXMLDiagnostic> diagnostics;
    const auto root = UXMLParser::parse(
        "<HorizontalBox><Slider id='volume' min='0' max='10' value='2' step='1' focusable='true'/></HorizontalBox>",
        &diagnostics);
    REQUIRE(root != nullptr);
    CHECK(diagnostics.empty());
    auto* slider = dynamic_cast<Slider*>(root->getChildren()[0].get());
    REQUIRE(slider != nullptr);
    CHECK(slider->getValue() == doctest::Approx(2.0f));
    bool callbackCalled = false;
    slider->setOnChanged([&callbackCalled](float) { callbackCalled = true; });
    root->setLayoutRect(Rect(0.0f, 0.0f, 240.0f, 40.0f));
    slider->setLayoutRect(Rect(0.0f, 0.0f, 200.0f, 40.0f));
    EventRouter::reset();
    CHECK(EventRouter::pointerDown(*root, Vector2(150.0f, 20.0f), D_MOUSE_BUTTON_LEFT));
    CHECK(slider->getValue() == doctest::Approx(8.0f));
    CHECK(callbackCalled);
    CHECK(EventRouter::keyDown(*root, D_KEY_LEFT));
    CHECK(slider->getValue() == doctest::Approx(7.0f));
    CHECK(EventRouter::pointerUp(*root, Vector2(150.0f, 20.0f), D_MOUSE_BUTTON_LEFT));
    EventRouter::reset();
}

TEST_CASE("slate::RadioButton enforces sibling exclusivity and UXML attributes") {
    std::vector<UXMLDiagnostic> diagnostics;
    const auto root = UXMLParser::parse(
        "<HorizontalBox><RadioButton id='one' text='One' group='mode' checked='true'/>"
        "<RadioButton id='two' text='Two' group='mode'/></HorizontalBox>", &diagnostics);
    REQUIRE(root != nullptr);
    CHECK(diagnostics.empty());
    auto* one = dynamic_cast<RadioButton*>(root->getChildren()[0].get());
    auto* two = dynamic_cast<RadioButton*>(root->getChildren()[1].get());
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    CHECK(one->isChecked());
    CHECK(two->getGroup() == "mode");
    two->setLayoutRect(Rect(100.0f, 0.0f, 100.0f, 32.0f));
    bool changed = false;
    two->setOnChanged([&changed](bool value) { changed = value; });
    EventRouter::reset();
    CHECK(EventRouter::pointerUp(*two, Vector2(110.0f, 10.0f), D_MOUSE_BUTTON_LEFT));
    CHECK(!one->isChecked());
    CHECK(two->isChecked());
    CHECK(changed);
    EventRouter::reset();
}
