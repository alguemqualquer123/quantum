// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "../../libs/doctest/doctest.h"

#include <string>
#include <vector>

#include "slate/core/USS.h"
#include "slate/core/Widget.h"
#include "slate/widgets/Button.h"

using namespace quantum::slate;

TEST_CASE("slate::USS parses selectors and resolves visual states") {
    StyleSet styles;
    std::vector<USSDiagnostic> diagnostics;
    const std::string source =
        ":root { --accent: #336699; }\n"
        ".primary { background-color: var(--accent); padding: 4; }\n"
        "#play:hover { background-color: #ff0000; color: #ffffff; }\n";

    CHECK(USSParser::parse(source, styles, &diagnostics));
    CHECK(diagnostics.empty());

    Widget button("play");
    button.addStyleClass("primary");
    StyleSet::set(&styles);

    button.applyStyle();
    CHECK(doctest::Approx(button.getStyle().backgroundColor.x) == 0.2f);
    CHECK(doctest::Approx(button.getStyle().backgroundColor.y) == 0.4f);
    CHECK(doctest::Approx(button.getStyle().backgroundColor.z) == 0.6f);
    CHECK(doctest::Approx(button.getStyle().backgroundColor.w) == 1.0f);
    CHECK(button.getStyle().padding.left == doctest::Approx(4.0f));

    button.setHovered(true);
    button.applyStyle();
    CHECK(doctest::Approx(button.getStyle().backgroundColor.x) == 1.0f);
    CHECK(doctest::Approx(button.getStyle().backgroundColor.y) == 0.0f);
    CHECK(doctest::Approx(button.getStyle().backgroundColor.z) == 0.0f);
    CHECK(doctest::Approx(button.getStyle().textColor.w) == 1.0f);
    StyleSet::set(nullptr);
}

TEST_CASE("slate::USS reports malformed declarations without throwing") {
    StyleSet styles;
    std::vector<USSDiagnostic> diagnostics;
    CHECK_FALSE(USSParser::parse(".broken { background-color: nope; }", styles, &diagnostics));
    REQUIRE(diagnostics.size() == 1u);
    CHECK(diagnostics[0].line == 1);
}

TEST_CASE("slate::USS type selectors see concrete widget types") {
    StyleSet styles;
    NamedStyle& buttonStyle = styles.defineStyle("__button_type");
    buttonStyle.base().borderWidth = 3.0f;
    styles.registerRule("Button", "__button_type");
    Button button;
    StyleSet::set(&styles);
    button.applyStyle();
    CHECK(std::string(button.getTypeName()) == "Button");
    CHECK(doctest::Approx(button.getStyle().borderWidth) == 3.0f);
    StyleSet::set(nullptr);
}
