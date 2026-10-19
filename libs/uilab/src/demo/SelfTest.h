// SelfTest.h - headless verification of the lab's logic.
//
// Run with `UniversalUIComponentsLab --self-test`. No window, no OpenGL and no
// ImGui frame are created, so tests must only touch the plain C++ subsystems
// (registry, undo, validation, serialisation, canvas layout, asset library,
// theme, stylesheet) and must never call into ImGui.
#pragma once

#include <string>
#include <vector>

namespace uilab {

class AppContext;

namespace demo {

struct TestCase {
    const char* name;
    bool passed;
    std::string detail;
};

// Runs every check against a freshly prepared context and appends human
// readable results to `report`. Returns the number of failures.
int RunSelfTest(AppContext& ctx, bool verbose, std::vector<TestCase>& results, std::string& report);

// Convenience used by main(): runs the suite and prints it to stdout.
int RunSelfTestAndPrint(AppContext& ctx, bool verbose);

} // namespace demo
} // namespace uilab
