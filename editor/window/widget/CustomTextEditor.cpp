// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "CustomTextEditor.h"
#include "imgui_internal.h"
#include "InputTextContextMenu.h"
#include "SemanticSuggestions.h"
#include "engine_api_suggestions.h"
#include "external/IconsFontAwesome6.h"
#include "script/ScriptSearch.h"
#include "util/UIUtils.h"
#include "App.h"
#include "Backend.h"
#include "KeyBindings.h"
#include "Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <sstream>
#include <cstring>
#include <regex>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace quantum::editor {

namespace {

// Case-folded copy, for case-insensitive search
std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// needle at pos, without the out-of-range compare() that throws (and aborts here)
bool startsWithAt(const std::string& text, size_t pos, const std::string& needle) {
    return pos <= text.size() && text.compare(pos, needle.size(), needle) == 0;
}

// Identifier that ends at endColumn, skipping trailing whitespace
std::string wordEndingAt(const std::string& line, int endColumn) {
    int end = std::min(endColumn, static_cast<int>(line.size()) - 1);
    while (end >= 0 && std::isspace(static_cast<unsigned char>(line[end]))) end--;

    int start = end + 1;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '_')) start--;

    if (start > end) return "";
    return line.substr(start, end - start + 1);
}

bool isEngineApiConstructorSymbol(const EngineAPISymbol& sym) {
    if (!sym.name || !sym.parent || !sym.detail) return false;

    const std::string name = sym.name;
    const std::string parent = sym.parent;
    const std::string detail = sym.detail;
    return !name.empty() &&
           parent == name &&
           detail.rfind(name + "(", 0) == 0;
}

// Class details name their base as "class Mesh : Object"
std::string baseClassFromDetail(const std::string& detail) {
    size_t colonPos = detail.find(" : ");
    if (colonPos == std::string::npos) return "";

    std::string base = detail.substr(colonPos + 3);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    return base;
}

// Member details start with "Parent.name", which the label already shows
std::string detailAfterName(const std::string& detail, const std::string& parent, const std::string& name) {
    for (const char* sep : {"::", ":", "."}) {
        std::string prefix = parent + sep + name;
        if (detail.rfind(prefix, 0) == 0) {
            size_t start = detail.find_first_not_of(' ', prefix.size());
            return start == std::string::npos ? "" : detail.substr(start);
        }
    }
    return "";
}

bool isUtf8Continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// Lines without their '\r', as the editor holds them
void splitLines(const std::string& text, std::vector<std::string>& out) {
    out.clear();

    std::string line;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    out.push_back(line);
}

// VSCode's Dark Modern gutter colors
constexpr ImU32 DIFF_ADDED_COLOR = IM_COL32(0x2E, 0xA0, 0x43, 0xFF);
constexpr ImU32 DIFF_MODIFIED_COLOR = IM_COL32(0x00, 0x78, 0xD4, 0xFF);
constexpr ImU32 DIFF_DELETED_COLOR = IM_COL32(0xF8, 0x51, 0x49, 0xFF);

ImU32 diffColor(const LineChange& change) {
    if (change.originalStart == change.originalEnd) return DIFF_ADDED_COLOR;
    if (change.modifiedStart == change.modifiedEnd) return DIFF_DELETED_COLOR;
    return DIFF_MODIFIED_COLOR;
}

// Modified lines get a striped bar
void drawHatchedBar(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color) {
    // Only the visible part, a block can span thousands of lines
    const float top = std::max(min.y, drawList->GetClipRectMin().y);
    const float bottom = std::min(max.y, drawList->GetClipRectMax().y);
    if (top >= bottom) return;

    const float width = max.x - min.x;
    const float thickness = width * 0.5f;

    // Antialiasing would blur stripes this thin into a flat tint
    const ImDrawListFlags flags = drawList->Flags;
    drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    drawList->PushClipRect(ImVec2(min.x, top), ImVec2(max.x, bottom), true);

    // Anchored to the block, so they scroll with the text
    for (float y = min.y + std::floor((top - min.y) / width) * width - 2.0f * width; y < bottom; y += width) {
        drawList->AddQuadFilled(ImVec2(min.x, y + width), ImVec2(max.x, y),
                                ImVec2(max.x, y + thickness), ImVec2(min.x, y + width + thickness), color);
    }

    drawList->PopClipRect();
    drawList->Flags = flags;
}

// VSCode's diff tints, the changed part of a line gets a second one
constexpr ImU32 DIFF_REMOVED_LINE_BG = IM_COL32(255, 0, 0, 51);
constexpr ImU32 DIFF_ADDED_LINE_BG = IM_COL32(155, 185, 85, 51);
constexpr ImU32 DIFF_REMOVED_TEXT_BG = IM_COL32(255, 0, 0, 51);
constexpr ImU32 DIFF_ADDED_TEXT_BG = IM_COL32(156, 204, 44, 51);

// Seconds on a marker to open its popup, and off both to close it
constexpr double DIFF_PEEK_DELAY = 0.15;
constexpr double DIFF_PEEK_GRACE = 0.3;

// Bytes between the common prefix and suffix of two lines, cut at characters
void changedBytes(const std::string& before, const std::string& after, int& start, int& beforeEnd, int& afterEnd) {
    auto insideCharacter = [&](size_t i) {
        return (i < before.size() && isUtf8Continuation(before[i])) || (i < after.size() && isUtf8Continuation(after[i]));
    };

    size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) prefix++;
    while (prefix > 0 && insideCharacter(prefix)) prefix--;

    size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        suffix++;
    }
    while (suffix > 0 && isUtf8Continuation(before[before.size() - suffix])) suffix--;

    start = static_cast<int>(prefix);
    beforeEnd = static_cast<int>(before.size() - suffix);
    afterEnd = static_cast<int>(after.size() - suffix);
}

} // namespace

CustomTextEditor::CustomTextEditor()
    : primaryCursor(0)
    , undoIndex(0)
    , language(SyntaxLanguage::None)
    , readOnly(false)
    , tabSize(4)
    , showLineNumbers(true)
    , autoIndent(true)
    , highlightCurrentLine(true)
    , matchBrackets(true)
    , autoComplete(true)
    , isDragging(false)
    , isMiddleDragging(false)
    , isDraggingText(false)
    , cursorBlinkOn(false)
    , middleDragLine(0)
    , middleDragX(0.0f)
    , mayDragText(false)
    , clickCount(0)
    , scrollX(0)
    , scrollY(0)
    , showAutoComplete(false)
    , suggestionIndex(0)
    , suggestionsHovered(false)
    , showParamHint(false)
    , paramHintActiveParam(0)
    , paramHintOverloadIndex(0)
    , currentSearchResult(-1)
    , showFindDialog(false)
    , showReplaceInput(false)
    , findCaseSensitive(false)
    , findUseRegex(false)
    , findWholeWord(false)
    , findInSelection(false)
    , showGoToLine(false)
    , goToLineRefocus(false)
    , charWidth(0)
    , lineHeight(0)
    , lineHeightFactor(1.35f)
    , textOffsetY(0)
    , lineNumberDigits(1)
    , lineNumberWidth(0)
    , leftMargin(10)
    , textStartX(0)
    , measureFont(nullptr)
    , measureFontSize(0)
    , maxLineWidth(0)
    , maxLineWidthDirty(true)
    , hasDiffBase(false)
    , diffDirty(false)
    , diffNextTime(0.0)
    , diffVersion(1)
    , diffHoverChange(-1)
    , diffHoverStart(0.0)
    , diffPeekChange(-1)
    , diffPeekKey{}
    , diffPeekLeaveTime(-1.0)
    , diffPeekHovered(false)
    , diffPeekVersion(0)
    , diffPeekTextWidth(0.0f)
    , suggestions(std::make_unique<SemanticSuggestions>())
    , scrollToSuggestion(false)
{
    lines.push_back("");
    cursors.push_back(Cursor());
    memset(findInputBuffer, 0, sizeof(findInputBuffer));
    memset(replaceInputBuffer, 0, sizeof(replaceInputBuffer));
    memset(goToLineBuffer, 0, sizeof(goToLineBuffer));
    initializePalette();
    initializeLanguage();
    initializeSuggestions();
}

CustomTextEditor::~CustomTextEditor() {
}

void CustomTextEditor::initializePalette() {
    // VS Code Dark+ inspired color scheme
    backgroundColor = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);
    lineNumberColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    currentLineColor = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    selectionColor = ImVec4(0.26f, 0.40f, 0.60f, 0.5f);
    cursorColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    matchingBracketColor = ImVec4(0.5f, 0.5f, 0.5f, 0.4f);
    searchHighlightColor = ImVec4(0.9f, 0.8f, 0.3f, 0.3f);

    palette[static_cast<int>(TokenType::Default)] = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    palette[static_cast<int>(TokenType::Keyword)] = ImVec4(0.57f, 0.44f, 0.86f, 1.0f);      // Purple
    palette[static_cast<int>(TokenType::Type)] = ImVec4(0.31f, 0.69f, 0.78f, 1.0f);         // Cyan
    palette[static_cast<int>(TokenType::Number)] = ImVec4(0.71f, 0.82f, 0.53f, 1.0f);       // Light green
    palette[static_cast<int>(TokenType::String)] = ImVec4(0.81f, 0.55f, 0.42f, 1.0f);       // Orange
    palette[static_cast<int>(TokenType::Comment)] = ImVec4(0.42f, 0.55f, 0.35f, 1.0f);      // Green
    palette[static_cast<int>(TokenType::MultiLineComment)] = ImVec4(0.42f, 0.55f, 0.35f, 1.0f);
    palette[static_cast<int>(TokenType::Preprocessor)] = ImVec4(0.61f, 0.43f, 0.62f, 1.0f); // Magenta
    palette[static_cast<int>(TokenType::Identifier)] = ImVec4(0.61f, 0.82f, 0.98f, 1.0f);   // Light blue
    palette[static_cast<int>(TokenType::Punctuation)] = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    palette[static_cast<int>(TokenType::Operator)] = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    palette[static_cast<int>(TokenType::Function)] = ImVec4(0.86f, 0.82f, 0.53f, 1.0f);     // Yellow
}

void CustomTextEditor::initializeLanguage() {
    languageDef = LanguageDefinition();

    switch (language) {
        case SyntaxLanguage::Cpp:
            languageDef.keywords = {
                "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
                "break", "case", "catch", "class", "compl", "concept", "const", "consteval",
                "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
                "co_yield", "decltype", "default", "delete", "do", "dynamic_cast", "else",
                "enum", "explicit", "export", "extern", "false", "for", "friend", "goto",
                "if", "inline", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
                "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
                "register", "reinterpret_cast", "requires", "return", "sizeof", "static",
                "static_assert", "static_cast", "struct", "switch", "template", "this",
                "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
                "union", "using", "virtual", "volatile", "while", "xor", "xor_eq",
                "override", "final"
            };
            languageDef.types = {
                "void", "bool", "char", "wchar_t", "char8_t", "char16_t", "char32_t",
                "short", "int", "long", "signed", "unsigned", "float", "double",
                "int8_t", "int16_t", "int32_t", "int64_t",
                "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                "size_t", "ptrdiff_t", "nullptr_t",
                "string", "vector", "map", "unordered_map", "set", "unordered_set",
                "array", "list", "deque", "queue", "stack", "pair", "tuple",
                "unique_ptr", "shared_ptr", "weak_ptr",
                "function", "optional", "variant", "any"
            };
            // Add engine types from auto-generated header
            for (const auto& t : getEngineTypeNames()) languageDef.types.insert(t);
            languageDef.singleLineComment = "//";
            languageDef.multiLineCommentStart = "/*";
            languageDef.multiLineCommentEnd = "*/";
            languageDef.preprocessorPrefix = "#";
            break;

        case SyntaxLanguage::Lua:
            languageDef.keywords = {
                "and", "break", "do", "else", "elseif", "end", "false", "for",
                "function", "goto", "if", "in", "local", "nil", "not", "or",
                "repeat", "return", "then", "true", "until", "while"
            };
            languageDef.types = {
                "string", "number", "boolean", "table", "function", "thread",
                "userdata"
            };
            // Add engine types from auto-generated header
            for (const auto& t : getEngineTypeNames()) languageDef.types.insert(t);
            languageDef.builtinFunctions = {
                "assert", "collectgarbage", "dofile", "error", "getmetatable",
                "ipairs", "load", "loadfile", "next", "pairs", "pcall", "print",
                "rawequal", "rawget", "rawlen", "rawset", "require", "select",
                "setmetatable", "tonumber", "tostring", "type", "xpcall",
                "coroutine", "debug", "io", "math", "os", "package", "string", "table", "utf8"
            };
            // Add engine builtins (static classes + enums) from auto-generated header
            for (const auto& b : getEngineBuiltinNames()) languageDef.builtinFunctions.insert(b);
            for (const auto& e : getEngineEnumNames()) languageDef.builtinFunctions.insert(e);
            languageDef.singleLineComment = "--";
            break;

        case SyntaxLanguage::CMake:
            languageDef.keywords = {
                "if", "elseif", "else", "endif", "foreach", "endforeach", "while", "endwhile",
                "function", "endfunction", "macro", "endmacro", "return", "break", "continue",
                "option", "set", "unset", "list", "string", "math", "file", "configure_file",
                "add_executable", "add_library", "add_subdirectory", "add_custom_command",
                "add_custom_target", "add_dependencies", "add_definitions", "add_compile_definitions",
                "add_compile_options", "add_link_options", "target_link_libraries",
                "target_include_directories", "target_compile_definitions", "target_compile_options",
                "target_compile_features", "target_sources", "target_link_options",
                "include", "include_directories", "link_directories", "link_libraries",
                "find_package", "find_library", "find_path", "find_file", "find_program",
                "project", "cmake_minimum_required", "message", "install", "export",
                "set_target_properties", "set_property", "get_property", "get_target_property",
                "get_filename_component", "get_directory_property", "define_property",
                "enable_testing", "add_test", "set_tests_properties",
                "execute_process", "cmake_policy", "cmake_parse_arguments"
            };
            languageDef.types = {
                "BOOL", "STRING", "PATH", "FILEPATH", "INTERNAL", "CACHE", "FORCE",
                "PARENT_SCOPE", "GLOBAL", "DIRECTORY", "TARGET", "SOURCE", "TEST",
                "PRIVATE", "PUBLIC", "INTERFACE", "IMPORTED", "SHARED", "STATIC", "MODULE",
                "OBJECT", "UNKNOWN", "ALIAS", "REQUIRED", "QUIET", "COMPONENTS", "CONFIG",
                "TRUE", "FALSE", "ON", "OFF", "YES", "NO",
                "AND", "OR", "NOT", "COMMAND", "POLICY", "TARGET", "TEST", "DEFINED",
                "EXISTS", "IS_DIRECTORY", "IS_SYMLINK", "IS_ABSOLUTE", "MATCHES",
                "LESS", "GREATER", "EQUAL", "LESS_EQUAL", "GREATER_EQUAL",
                "STRLESS", "STRGREATER", "STREQUAL", "STRLESS_EQUAL", "STRGREATER_EQUAL",
                "VERSION_LESS", "VERSION_GREATER", "VERSION_EQUAL"
            };
            languageDef.builtinFunctions = {
                "CMAKE_SOURCE_DIR", "CMAKE_BINARY_DIR", "CMAKE_CURRENT_SOURCE_DIR",
                "CMAKE_CURRENT_BINARY_DIR", "CMAKE_CURRENT_LIST_DIR", "CMAKE_CURRENT_LIST_FILE",
                "PROJECT_SOURCE_DIR", "PROJECT_BINARY_DIR", "PROJECT_NAME", "PROJECT_VERSION",
                "CMAKE_CXX_STANDARD", "CMAKE_C_STANDARD", "CMAKE_BUILD_TYPE",
                "CMAKE_INSTALL_PREFIX", "CMAKE_MODULE_PATH", "CMAKE_PREFIX_PATH",
                "CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_VERSION", "CMAKE_SYSTEM_PROCESSOR",
                "CMAKE_CXX_COMPILER", "CMAKE_C_COMPILER", "CMAKE_LINKER",
                "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_EXE_LINKER_FLAGS",
                "CMAKE_SHARED_LINKER_FLAGS", "CMAKE_STATIC_LINKER_FLAGS",
                "WIN32", "UNIX", "APPLE", "MSVC", "MINGW", "LINUX"
            };
            languageDef.singleLineComment = "#";
            break;

        default:
            break;
    }

    tokenizeAll();
}

void CustomTextEditor::initializeSuggestions() {
    if (!suggestions) return;

    // Snippets and symbols are appended, so a language switch has to drop the old ones
    suggestions->ClearSnippets();
    suggestions->ClearSymbols();
    suggestions->ClearInheritance();

    suggestions->SetMinPrefixLength(1);
    // The popup scrolls, so a high cap keeps inherited members from being crowded out
    suggestions->SetMaxSuggestions(200);
    suggestions->SetFuzzyMatching(true);
    suggestions->SetCaseSensitive(false);

    suggestions->SetKeywords(languageDef.keywords);
    suggestions->SetTypes(languageDef.types);
    suggestions->SetBuiltinFunctions(languageDef.builtinFunctions);

    if (language == SyntaxLanguage::Cpp) {
        suggestions->AddSnippet("if", "if (${1:condition}) {\n\t${2}\n}", "if statement");
        suggestions->AddSnippet("else", "else {\n\t${1}\n}", "else clause");
        suggestions->AddSnippet("elif", "else if (${1:condition}) {\n\t${2}\n}", "else if statement");
        suggestions->AddSnippet("for", "for (${1:int i = 0}; ${2:i < count}; ${3:++i}) {\n\t${4}\n}", "for loop");
        suggestions->AddSnippet("fori", "for (int ${1:i} = 0; ${1:i} < ${2:count}; ++${1:i}) {\n\t${3}\n}", "for loop with index");
        suggestions->AddSnippet("foreach", "for (const auto& ${1:item} : ${2:container}) {\n\t${3}\n}", "range-based for loop");
        suggestions->AddSnippet("while", "while (${1:condition}) {\n\t${2}\n}", "while loop");
        suggestions->AddSnippet("do", "do {\n\t${1}\n} while (${2:condition});", "do-while loop");
        suggestions->AddSnippet("switch", "switch (${1:expression}) {\n\tcase ${2:value}:\n\t\t${3}\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}", "switch statement");
        suggestions->AddSnippet("class", "class ${1:ClassName} {\npublic:\n\t${1:ClassName}();\n\t~${1:ClassName}();\n\nprivate:\n\t${2}\n};", "class definition");
        suggestions->AddSnippet("struct", "struct ${1:StructName} {\n\t${2}\n};", "struct definition");
        suggestions->AddSnippet("func", "${1:void} ${2:functionName}(${3}) {\n\t${4}\n}", "function definition");
        suggestions->AddSnippet("main", "int main(int argc, char* argv[]) {\n\t${1}\n\treturn 0;\n}", "main function");
        suggestions->AddSnippet("inc", "#include <${1:header}>", "include system header");
        suggestions->AddSnippet("incp", "#include \"${1:header}\"", "include local header");
        suggestions->AddSnippet("ifndef", "#ifndef ${1:GUARD}\n#define ${1:GUARD}\n\n${2}\n\n#endif // ${1:GUARD}", "include guard");
        suggestions->AddSnippet("pragma", "#pragma once", "pragma once");
        suggestions->AddSnippet("cout", "std::cout << ${1} << std::endl;", "cout statement");
        suggestions->AddSnippet("cerr", "std::cerr << ${1} << std::endl;", "cerr statement");
        suggestions->AddSnippet("try", "try {\n\t${1}\n} catch (${2:const std::exception& e}) {\n\t${3}\n}", "try-catch block");
        suggestions->AddSnippet("lambda", "[${1:capture}](${2:params}) {\n\t${3}\n}", "lambda expression");
        suggestions->AddSnippet("nullptr", "nullptr", "null pointer");
        suggestions->AddSnippet("auto", "auto ${1:var} = ${2:value};", "auto variable");
        suggestions->AddSnippet("const", "const ${1:type} ${2:name} = ${3:value};", "const variable");
        suggestions->AddSnippet("constexpr", "constexpr ${1:type} ${2:name} = ${3:value};", "constexpr variable");
    } else if (language == SyntaxLanguage::Lua) {
        suggestions->AddSnippet("if", "if ${1:condition} then\n\t${2}\nend", "if statement");
        suggestions->AddSnippet("ife", "if ${1:condition} then\n\t${2}\nelse\n\t${3}\nend", "if-else statement");
        suggestions->AddSnippet("elif", "elseif ${1:condition} then\n\t${2}", "elseif clause");
        suggestions->AddSnippet("for", "for ${1:i} = ${2:1}, ${3:10} do\n\t${4}\nend", "numeric for loop");
        suggestions->AddSnippet("forp", "for ${1:k}, ${2:v} in pairs(${3:table}) do\n\t${4}\nend", "pairs loop");
        suggestions->AddSnippet("fori", "for ${1:i}, ${2:v} in ipairs(${3:table}) do\n\t${4}\nend", "ipairs loop");
        suggestions->AddSnippet("while", "while ${1:condition} do\n\t${2}\nend", "while loop");
        suggestions->AddSnippet("repeat", "repeat\n\t${1}\nuntil ${2:condition}", "repeat-until loop");
        suggestions->AddSnippet("func", "function ${1:name}(${2:args})\n\t${3}\nend", "function definition");
        suggestions->AddSnippet("lfunc", "local function ${1:name}(${2:args})\n\t${3}\nend", "local function");
        suggestions->AddSnippet("local", "local ${1:name} = ${2:value}", "local variable");
        suggestions->AddSnippet("req", "local ${1:module} = require(\"${2:module}\")", "require module");
        suggestions->AddSnippet("ret", "return ${1:value}", "return statement");
        suggestions->AddSnippet("print", "print(${1:value})", "print statement");
    } else if (language == SyntaxLanguage::CMake) {
        suggestions->AddSnippet("if", "if(${1:condition})\n\t${2}\nendif()", "if statement");
        suggestions->AddSnippet("ife", "if(${1:condition})\n\t${2}\nelse()\n\t${3}\nendif()", "if-else statement");
        suggestions->AddSnippet("foreach", "foreach(${1:item} IN LISTS ${2:list})\n\t${3}\nendforeach()", "foreach loop");
        suggestions->AddSnippet("func", "function(${1:name} ${2:args})\n\t${3}\nendfunction()", "function definition");
        suggestions->AddSnippet("macro", "macro(${1:name} ${2:args})\n\t${3}\nendmacro()", "macro definition");
        suggestions->AddSnippet("add_exe", "add_executable(${1:target}\n\t${2:sources}\n)", "add executable");
        suggestions->AddSnippet("add_lib", "add_library(${1:target} ${2:STATIC}\n\t${3:sources}\n)", "add library");
        suggestions->AddSnippet("target_link", "target_link_libraries(${1:target}\n\t${2:PRIVATE}\n\t${3:libraries}\n)", "link libraries");
        suggestions->AddSnippet("target_inc", "target_include_directories(${1:target}\n\t${2:PRIVATE}\n\t${3:directories}\n)", "include directories");
        suggestions->AddSnippet("find", "find_package(${1:package} ${2:REQUIRED})", "find package");
        suggestions->AddSnippet("set", "set(${1:variable} ${2:value})", "set variable");
        suggestions->AddSnippet("msg", "message(STATUS \"${1:message}\")", "status message");
        suggestions->AddSnippet("option", "option(${1:OPTION_NAME} \"${2:description}\" ${3:OFF})", "option definition");
        suggestions->AddSnippet("cmake_min", "cmake_minimum_required(VERSION ${1:3.16})", "cmake minimum version");
        suggestions->AddSnippet("project", "project(${1:name}\n\tVERSION ${2:1.0.0}\n\tLANGUAGES ${3:CXX}\n)", "project definition");
    }

    if (language == SyntaxLanguage::Lua || language == SyntaxLanguage::Cpp) {
        addEngineAPISuggestions();
    }
}

void CustomTextEditor::addEngineAPISuggestions() {
    if (!suggestions) return;

    static const auto& apiSymbols = getEngineAPISymbols();

    static const std::unordered_map<std::string, SuggestionKind> kindMap = {
        {"Class", SuggestionKind::Class},
        {"Enum", SuggestionKind::Enum},
        {"EnumMember", SuggestionKind::EnumMember},
        {"Method", SuggestionKind::Method},
        {"CppMethod", SuggestionKind::Method},
        {"Function", SuggestionKind::Function},
        {"Property", SuggestionKind::Property},
        {"Constant", SuggestionKind::Constant},
        {"Variable", SuggestionKind::Variable},
    };

    // "CppMethod" is unbound in Lua: suggesting it there yields a nil call at runtime.
    const bool isLua = (language == SyntaxLanguage::Lua);

    // In C++ the engine's top-level classes and enums are reached through "quantum::"
    if (!isLua) {
        suggestions->AddSymbol("quantum", SuggestionKind::Module, "namespace quantum");
    }

    for (const auto& sym : apiSymbols) {
        if (isEngineApiConstructorSymbol(sym)) continue;
        if (isLua && sym.kind && std::string(sym.kind) == "CppMethod") continue;

        SuggestionKind sk = SuggestionKind::Variable;
        auto it = kindMap.find(sym.kind);
        if (it != kindMap.end()) sk = it->second;
        const bool topLevel = !sym.parent || !sym.parent[0];
        suggestions->AddSymbol(sym.name, sk, sym.detail, sym.parent ? sym.parent : "", "",
                               (topLevel && !isLua) ? "quantum" : "");

        if (sk == SuggestionKind::Class && sym.detail) {
            suggestions->SetClassParent(sym.name, baseClassFromDetail(sym.detail));
        }
    }

    // The script events are hand-written statics in engine/core/Engine.h, so no
    // generator lists them and this is the list. Lua assigns Engine.onX = f, C++
    // subscribes with REGISTER_ENGINE_EVENT(onX); both want the same names.
    static const char* const engineEvents[] = {
        "onViewLoaded", "onViewChanged", "onViewDestroyed", "onDraw", "onPostDraw",
        "onUpdate", "onFixedUpdate", "onPostUpdate", "onPause", "onResume", "onShutdown",
        "onSceneLoaded", "onTouchStart", "onTouchEnd", "onTouchMove", "onTouchCancel",
        "onMouseDown", "onMouseUp", "onMouseScroll", "onMouseMove", "onMouseEnter",
        "onMouseLeave", "onKeyDown", "onKeyUp", "onCharInput", "onGamepadConnect",
        "onGamepadDisconnect", "onGamepadButtonDown", "onGamepadButtonUp", "onGamepadAxisMove",
    };
    for (const char* event : engineEvents) {
        suggestions->AddSymbol(event, SuggestionKind::Field,
                               std::string("Engine.") + event + " event handler", "Engine");
    }

    if (!isLua) {
        // The two macros a game script needs and no header declares as a symbol
        suggestions->AddSymbol("DPROPERTY", SuggestionKind::Function,
                               "DPROPERTY(Type, name) exposed script property");
        suggestions->AddSymbol("REGISTER_ENGINE_EVENT", SuggestionKind::Function,
                               "REGISTER_ENGINE_EVENT(onUpdate) subscribes an event handler");
    }
}

void CustomTextEditor::SetLanguage(SyntaxLanguage lang) {
    if (language != lang) {
        language = lang;
        initializeLanguage();
        initializeSuggestions();
    }
}

const char* CustomTextEditor::GetLanguageName() const {
    switch (language) {
        case SyntaxLanguage::Cpp: return "C++";
        case SyntaxLanguage::Lua: return "Lua";
        case SyntaxLanguage::CMake: return "CMake";
        default: return "Plain Text";
    }
}

void CustomTextEditor::setLinesFromText(const std::string& text) {
    splitLines(text, lines);
}

void CustomTextEditor::SetText(const std::string& text) {
    setLinesFromText(text);
    tokenizeAll();

    cursors.clear();
    cursors.push_back(Cursor());
    primaryCursor = 0;

    scrollX = 0;
    scrollY = 0;

    undoBuffer.clear();
    undoIndex = 0;
}

std::string CustomTextEditor::GetText() const {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i < lines.size() - 1) {
            result += '\n';
        }
    }
    return result;
}

void CustomTextEditor::SetCursorPosition(int line, int column) {
    cursors.clear();
    Cursor cursor;
    cursor.position = clampPosition(TextPosition(line, column));
    cursor.selection.start = cursor.position;
    cursor.selection.end = cursor.position;
    cursors.push_back(cursor);
    primaryCursor = 0;
}

void CustomTextEditor::GetCursorPosition(int& line, int& column) const {
    if (!cursors.empty()) {
        line = cursors[primaryCursor].position.line;
        column = cursors[primaryCursor].position.column;
    } else {
        line = 0;
        column = 0;
    }
}

void CustomTextEditor::GetCursorDisplayPosition(int& line, int& column) const {
    GetCursorPosition(line, column);
    column = byteOffsetToVisualColumn(line, column);
}

bool CustomTextEditor::HasSelection() const {
    for (const auto& cursor : cursors) {
        if (!cursor.selection.isEmpty()) {
            return true;
        }
    }
    return false;
}

std::string CustomTextEditor::GetSelectedText() const {
    std::string result;
    bool first = true;
    for (const auto& cursor : cursors) {
        if (!cursor.selection.isEmpty()) {
            if (!first) result += '\n';
            result += getRange(cursor.selection.getMin(), cursor.selection.getMax());
            first = false;
        }
    }
    return result;
}

void CustomTextEditor::SelectAll() {
    cursors.clear();
    Cursor cursor;
    cursor.selection.start = TextPosition(0, 0);
    cursor.selection.end = TextPosition(static_cast<int>(lines.size()) - 1, 
                                         static_cast<int>(lines.back().size()));
    cursor.position = cursor.selection.end;
    cursors.push_back(cursor);
    primaryCursor = 0;
}

void CustomTextEditor::ClearSelection() {
    for (auto& cursor : cursors) {
        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }
}

void CustomTextEditor::AddCursor(int line, int column) {
    Cursor cursor;
    cursor.position = clampPosition(TextPosition(line, column));
    cursor.selection.start = cursor.position;
    cursor.selection.end = cursor.position;
    cursors.push_back(cursor);
}

void CustomTextEditor::ClearExtraCursors() {
    if (cursors.size() > 1) {
        Cursor primary = cursors[primaryCursor];
        cursors.clear();
        cursors.push_back(primary);
        primaryCursor = 0;
    }
}

void CustomTextEditor::tokenizeText(const std::string& line, OpenToken& open, std::vector<Token>& tokens) const {
    tokens.clear();
    if (line.empty()) return;

    int i = 0;
    int len = static_cast<int>(line.size());

    while (i < len) {
        if (std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
            continue;
        }

        // A comment or string that can span lines, from the line before or starting here.
        // It goes before line comments, as Lua's --[[ also starts one
        int openLength = 0;
        if (open.type == TokenType::Default) openLength = multiLineTokenAt(line, i, open);
        if (open.type != TokenType::Default) {
            int start = i;
            TokenType type = open.type;
            i = scanOpenToken(line, i + openLength, open);
            tokens.push_back({start, i - start, type});
            continue;
        }

        // Preprocessor (C/C++)
        if (!languageDef.preprocessorPrefix.empty() && 
            line.compare(i, languageDef.preprocessorPrefix.size(), languageDef.preprocessorPrefix) == 0) {
            int start = i;
            i = len;
            tokens.push_back({start, i - start, TokenType::Preprocessor});
            continue;
        }

        // Single-line comment
        if (!languageDef.singleLineComment.empty() &&
            line.compare(i, languageDef.singleLineComment.size(), languageDef.singleLineComment) == 0) {
            tokens.push_back({i, len - i, TokenType::Comment});
            break;
        }

        // Numbers
        if (std::isdigit(static_cast<unsigned char>(line[i])) || (line[i] == '.' && i + 1 < len && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            int start = i;
            bool hasDecimal = false;
            bool hasExponent = false;

            // Hex prefix
            if (line[i] == '0' && i + 1 < len && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                i += 2;
                while (i < len && std::isxdigit(static_cast<unsigned char>(line[i]))) ++i;
            } else {
                while (i < len) {
                    if (std::isdigit(static_cast<unsigned char>(line[i]))) {
                        ++i;
                    } else if (line[i] == '.' && !hasDecimal && !hasExponent) {
                        hasDecimal = true;
                        ++i;
                    } else if ((line[i] == 'e' || line[i] == 'E') && !hasExponent) {
                        hasExponent = true;
                        ++i;
                        if (i < len && (line[i] == '+' || line[i] == '-')) ++i;
                    } else if (line[i] == 'f' || line[i] == 'F' || line[i] == 'l' || line[i] == 'L' ||
                               line[i] == 'u' || line[i] == 'U') {
                        ++i;
                    } else {
                        break;
                    }
                }
            }
            tokens.push_back({start, i - start, TokenType::Number});
            continue;
        }

        // Identifiers and keywords
        if (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_') {
            int start = i;
            while (i < len && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) ++i;

            std::string word = line.substr(start, i - start);
            TokenType type = classifyWord(word);

            // A name followed by '(' is a call
            int j = i;
            while (j < len && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            if (j < len && line[j] == '(' && type == TokenType::Identifier) {
                type = TokenType::Function;
            }

            tokens.push_back({start, i - start, type});
            continue;
        }

        // Operators and punctuation
        const char* operators = "+-*/%=<>!&|^~?:";
        const char* punctuation = "(){}[],.;";

        if (std::strchr(operators, line[i])) {
            int start = i++;
            while (i < len && std::strchr(operators, line[i])) ++i;
            tokens.push_back({start, i - start, TokenType::Operator});
            continue;
        }

        if (std::strchr(punctuation, line[i])) {
            tokens.push_back({i, 1, TokenType::Punctuation});
            ++i;
            continue;
        }

        // Non-ASCII runs stay whole so every token is valid UTF-8
        if (static_cast<unsigned char>(line[i]) >= 0x80) {
            int start = i;
            while (i < len && static_cast<unsigned char>(line[i]) >= 0x80) ++i;
            tokens.push_back({start, i - start, TokenType::Default});
            continue;
        }

        tokens.push_back({i, 1, TokenType::Default});
        ++i;
    }
}

int CustomTextEditor::multiLineTokenAt(const std::string& line, int i, OpenToken& token) const {
    const std::string& commentStart = languageDef.multiLineCommentStart;
    if (!commentStart.empty() && line.compare(i, commentStart.size(), commentStart) == 0) {
        token = {TokenType::MultiLineComment, languageDef.multiLineCommentEnd};
        return static_cast<int>(commentStart.size());
    }

    // Quoted strings; CMake only quotes with "
    if (line[i] == '"' || (line[i] == '\'' && language != SyntaxLanguage::CMake)) {
        token = {TokenType::String, std::string(1, line[i])};
        return 1;
    }

    if (language == SyntaxLanguage::Cpp) {
        // Raw strings, R"delim(...)delim", maybe with a u8, u, U or L prefix
        size_t r = i;
        if (line.compare(r, 2, "u8") == 0) {
            r += 2;
        } else if (line[r] == 'u' || line[r] == 'U' || line[r] == 'L') {
            ++r;
        }
        if (line.compare(r, 2, "R\"") != 0) return 0;
        size_t paren = line.find('(', r + 2);
        if (paren == std::string::npos || paren - r - 2 > 16) return 0;
        std::string delimiter = line.substr(r + 2, paren - r - 2);
        if (delimiter.find_first_of(" )\\\t\v\f") != std::string::npos) return 0;
        token = {TokenType::String, ")" + delimiter + "\""};
        return static_cast<int>(paren) + 1 - i;
    }

    if (language == SyntaxLanguage::Lua || language == SyntaxLanguage::CMake) {
        // Long brackets, [[ ]], [=[ ]=] and so on, make strings, or comments after the comment marker
        const std::string& marker = languageDef.singleLineComment;
        size_t first = (line.compare(i, marker.size(), marker) == 0) ? i + marker.size() : i;
        size_t second = first + 1;
        while (second < line.size() && line[second] == '=') ++second;
        if (first >= line.size() || line[first] != '[' || second >= line.size() || line[second] != '[') return 0;
        TokenType type = (first > static_cast<size_t>(i)) ? TokenType::MultiLineComment : TokenType::String;
        token = {type, "]" + std::string(second - first - 1, '=') + "]"};
        return static_cast<int>(second) + 1 - i;
    }
    return 0;
}

int CustomTextEditor::scanOpenToken(const std::string& line, int i, OpenToken& open) const {
    int len = static_cast<int>(line.size());
    if (open.end != "\"" && open.end != "'") {
        size_t endPos = line.find(open.end, i);
        if (endPos == std::string::npos) return len;
        i = static_cast<int>(endPos + open.end.size());
        open = OpenToken();
        return i;
    }

    // Quoted strings skip escapes, and run on past the line end after a backslash, Lua's \z, or in CMake
    bool skipping = false;
    while (i < len) {
        if (line[i] == open.end[0]) {
            open = OpenToken();
            return i + 1;
        }
        if (line[i] == '\\') {
            if (i + 1 == len) return len;
            skipping = (language == SyntaxLanguage::Lua && line[i + 1] == 'z');
            i += 2;
        } else {
            if (!std::isspace(static_cast<unsigned char>(line[i]))) skipping = false;
            ++i;
        }
    }
    if (!skipping && language != SyntaxLanguage::CMake) open = OpenToken();
    return len;
}

void CustomTextEditor::tokenizeAll() {
    // Every edit retokenizes, so this is also where cached widths and the diff expire
    maxLineWidthDirty = true;
    if (hasDiffBase) diffDirty = true;
    lineTokens.clear();
    lineTokens.resize(lines.size());
    OpenToken open;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        tokenizeText(lines[i], open, lineTokens[i]);
    }
}

TokenType CustomTextEditor::classifyWord(const std::string& word) const {
    if (languageDef.keywords.count(word)) return TokenType::Keyword;
    if (languageDef.types.count(word)) return TokenType::Type;
    if (languageDef.builtinFunctions.count(word)) return TokenType::Function;
    return TokenType::Identifier;
}

// -- Multi-cursor position adjustment helpers --

static TextPosition adjustPosAfterDelete(const TextPosition& pos, const TextPosition& delStart, const TextPosition& delEnd) {
    if (pos <= delStart) return pos;
    if (pos <= delEnd) return delStart;
    if (pos.line == delEnd.line) {
        return TextPosition(delStart.line, delStart.column + (pos.column - delEnd.column));
    }
    return TextPosition(pos.line - (delEnd.line - delStart.line), pos.column);
}

static TextPosition adjustPosAfterInsert(const TextPosition& pos, const TextPosition& insertPos, const TextPosition& afterPos, int removed = 0) {
    if (pos < insertPos) return pos;
    int deltaLines = afterPos.line - insertPos.line;
    if (pos.line == insertPos.line) {
        // Positions in the whitespace that auto-indent removed after insertPos collapse onto afterPos
        return TextPosition(pos.line + deltaLines, afterPos.column + std::max(0, pos.column - insertPos.column - removed));
    }
    return TextPosition(pos.line + deltaLines, pos.column);
}

static void adjustCursorAfterDelete(Cursor& cursor, const TextPosition& delStart, const TextPosition& delEnd) {
    cursor.position = adjustPosAfterDelete(cursor.position, delStart, delEnd);
    cursor.selection.start = adjustPosAfterDelete(cursor.selection.start, delStart, delEnd);
    cursor.selection.end = adjustPosAfterDelete(cursor.selection.end, delStart, delEnd);
}

static void adjustCursorAfterInsert(Cursor& cursor, const TextPosition& insertPos, const TextPosition& afterPos, int removed = 0) {
    cursor.position = adjustPosAfterInsert(cursor.position, insertPos, afterPos, removed);
    cursor.selection.start = adjustPosAfterInsert(cursor.selection.start, insertPos, afterPos, removed);
    cursor.selection.end = adjustPosAfterInsert(cursor.selection.end, insertPos, afterPos, removed);
}

void CustomTextEditor::InsertText(const std::string& text, bool allowAutoIndent) {
    if (readOnly || text.empty()) return;

    addUndoRecord();

    std::vector<size_t> order = cursorEditOrder();

    for (size_t idx = 0; idx < order.size(); ++idx) {
        auto& cursor = cursors[order[idx]];

        if (!cursor.selection.isEmpty()) {
            TextPosition delStart = cursor.selection.getMin();
            TextPosition delEnd = cursor.selection.getMax();
            deleteRange(delStart, delEnd);
            cursor.position = delStart;
            cursor.selection.start = cursor.position;
            cursor.selection.end = cursor.position;

            // Adjust all subsequent cursors for the deletion
            for (size_t j = idx + 1; j < order.size(); ++j) {
                adjustCursorAfterDelete(cursors[order[j]], delStart, delEnd);
            }
        }

        TextPosition insertPos = cursor.position;
        int removed = 0;
        TextPosition afterPos = insertTextAtCursor(cursor, text, allowAutoIndent, &removed);

        // Adjust all subsequent cursors for the insertion
        for (size_t j = idx + 1; j < order.size(); ++j) {
            adjustCursorAfterInsert(cursors[order[j]], insertPos, afterPos, removed);
        }
        if (showParamHint) {
            paramHintAnchor = adjustPosAfterInsert(paramHintAnchor, insertPos, afterPos, removed);
        }
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::SetTextUndoable(const std::string& text) {
    if (readOnly) return;

    if (showParamHint) closeParamHint();
    CloseAutoComplete();

    addUndoRecord();
    setLinesFromText(text);
    ensureValidCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

TextPosition CustomTextEditor::insertTextAtCursor(Cursor& cursor, const std::string& text, bool allowAutoIndent, int* removed) {
    int line = cursor.position.line;
    int col = cursor.position.column;
    // Set when a block is broken open and the text after the cursor goes to the next line
    int tailColumn = -1;
    int stripped = 0;

    for (char c : text) {
        if (c == '\n') {
            std::string currentLine = lines[line];
            std::string before = currentLine.substr(0, col);
            std::string after = currentLine.substr(col);

            lines[line] = before;
            lines.insert(lines.begin() + line + 1, after);

            ++line;
            col = 0;

            if (autoIndent && allowAutoIndent) {
                int indent = getLineIndent(line - 1);
                OpenToken state = lineStartState(line - 1);
                std::vector<Token> tokens;
                tokenizeText(before, state, tokens);

                // Only code gets block indents. With just whitespace before the cursor, or inside
                // a comment or string that spans lines, the line moves down as it is
                if (!tokens.empty() && state.type == TokenType::Default) {
                    int blanks = static_cast<int>(std::min(after.find_first_not_of(" \t"), after.size()));
                    after.erase(0, blanks);
                    stripped += blanks;

                    bool opens = opensBlock(before, tokens);
                    bool closes = closesBlock(after);
                    if (opens && closes) {
                        // Break the block open: an indented line for the content and one for the closer
                        lines.insert(lines.begin() + line + 1, getIndentString(indent) + after);
                        after.clear();
                        tailColumn = indent;
                    }

                    if (opens) {
                        indent += tabSize;
                    } else if (closes) {
                        indent = std::max(0, indent - tabSize);
                    }
                }

                lines[line] = getIndentString(indent) + after;
                col = indent;
            }
        } else if (c == '\t') {
            int visualCol = byteOffsetToVisualColumn(line, col);
            int spacesToInsert = tabSize - (visualCol % tabSize);
            std::string spaces(spacesToInsert, ' ');
            lines[line].insert(col, spaces);
            col += spacesToInsert;
        } else {
            lines[line].insert(col, 1, c);
            ++col;
        }
    }

    cursor.position.line = line;
    cursor.position.column = col;
    cursor.selection.start = cursor.position;
    cursor.selection.end = cursor.position;
    cursor.preferredColumn = byteOffsetToVisualColumn(line, col);

    if (removed) *removed = stripped;
    return (tailColumn >= 0) ? TextPosition(line + 1, tailColumn) : cursor.position;
}

void CustomTextEditor::DeleteSelection() {
    if (readOnly) return;

    addUndoRecord();

    std::vector<size_t> order = cursorEditOrder();

    for (size_t idx = 0; idx < order.size(); ++idx) {
        auto& cursor = cursors[order[idx]];
        if (!cursor.selection.isEmpty()) {
            TextPosition delStart = cursor.selection.getMin();
            TextPosition delEnd = cursor.selection.getMax();
            deleteRange(delStart, delEnd);
            cursor.position = delStart;
            cursor.selection.start = cursor.position;
            cursor.selection.end = cursor.position;

            for (size_t j = idx + 1; j < order.size(); ++j) {
                adjustCursorAfterDelete(cursors[order[j]], delStart, delEnd);
            }
        }
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

// True when the caret sits exactly between two quotes that close each other; the pair is
// removed and the caret left where its opening half was. Quotes are ASCII, so a byte
// comparison cannot split a UTF-8 sequence: a continuation byte matches nothing here.
bool CustomTextEditor::deleteEmptyPair(Cursor& cursor) {
    const int line = cursor.position.line;
    const int column = cursor.position.column;
    if (line < 0 || line >= static_cast<int>(lines.size())) return false;
    if (column <= 0 || column >= static_cast<int>(lines[line].size())) return false;

    const std::string& text = lines[line];
    const char opening = text[static_cast<size_t>(column) - 1];
    const char closing = text[static_cast<size_t>(column)];
    const char expected = quotePair(opening);
    if (expected == 0 || expected != closing) return false;

    lines[line].erase(static_cast<size_t>(column) - 1, 2);
    cursor.position.column = column - 1;
    return true;
}

void CustomTextEditor::Backspace() {
    if (readOnly) return;

    addUndoRecord();

    std::vector<size_t> order = cursorEditOrder();

    for (size_t idx = 0; idx < order.size(); ++idx) {
        auto& cursor = cursors[order[idx]];

        TextPosition delStart, delEnd;
        bool didDelete = true;

        if (!cursor.selection.isEmpty()) {
            delStart = cursor.selection.getMin();
            delEnd = cursor.selection.getMax();
            deleteRange(delStart, delEnd);
            cursor.position = delStart;
        } else if (deleteEmptyPair(cursor)) {
            delStart = cursor.position;
            delEnd = TextPosition(delStart.line, delStart.column + 2);
        } else if (cursor.position.column > 0) {
            delEnd = cursor.position;
            cursor.position.column = prevUtf8Column(cursor.position.line, cursor.position.column);
            delStart = cursor.position;
            lines[cursor.position.line].erase(delStart.column, delEnd.column - delStart.column);
        } else if (cursor.position.line > 0) {
            delEnd = cursor.position;
            int prevLineLen = static_cast<int>(lines[cursor.position.line - 1].size());
            lines[cursor.position.line - 1] += lines[cursor.position.line];
            lines.erase(lines.begin() + cursor.position.line);
            cursor.position.line--;
            cursor.position.column = prevLineLen;
            delStart = cursor.position;
        } else {
            didDelete = false;
        }

        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;

        if (didDelete) {
            for (size_t j = idx + 1; j < order.size(); ++j) {
                adjustCursorAfterDelete(cursors[order[j]], delStart, delEnd);
            }
            if (showParamHint && cursor.selection.isEmpty()) {
                paramHintAnchor = adjustPosAfterDelete(paramHintAnchor, delStart, delEnd);
            }
        }
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::Delete() {
    if (readOnly) return;

    addUndoRecord();

    std::vector<size_t> order = cursorEditOrder();

    for (size_t idx = 0; idx < order.size(); ++idx) {
        auto& cursor = cursors[order[idx]];

        TextPosition delStart, delEnd;
        bool didDelete = true;

        if (!cursor.selection.isEmpty()) {
            delStart = cursor.selection.getMin();
            delEnd = cursor.selection.getMax();
            deleteRange(delStart, delEnd);
            cursor.position = delStart;
        } else {
            int lineLen = static_cast<int>(lines[cursor.position.line].size());
            if (cursor.position.column < lineLen) {
                delStart = cursor.position;
                delEnd = TextPosition(cursor.position.line, nextUtf8Column(cursor.position.line, cursor.position.column));
                lines[cursor.position.line].erase(delStart.column, delEnd.column - delStart.column);
            } else if (cursor.position.line < static_cast<int>(lines.size()) - 1) {
                delStart = cursor.position;
                delEnd = TextPosition(cursor.position.line + 1, 0);
                lines[cursor.position.line] += lines[cursor.position.line + 1];
                lines.erase(lines.begin() + cursor.position.line + 1);
            } else {
                didDelete = false;
            }
        }

        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;

        if (didDelete) {
            for (size_t j = idx + 1; j < order.size(); ++j) {
                adjustCursorAfterDelete(cursors[order[j]], delStart, delEnd);
            }
            if (showParamHint && cursor.selection.isEmpty()) {
                paramHintAnchor = adjustPosAfterDelete(paramHintAnchor, delStart, delEnd);
            }
        }
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::DeleteWordLeft() {
    if (readOnly) return;

    addUndoRecord();

    std::vector<size_t> order = cursorEditOrder();

    for (size_t idx = 0; idx < order.size(); ++idx) {
        auto& cursor = cursors[order[idx]];

        TextPosition delStart, delEnd;

        if (!cursor.selection.isEmpty()) {
            delStart = cursor.selection.getMin();
            delEnd = cursor.selection.getMax();
        } else {
            delStart = findDeleteWordStart(cursor.position);
            delEnd = cursor.position;
        }

        if (delStart < delEnd) {
            deleteRange(delStart, delEnd);
            cursor.position = delStart;

            for (size_t j = idx + 1; j < order.size(); ++j) {
                adjustCursorAfterDelete(cursors[order[j]], delStart, delEnd);
            }
        }

        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::DeleteWordRight() {
    if (readOnly) return;

    addUndoRecord();

    std::vector<size_t> order = cursorEditOrder();

    for (size_t idx = 0; idx < order.size(); ++idx) {
        auto& cursor = cursors[order[idx]];

        TextPosition delStart, delEnd;

        if (!cursor.selection.isEmpty()) {
            delStart = cursor.selection.getMin();
            delEnd = cursor.selection.getMax();
        } else {
            delStart = cursor.position;
            delEnd = findDeleteWordEnd(cursor.position);
        }

        if (delStart < delEnd) {
            deleteRange(delStart, delEnd);
            cursor.position = delStart;

            for (size_t j = idx + 1; j < order.size(); ++j) {
                adjustCursorAfterDelete(cursors[order[j]], delStart, delEnd);
            }
        }

        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::deleteRange(const TextPosition& start, const TextPosition& end) {
    if (start == end) return;

    TextPosition minPos = start < end ? start : end;
    TextPosition maxPos = start < end ? end : start;

    if (minPos.line < 0 || minPos.line >= static_cast<int>(lines.size())) return;
    if (maxPos.line < 0 || maxPos.line >= static_cast<int>(lines.size())) return;

    minPos.column = std::clamp(minPos.column, 0, static_cast<int>(lines[minPos.line].size()));
    maxPos.column = std::clamp(maxPos.column, 0, static_cast<int>(lines[maxPos.line].size()));

    if (minPos.line == maxPos.line) {
        if (minPos.column < maxPos.column) {
            lines[minPos.line].erase(minPos.column, maxPos.column - minPos.column);
        }
    } else {
        std::string before = lines[minPos.line].substr(0, minPos.column);
        std::string after = lines[maxPos.line].substr(maxPos.column);

        lines[minPos.line] = before + after;
        lines.erase(lines.begin() + minPos.line + 1, lines.begin() + maxPos.line + 1);
    }

    if (showParamHint) {
        paramHintAnchor = adjustPosAfterDelete(paramHintAnchor, minPos, maxPos);
    }
}

std::string CustomTextEditor::getRange(const TextPosition& start, const TextPosition& end) const {
    if (start == end) return "";

    TextPosition minPos = start < end ? start : end;
    TextPosition maxPos = start < end ? end : start;

    if (minPos.line < 0 || minPos.line >= static_cast<int>(lines.size())) return "";
    if (maxPos.line < 0 || maxPos.line >= static_cast<int>(lines.size())) return "";

    minPos.column = std::clamp(minPos.column, 0, static_cast<int>(lines[minPos.line].size()));
    maxPos.column = std::clamp(maxPos.column, 0, static_cast<int>(lines[maxPos.line].size()));

    if (minPos.line == maxPos.line) {
        if (minPos.column >= maxPos.column) return "";
        return lines[minPos.line].substr(minPos.column, maxPos.column - minPos.column);
    }

    std::string result = lines[minPos.line].substr(minPos.column) + "\n";
    for (int i = minPos.line + 1; i < maxPos.line; ++i) {
        result += lines[i] + "\n";
    }
    result += lines[maxPos.line].substr(0, maxPos.column);

    return result;
}

void CustomTextEditor::Undo(int steps) {
    if (!CanUndo()) return;

    if (showParamHint) closeParamHint();

    for (int i = 0; i < steps && undoIndex > 0; ++i) {
        --undoIndex;
        if (undoIndex < static_cast<int>(undoBuffer.size())) {
            undoBuffer[undoIndex].afterText = GetText();
            undoBuffer[undoIndex].afterCursors = cursors;

            setLinesFromText(undoBuffer[undoIndex].beforeText);
            tokenizeAll();

            if (!undoBuffer[undoIndex].beforeCursors.empty()) {
                cursors = undoBuffer[undoIndex].beforeCursors;
            }
            ensureValidCursors();
        }
    }
}

void CustomTextEditor::Redo(int steps) {
    if (!CanRedo()) return;

    if (showParamHint) closeParamHint();

    for (int i = 0; i < steps && undoIndex < static_cast<int>(undoBuffer.size()); ++i) {
        if (!undoBuffer[undoIndex].afterText.empty()) {
            setLinesFromText(undoBuffer[undoIndex].afterText);
            tokenizeAll();

            if (!undoBuffer[undoIndex].afterCursors.empty()) {
                cursors = undoBuffer[undoIndex].afterCursors;
            }
            ensureValidCursors();
        }
        ++undoIndex;
    }
}

void CustomTextEditor::addUndoRecord() {
    // Remove any redo history
    if (undoIndex < static_cast<int>(undoBuffer.size())) {
        undoBuffer.erase(undoBuffer.begin() + undoIndex, undoBuffer.end());
    }

    UndoRecord record;
    record.beforeText = GetText();
    record.afterText = "";  // Will be filled by finalizeUndoRecord
    record.beforeCursors = cursors;
    record.timestamp = std::chrono::steady_clock::now();

    // Try to merge with previous record if it's recent
    if (!undoBuffer.empty()) {
        auto& last = undoBuffer.back();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            record.timestamp - last.timestamp).count();
        if (elapsed < 300 && !last.isMerged) {
            // Keep the previous record and its beforeText, finalize updates its afterText
            last.isMerged = true;
            return;
        }
    }

    undoBuffer.push_back(record);
    ++undoIndex;

    while (undoBuffer.size() > maxUndoSteps) {
        undoBuffer.erase(undoBuffer.begin());
        if (undoIndex > 0) --undoIndex;
    }
}

void CustomTextEditor::finalizeUndoRecord() {
    if (!undoBuffer.empty() && undoIndex > 0) {
        undoBuffer[undoIndex - 1].afterText = GetText();
        undoBuffer[undoIndex - 1].afterCursors = cursors;
    }
}

void CustomTextEditor::Copy() {
    std::string text = GetSelectedText();
    if (text.empty()) {
        if (!cursors.empty()) {
            int line = cursors[primaryCursor].position.line;
            text = lines[line] + "\n";
        }
    }

    if (!text.empty()) {
        ImGui::SetClipboardText(text.c_str());
    }
}

void CustomTextEditor::Cut() {
    if (readOnly) return;

    if (HasSelection()) {
        Copy();
        DeleteSelection();
    } else {
        // Cut entire line (VSCode behavior)
        Copy();
        if (!cursors.empty()) {
            addUndoRecord();
            int line = cursors[primaryCursor].position.line;
            if (lines.size() > 1) {
                lines.erase(lines.begin() + line);
                if (line >= static_cast<int>(lines.size())) line = static_cast<int>(lines.size()) - 1;
                cursors[primaryCursor].position = TextPosition(line, 0);
            } else {
                lines[0].clear();
                cursors[primaryCursor].position = TextPosition(0, 0);
            }
            cursors[primaryCursor].selection.start = cursors[primaryCursor].position;
            cursors[primaryCursor].selection.end = cursors[primaryCursor].position;
            tokenizeAll();
            finalizeUndoRecord();
        }
    }
}

void CustomTextEditor::Paste() {
    if (readOnly) return;

    const char* clipboard = ImGui::GetClipboardText();
    if (clipboard) {
        InsertText(clipboard, false);
    }
}

void CustomTextEditor::SetSearchText(const std::string& text) {
    searchText = text;
    updateSearchResults();
}

void CustomTextEditor::OpenFind() {
    showFindDialog = true;
    if (HasSelection()) {
        std::string selection = GetSelectedText();
        // Only use single-line selections
        if (selection.find('\n') == std::string::npos && selection.size() < sizeof(findInputBuffer) - 1) {
            strncpy(findInputBuffer, selection.c_str(), sizeof(findInputBuffer) - 1);
            findInputBuffer[sizeof(findInputBuffer) - 1] = '\0';
            SetSearchText(selection);
        }
    }
}

void CustomTextEditor::CloseFind() {
    showFindDialog = false;
    searchText.clear();
    searchResults.clear();
    findError.clear();
    currentSearchResult = -1;
}

void CustomTextEditor::SetFindRegex(bool on) {
    if (findUseRegex == on) return;
    findUseRegex = on;
    updateSearchResults();
}

void CustomTextEditor::SetFindWholeWord(bool on) {
    if (findWholeWord == on) return;
    findWholeWord = on;
    updateSearchResults();
}

void CustomTextEditor::SetFindInSelection(bool on) {
    if (findInSelection == on) return;
    findInSelection = on;
    updateSearchResults();
}

CustomTextEditor::FindRequest CustomTextEditor::findRequest() const {
    FindRequest request;
    request.query = searchText;
    request.useRegex = findUseRegex;
    request.caseSensitive = findCaseSensitive;
    request.wholeWord = findWholeWord;
    request.inSelection = findInSelection;
    request.firstLine = -1;
    request.lastLine = -1;
    if (findInSelection && !cursors.empty() && !cursors[primaryCursor].selection.isEmpty()) {
        const Cursor& cursor = cursors[primaryCursor];
        request.firstLine = cursor.selection.getMin().line;
        // A selection that stops at column 0 touched that line with its newline only
        request.lastLine = cursor.selection.getMax().column > 0
            ? cursor.selection.getMax().line : cursor.selection.getMax().line - 1;
    }
    return request;
}

script::SearchOptions CustomTextEditor::findOptionsFromRequest(const FindRequest& request) {
    script::SearchOptions options;
    options.query = request.query;
    options.useRegex = request.useRegex;
    options.caseSensitive = request.caseSensitive;
    options.wholeWord = request.wholeWord;
    options.firstLine = request.firstLine;
    options.lastLine = request.lastLine;
    // A document search wants every hit, not the first 400 of a project scan
    options.maxMatchesPerFile = 20000;
    options.maxTotalMatches = 20000;
    return options;
}

void CustomTextEditor::finishLineEdit() {
    tokenizeAll();
    ensureValidCursors();
    maxLineWidthDirty = true;
    finalizeUndoRecord();
    updateSearchResults();
    scrollToCursor();
}

void CustomTextEditor::updateSearchResults() {
    searchResults.clear();
    currentSearchResult = -1;
    findError.clear();

    const FindRequest request = findRequest();
    if (request.query.empty()) return;

    script::SearchOptions options = findOptionsFromRequest(request);
    script::ScriptRegex regex;
    if (!script::buildSearchRegex(options, regex, findError)) return;

    script::SearchFileResult found;
    script::searchLines(lines, options, regex, found);
    searchResults.reserve(found.matches.size());
    for (const script::SearchMatch& match : found.matches) {
        searchResults.push_back(FindSpan{ match.line, match.column, match.endColumn });
    }
}

void CustomTextEditor::selectSearchResult(int index, bool caretAtEnd) {
    if (index < 0 || index >= static_cast<int>(searchResults.size())) return;
    const FindSpan& hit = searchResults[(size_t)index];

    currentSearchResult = index;
    SetCursorPosition(hit.line, hit.column);
    cursors[primaryCursor].selection.start = TextPosition(hit.line, hit.column);
    cursors[primaryCursor].selection.end = TextPosition(hit.line, hit.endColumn);
    cursors[primaryCursor].position = caretAtEnd ? cursors[primaryCursor].selection.end
                                                 : cursors[primaryCursor].selection.start;
    pendingScrollToCursor = true;
}

bool CustomTextEditor::FindNext() {
    if (searchResults.empty()) return false;

    const int count = static_cast<int>(searchResults.size());
    const TextPosition pivot = !cursors[primaryCursor].selection.isEmpty()
        ? cursors[primaryCursor].selection.getMax()
        : cursors[primaryCursor].position;

    for (int i = 0; i < count; ++i) {
        // The selected hit is skipped by index: a zero-width match starts exactly where
        // the selection ends, and comparing positions alone would land on it again.
        if (i == currentSearchResult) continue;
        const FindSpan& hit = searchResults[(size_t)i];
        if (hit.line > pivot.line || (hit.line == pivot.line && hit.column >= pivot.column)) {
            selectSearchResult(i, true);
            return true;
        }
    }

    selectSearchResult(0, true);   // wrap to the first
    return true;
}

bool CustomTextEditor::FindPrevious() {
    if (searchResults.empty()) return false;

    const int count = static_cast<int>(searchResults.size());
    const TextPosition pivot = !cursors[primaryCursor].selection.isEmpty()
        ? cursors[primaryCursor].selection.getMin()
        : cursors[primaryCursor].position;

    for (int i = count - 1; i >= 0; --i) {
        if (i == currentSearchResult) continue;
        const FindSpan& hit = searchResults[(size_t)i];
        if (hit.line < pivot.line || (hit.line == pivot.line && hit.column < pivot.column)) {
            selectSearchResult(i, false);
            return true;
        }
    }

    selectSearchResult(count - 1, false);   // wrap to the last
    return true;
}

bool CustomTextEditor::ReplaceNext() {
    if (readOnly || searchText.empty() || searchResults.empty()) return false;

    const int index = currentSearchResult >= 0 ? currentSearchResult : 0;
    const FindSpan& hit = searchResults[(size_t)index];
    const Cursor& cursor = cursors[primaryCursor];
    const TextPosition selMin = cursor.selection.getMin();
    const TextPosition selMax = cursor.selection.getMax();

    // "Replace" acts on the selected match only; anywhere else it just moves to the next
    if (selMin.line != hit.line || selMin.column != hit.column || selMax.column != hit.endColumn) {
        return FindNext();
    }

    const FindRequest request = findRequest();
    script::SearchOptions options = findOptionsFromRequest(request);
    script::ScriptRegex regex;
    if (!script::buildSearchRegex(options, regex, findError)) return false;

    // The pattern runs again on this one line so `$1` can splice the groups of this match
    script::RegexMatch match;
    regex.search(lines[(size_t)hit.line], static_cast<size_t>(hit.column), match);
    std::string replacement = script::expandReplacement(replaceInputBuffer, match,
                                                        lines[(size_t)hit.line], request.useRegex);

    // One replacement cannot be spread over several cursors, so the bar's cursor is kept
    if (cursors.size() > 1) {
        Cursor keep = cursors[primaryCursor];
        cursors.clear();
        cursors.push_back(keep);
        primaryCursor = 0;
    }

    if (replacement.empty()) {
        DeleteSelection();
    } else {
        InsertText(replacement, false);
    }
    updateSearchResults();
    return FindNext();
}

int CustomTextEditor::replaceAllRequest(const FindRequest& request, const std::string& replacement) {
    if (readOnly || request.query.empty()) return 0;

    script::SearchOptions options = findOptionsFromRequest(request);
    script::ScriptRegex regex;
    if (!script::buildSearchRegex(options, regex, findError)) return 0;

    std::vector<std::string> updated = lines;
    bool truncated = false;
    const int count = script::replaceLines(updated, options, regex, replacement, 0, truncated);
    if (count == 0) return 0;

    addUndoRecord();
    lines.swap(updated);
    tokenizeAll();
    ensureValidCursors();
    finalizeUndoRecord();
    maxLineWidthDirty = true;
    updateSearchResults();
    return count;
}

int CustomTextEditor::ReplaceAll(const std::string& find, const std::string& replace, bool caseSensitive) {
    FindRequest request;
    request.query = find;
    request.useRegex = false;
    request.caseSensitive = caseSensitive;
    request.wholeWord = false;
    request.inSelection = false;
    request.firstLine = -1;
    request.lastLine = -1;
    return replaceAllRequest(request, replace);
}

int CustomTextEditor::ReplaceAllFromFindBar() {
    return replaceAllRequest(findRequest(), std::string(replaceInputBuffer));
}

void CustomTextEditor::OpenGoToLine() {
    showGoToLine = true;
    goToLineRefocus = true;
    const int line = cursors.empty() ? 1 : cursors[primaryCursor].position.line + 1;
    snprintf(goToLineBuffer, sizeof(goToLineBuffer), "%d", line);
}

void CustomTextEditor::GoToLine(int line, int column) {
    if (lines.empty()) return;
    const int target = std::max(1, std::min(line, static_cast<int>(lines.size()))) - 1;
    const int col = std::max(1, column) - 1;
    SetCursorPosition(target, std::min(col, static_cast<int>(lines[(size_t)target].size())));
    scrollToCursor();
}

void CustomTextEditor::DeleteLines() {
    if (readOnly || cursors.empty()) return;
    const std::vector<int> targets = cursorLines();
    if (targets.empty()) return;

    addUndoRecord();
    // Erased from the back so the indices of the lines below stay valid
    for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
        lines.erase(lines.begin() + *it);
    }
    if (lines.empty()) lines.push_back(std::string());

    // Each cursor moves up by the number of deleted lines above it
    for (Cursor& cursor : cursors) {
        int shift = 0;
        for (int deleted : targets) {
            if (deleted < cursor.position.line) ++shift;
        }
        cursor.position.line = std::max(0, cursor.position.line - shift);
        cursor.position.column = 0;
        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }
    finishLineEdit();
}

void CustomTextEditor::DuplicateLines() {
    if (readOnly || cursors.empty()) return;
    const std::vector<int> targets = cursorLines();
    if (targets.empty()) return;

    addUndoRecord();
    for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
        lines.insert(lines.begin() + *it + 1, lines[(size_t)*it]);
    }

    for (Cursor& cursor : cursors) {
        int shift = 0;
        for (int source : targets) {
            if (source < cursor.position.line) ++shift;
        }
        cursor.position.line += shift;
        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }
    finishLineEdit();
}

void CustomTextEditor::MoveLines(int delta) {
    if (readOnly || cursors.empty() || lines.empty()) return;
    const std::vector<int> targets = cursorLines();
    if (targets.empty()) return;

    const int first = targets.front();
    const int last = targets.back();
    // A gap between two cursors moves with them, which is what the same command does in
    // VSCode: the block is the lines the cursors touch, not only those lines.
    if (first + delta < 0 || last + delta >= static_cast<int>(lines.size())) return;

    addUndoRecord();
    std::vector<std::string> block(lines.begin() + first, lines.begin() + last + 1);
    lines.erase(lines.begin() + first, lines.begin() + last + 1);
    lines.insert(lines.begin() + first + delta, block.begin(), block.end());

    for (Cursor& cursor : cursors) {
        if (cursor.position.line >= first && cursor.position.line <= last) {
            cursor.position.line += delta;
        }
        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }
    finishLineEdit();
}

void CustomTextEditor::SelectLines() {
    if (cursors.empty()) return;
    Cursor& cursor = cursors[primaryCursor];
    const int lastLine = static_cast<int>(lines.size()) - 1;

    const TextPosition min = cursor.selection.getMin();
    const TextPosition max = cursor.selection.getMax();

    // The end is the start of the line after the block, which is what carries the newline
    // of the last line selected; a partial selection snaps to the whole lines it touches.
    const int endLine = max.line + 1;

    cursor.selection.start = TextPosition(min.line, 0);
    cursor.selection.end = endLine > lastLine
        ? TextPosition(lastLine, static_cast<int>(lines[(size_t)lastLine].size()))
        : TextPosition(endLine, 0);
    cursor.position = cursor.selection.end;
    pendingScrollToCursor = true;
}

std::string CustomTextEditor::wordUnderCursor() const {
    if (cursors.empty() || showFindDialog || showAutoComplete) return std::string();
    const Cursor& cursor = cursors[primaryCursor];
    if (!cursor.selection.isEmpty()) return std::string();

    const int line = cursor.position.line;
    const int column = cursor.position.column;
    if (line < 0 || line >= static_cast<int>(lines.size())) return std::string();
    const std::string& text = lines[(size_t)line];
    if (text.empty() || column > static_cast<int>(text.size())) return std::string();

    // The word the caret is inside, or the one it sits just to the right of
    const char at = column < static_cast<int>(text.size()) ? text[(size_t)column] : '\0';
    const char before = column > 0 ? text[(size_t)column - 1] : '\0';
    if (!isWordChar(at) && !isWordChar(before)) return std::string();

    const TextPosition start = findWordStart(cursor.position);
    const TextPosition end = findWordEnd(cursor.position);
    const int length = end.column - start.column;
    // Two characters, so a lone "a" or "i" does not light up half the file
    if (length < 2 || start.line != end.line) return std::string();
    return text.substr((size_t)start.column, (size_t)length);
}

char CustomTextEditor::quotePair(char c) {
    switch (c) {
    case '"': return '"';
    case '\'': return '\'';
    case '`': return '`';
    default: return 0;
    }
}

int CustomTextEditor::getLineIndent(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;

    int indent = 0;
    for (char c : lines[lineIndex]) {
        if (c == ' ') {
            ++indent;
        } else if (c == '\t') {
            indent += tabSize;
        } else {
            break;
        }
    }
    return indent;
}

std::string CustomTextEditor::getIndentString(int level) const {
    return std::string(level, ' ');
}

OpenToken CustomTextEditor::lineStartState(int lineIndex) const {
    // Scans from the top, since lineTokens can be behind in the middle of an edit
    OpenToken state;
    std::vector<Token> tokens;
    for (int i = 0; i < lineIndex; ++i) {
        tokenizeText(lines[i], state, tokens);
    }
    return state;
}

bool CustomTextEditor::opensBlock(const std::string& text, const std::vector<Token>& tokens) const {
    std::string last;
    int blocks = 0;
    for (const Token& token : tokens) {
        if (token.type == TokenType::Comment || token.type == TokenType::MultiLineComment) continue;
        last = text.substr(token.start, token.length);

        // Lua blocks are counted, so "if x then return end" opens none
        if (language == SyntaxLanguage::Lua && token.type == TokenType::Keyword) {
            if (last == "function" || last == "then" || last == "do" || last == "repeat") {
                ++blocks;
            } else if (last == "else") {
                blocks = std::max(blocks, 1);
            } else if (last == "elseif" || last == "end" || last == "until") {
                blocks = std::max(blocks - 1, 0);
            }
        }
    }

    if (last == "{" || last == "(" || last == "[") return true;
    // Labels, access specifiers and case
    if (last == ":" && language != SyntaxLanguage::Lua) return true;
    return blocks > 0;
}

bool CustomTextEditor::closesBlock(const std::string& text) const {
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    if (text[start] == '}' || text[start] == ')' || text[start] == ']') return true;
    if (language != SyntaxLanguage::Lua) return false;

    size_t end = start;
    while (end < text.size() && isWordChar(text[end])) ++end;
    std::string word = text.substr(start, end - start);
    return word == "end" || word == "else" || word == "elseif" || word == "until";
}

int CustomTextEditor::openingBracketLine(const TextPosition& closer) const {
    char closeChar = lines[closer.line][closer.column];
    char openChar = getMatchingBracket(closeChar);
    int depth = 0;
    for (int i = closer.line; i >= 0; --i) {
        // Brackets in strings and comments aren't punctuation tokens
        for (auto token = lineTokens[i].rbegin(); token != lineTokens[i].rend(); ++token) {
            if (token->type != TokenType::Punctuation || (i == closer.line && token->start >= closer.column)) continue;
            char c = lines[i][token->start];
            if (c == closeChar) {
                ++depth;
            } else if (c == openChar) {
                if (depth == 0) return i;
                --depth;
            }
        }
    }
    return -1;
}

void CustomTextEditor::reindentAfterTyping(int typedLength) {
    if (!autoIndent) return;

    bool changed = false;
    for (size_t i = 0; i < cursors.size(); ++i) {
        int lineIndex = cursors[i].position.line;
        int typedColumn = cursors[i].position.column - typedLength;
        if (typedColumn < 0) continue;

        // Only when the typed text makes the line start with a closer, or stop doing so
        std::string& line = lines[lineIndex];
        std::string before = line;
        before.erase(typedColumn, typedLength);
        bool closes = closesBlock(line);
        if (closes == closesBlock(before)) continue;

        int indentEnd = static_cast<int>(std::min(line.find_first_not_of(" \t"), line.size()));
        // A closer gets its indent back only by growing into a longer word, as "end" into "endpoint"
        if (!closes && typedColumn <= indentEnd) continue;
        // Keep "else" outdented while it becomes "elseif"
        if (!closes && line.compare(indentEnd, 5, "elsei") == 0) continue;
        // The inside of a comment or string that spans lines is text, not code
        if (lineStartState(lineIndex).type != TokenType::Default) continue;

        // The line above sets the indent, one level deeper if it opens a block
        int indent = 0;
        for (int prev = lineIndex - 1; prev >= 0; --prev) {
            if (lines[prev].find_first_not_of(" \t") == std::string::npos) continue;
            indent = getLineIndent(prev) + (opensBlock(lines[prev], lineTokens[prev]) ? tabSize : 0);
            break;
        }
        if (closes) {
            // A bracket lines up with the line that opened it
            int opener = isCloseBracket(line[indentEnd]) ? openingBracketLine(TextPosition(lineIndex, indentEnd)) : -1;
            indent = (opener >= 0) ? getLineIndent(opener) : std::max(0, indent - tabSize);
        }
        if (indent == getLineIndent(lineIndex)) continue;

        line = getIndentString(indent) + line.substr(indentEnd);
        for (auto& cursor : cursors) {
            adjustCursorAfterDelete(cursor, TextPosition(lineIndex, 0), TextPosition(lineIndex, indentEnd));
            adjustCursorAfterInsert(cursor, TextPosition(lineIndex, 0), TextPosition(lineIndex, indent));
            if (cursor.position.line == lineIndex) cursor.preferredColumn = -1;
        }
        // Later cursors read the tokens
        tokenizeAll();
        changed = true;
    }

    if (changed) finalizeUndoRecord();
}

// -- Comment and format commands --

const std::string& CustomTextEditor::lineCommentPrefix() const {
    return languageDef.singleLineComment;
}

std::string CustomTextEditor::blockCommentStart() const {
    if (!languageDef.multiLineCommentStart.empty()) return languageDef.multiLineCommentStart;
    // Lua and CMake open a long comment by putting their line marker in front of a long bracket
    if (language == SyntaxLanguage::Lua && !languageDef.singleLineComment.empty())
        return languageDef.singleLineComment + "[[";
    if (language == SyntaxLanguage::CMake) return "#[[";
    return "";
}

std::string CustomTextEditor::blockCommentEnd() const {
    if (!languageDef.multiLineCommentEnd.empty()) return languageDef.multiLineCommentEnd;
    if (language == SyntaxLanguage::Lua || language == SyntaxLanguage::CMake) return "]]";
    return "";
}

int CustomTextEditor::firstCodeColumn(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;
    const size_t content = lines[lineIndex].find_first_not_of(" \t");
    return content == std::string::npos ? static_cast<int>(lines[lineIndex].size()) : static_cast<int>(content);
}

std::vector<int> CustomTextEditor::cursorLines() const {
    std::vector<int> result;
    for (const Cursor& cursor : cursors) {
        int first = cursor.selection.getMin().line;
        int last = cursor.selection.getMax().line;
        // A selection that only touches column 0 of the next line does not cover it
        if (last > first && cursor.selection.getMax().column == 0) --last;
        for (int line = first; line <= last; ++line) result.push_back(line);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void CustomTextEditor::insertColumns(int line, int column, const std::string& text) {
    if (line < 0 || line >= static_cast<int>(lines.size()) || text.empty()) return;
    column = std::clamp(column, 0, static_cast<int>(lines[line].size()));

    lines[line].insert(column, text);

    const TextPosition from(line, column);
    const TextPosition to(line, column + static_cast<int>(text.size()));
    for (auto& cursor : cursors) {
        adjustCursorAfterInsert(cursor, from, to);
        if (cursor.position.line == line) cursor.preferredColumn = -1;
    }
    if (showParamHint) paramHintAnchor = adjustPosAfterInsert(paramHintAnchor, from, to);
}

void CustomTextEditor::eraseColumns(int line, int column, int count) {
    if (line < 0 || line >= static_cast<int>(lines.size()) || count <= 0) return;
    column = std::clamp(column, 0, static_cast<int>(lines[line].size()));
    count = std::min(count, static_cast<int>(lines[line].size()) - column);
    if (count == 0) return;

    lines[line].erase(column, count);

    const TextPosition from(line, column);
    const TextPosition to(line, column + count);
    for (auto& cursor : cursors) {
        adjustCursorAfterDelete(cursor, from, to);
        if (cursor.position.line == line) cursor.preferredColumn = -1;
    }
    if (showParamHint) paramHintAnchor = adjustPosAfterDelete(paramHintAnchor, from, to);
}

void CustomTextEditor::ToggleComment() {
    if (readOnly || lineCommentPrefix().empty()) return;

    const std::string& prefix = lineCommentPrefix();
    std::vector<int> targets = cursorLines();
    if (targets.empty()) return;

    // Blank lines and the inside of a comment that spans lines carry no code to mark, so
    // they are left out and a block comment never gets // on each of its rows. The token
    // state is carried down in one pass instead of using lineStartState(), which rescans
    // from the first line every time and turns a whole-file comment into a quadratic scan.
    std::vector<int> code;
    OpenToken state;
    std::vector<Token> tokens;
    for (int line = 0; line <= targets.back() && line < static_cast<int>(lines.size()); ++line) {
        const bool insideToken = state.type != TokenType::Default;
        tokenizeText(lines[line], state, tokens);
        // The row that opens a spanning comment is comment syntax itself: marking it would
        // bury the opener and stripping the prefix would cut "--" out of "--[".
        const int content = static_cast<int>(lines[line].find_first_not_of(" \t"));
        const bool opensToken = state.type != TokenType::Default && content >= 0 && !tokens.empty() &&
            tokens.front().start == content &&
            (tokens.front().type == TokenType::Comment || tokens.front().type == TokenType::MultiLineComment);
        if (!insideToken && !opensToken && content != -1 &&
            std::find(targets.begin(), targets.end(), line) != targets.end()) {
            code.push_back(line);
        }
    }
    if (code.empty()) return;

    // One line without the marker is enough to comment the whole block, as in VSCode
    bool uncomment = true;
    for (int line : code) {
        if (!startsWithAt(lines[line], firstCodeColumn(line), prefix)) {
            uncomment = false;
            break;
        }
    }

    addUndoRecord();
    for (int line : code) {
        const int column = firstCodeColumn(line);
        const bool marked = startsWithAt(lines[line], column, prefix);
        // A line that already carries the marker keeps it in the comment pass and a line
        // without it stays without in the uncomment pass, so the command cannot stack
        // "-- --" or leave a half uncommented block behind.
        if (marked != uncomment) continue;
        if (!uncomment) {
            insertColumns(line, column, prefix + " ");
            continue;
        }
        int count = static_cast<int>(prefix.size());
        // The space the marker brought with it leaves with it
        if (startsWithAt(lines[line], column + prefix.size(), " ")) ++count;
        eraseColumns(line, column, count);
    }
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::ToggleBlockComment() {
    if (readOnly || cursors.empty()) return;

    const std::string open = blockCommentStart();
    const std::string close = blockCommentEnd();
    if (open.empty() || close.empty()) return;

    TextPosition start = cursors[primaryCursor].selection.getMin();
    TextPosition end = cursors[primaryCursor].selection.getMax();
    if (start == end) {
        // Without a selection the line under the caret is what gets wrapped
        const int line = cursors[primaryCursor].position.line;
        if (line < 0 || line >= static_cast<int>(lines.size())) return;
        start = TextPosition(line, firstCodeColumn(line));
        end = TextPosition(line, static_cast<int>(lines[line].size()));
    }
    if (start.line < 0 || start.line >= static_cast<int>(lines.size())) return;

    const std::string between = getRange(start, end);
    TextPosition openStart = start;
    TextPosition closeEnd = end;
    bool wrapped = between.size() >= open.size() + close.size() &&
                   between.compare(0, open.size(), open) == 0 &&
                   startsWithAt(between, between.size() - close.size(), close);
    // Inserting shifts the selection inward, so on the second call the markers sit just
    // outside the range instead of inside it; that is the wrapped state to undo.
    if (!wrapped && start.column >= static_cast<int>(open.size()) &&
        end.column >= static_cast<int>(close.size()) &&
        startsWithAt(lines[start.line], start.column - static_cast<int>(open.size()), open) &&
        startsWithAt(lines[end.line], end.column - static_cast<int>(close.size()), close)) {
        wrapped = true;
        openStart = TextPosition(start.line, start.column - static_cast<int>(open.size()));
        closeEnd = end;
    }

    addUndoRecord();
    if (wrapped) {
        // The far marker first, so the near one's columns stay valid
        const TextPosition closeStart(closeEnd.line, closeEnd.column - static_cast<int>(close.size()));
        deleteRange(closeStart, closeEnd);
        for (auto& cursor : cursors) adjustCursorAfterDelete(cursor, closeStart, closeEnd);

        const TextPosition openEnd(openStart.line, openStart.column + static_cast<int>(open.size()));
        deleteRange(openStart, openEnd);
        for (auto& cursor : cursors) adjustCursorAfterDelete(cursor, openStart, openEnd);
    } else {
        insertColumns(start.line, start.column, open);
        // The end sits behind the marker that was just put in front of it, on the same line
        const int shift = (end.line == start.line) ? static_cast<int>(open.size()) : 0;
        insertColumns(end.line, end.column + shift, close);
    }
    tokenizeAll();
    finalizeUndoRecord();
}

std::vector<int> CustomTextEditor::computeBufferIndents() const {
    // One level per unclosed bracket, plus Lua's block keywords, since they do the
    // same job there that braces do in C++. Lines inside a comment or a string that
    // spans lines are prose, so they come back as -1 and keep whatever they have.
    std::vector<int> level(lines.size(), 0);

    // A label or an access specifier (public:, case:) is a soft level: it indents the
    // lines under it, but the brace that ends the block swallows it, otherwise a switch
    // would leave its own closing brace one level too deep.
    std::vector<char> stack;
    const auto closeThroughSoft = [&stack]() {
        while (!stack.empty()) {
            const char top = stack.back();
            stack.pop_back();
            if (top == 'h') break;
        }
    };

    OpenToken open;
    std::vector<Token> tokens;
    for (size_t i = 0; i < lines.size(); ++i) {
        const bool insideToken = open.type != TokenType::Default;
        tokenizeText(lines[i], open, tokens);
        if (insideToken) {
            level[i] = -1;
            continue;
        }

        const std::string& text = lines[i];
        if (text.find_first_not_of(" \t") == std::string::npos) continue;

        // Read the line's brackets before touching the stack: a label line has to drop the
        // label it replaces, and the level it prints at is the one after that drop.
        std::vector<char> ops;
        bool label = false;
        std::string lastCode;
        for (const Token& token : tokens) {
            if (token.type == TokenType::Comment || token.type == TokenType::MultiLineComment ||
                token.type == TokenType::String) {
                continue;
            }
            const std::string piece = text.substr(token.start, token.length);
            lastCode = piece;
            for (char c : piece) {
                if (c == '{' || c == '(' || c == '[') ops.push_back('o');
                else if (c == '}' || c == ')' || c == ']') ops.push_back('c');
            }
            if (language == SyntaxLanguage::Lua && token.type == TokenType::Keyword) {
                if (piece == "function" || piece == "then" || piece == "do" || piece == "repeat") ops.push_back('o');
                else if (piece == "end" || piece == "until" || piece == "elseif") ops.push_back('c');
            }
        }
        if (language != SyntaxLanguage::Lua && lastCode == ":") label = true;

        if (label) {
            while (!stack.empty() && stack.back() == 's') stack.pop_back();
        }
        int softs = 0;
        while (softs < static_cast<int>(stack.size()) && stack[stack.size() - 1 - softs] == 's') ++softs;
        const bool startsClosed = closesBlock(text);
        // Clamped: an unbalanced file (a stray run of openers) must not build a line whose
        // indent is longer than the code on it.
        level[i] = std::clamp(static_cast<int>(stack.size()) - (startsClosed ? softs + 1 : 0), 0, 32);

        for (char op : ops) {
            if (op == 'o') stack.push_back('h');
            else closeThroughSoft();
        }
        if (label) stack.push_back('s');
    }

    return level;
}

void CustomTextEditor::FormatDocument() {
    if (readOnly || language == SyntaxLanguage::None || lines.empty()) return;

    const std::vector<int> wanted = computeBufferIndents();

    // Rewriting a tab-indented file into spaces would touch every one of its lines,
    // so the character the file itself prefers wins
    int tabs = 0;
    int spaces = 0;
    for (const std::string& line : lines) {
        if (line.empty()) continue;
        if (line[0] == '\t') ++tabs;
        else if (line[0] == ' ') ++spaces;
    }
    const std::string unit = (tabs > spaces) ? "\t" : std::string(tabSize, ' ');

    std::vector<std::string> formatted(lines.size());
    std::vector<int> oldEnd(lines.size(), 0);
    std::vector<int> newEnd(lines.size(), 0);
    bool changed = false;
    for (size_t i = 0; i < lines.size() && i < wanted.size(); ++i) {
        formatted[i] = lines[i];
        if (wanted[i] < 0) continue;

        std::string indent;
        for (int level = 0; level < wanted[i]; ++level) indent += unit;

        const int contentStart = firstCodeColumn(static_cast<int>(i));
        const size_t contentEnd = lines[i].find_last_not_of(" \t");
        // A line that holds nothing but whitespace loses all of it
        formatted[i] = contentEnd == std::string::npos
                           ? std::string()
                           : indent + lines[i].substr(contentStart, contentEnd - contentStart + 1);

        oldEnd[i] = contentStart;
        newEnd[i] = static_cast<int>(indent.size());
        if (formatted[i] != lines[i]) changed = true;
    }
    if (!changed) return;

    addUndoRecord();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (formatted[i] == lines[i]) continue;

        const int lineSize = static_cast<int>(formatted[i].size());
        auto mapColumn = [&](int column) {
            if (column <= oldEnd[i]) return newEnd[i];
            return std::min(column - oldEnd[i] + newEnd[i], lineSize);
        };
        for (auto& cursor : cursors) {
            if (cursor.position.line == static_cast<int>(i)) {
                cursor.position.column = mapColumn(cursor.position.column);
                cursor.preferredColumn = -1;
            }
            if (cursor.selection.start.line == static_cast<int>(i))
                cursor.selection.start.column = mapColumn(cursor.selection.start.column);
            if (cursor.selection.end.line == static_cast<int>(i))
                cursor.selection.end.column = mapColumn(cursor.selection.end.column);
        }
        lines[i] = formatted[i];
    }
    tokenizeAll();
    finalizeUndoRecord();
}

void CustomTextEditor::ensureValidCursors() {
    for (auto& cursor : cursors) {
        cursor.position = clampPosition(cursor.position);
        cursor.selection.start = clampPosition(cursor.selection.start);
        cursor.selection.end = clampPosition(cursor.selection.end);
    }
}

void CustomTextEditor::scrollToCursor() {
    if (cursors.empty() || lineHeight <= 0) return;

    TextPosition cursorPos = cursors[primaryCursor].position;
    float cursorY = cursorPos.line * lineHeight;
    float cursorX = textStartX + byteOffsetToPixelX(cursorPos.line, cursorPos.column);

    float viewHeight = ImGui::GetWindowHeight();
    if (viewHeight <= 0) viewHeight = 400; // Default fallback

    float newScrollY = scrollY;
    if (cursorY < scrollY) {
        newScrollY = cursorY;
    } else if (cursorY > scrollY + viewHeight - lineHeight * 2) {
        newScrollY = cursorY - viewHeight + lineHeight * 2;
    }
    newScrollY = std::max(0.0f, newScrollY);
    if (newScrollY != scrollY) {
        scrollY = newScrollY;
        ImGui::SetScrollY(newScrollY);
    }

    float viewWidth = ImGui::GetWindowWidth();
    if (viewWidth <= 0) viewWidth = 600; // Default fallback

    float newScrollX = scrollX;
    if (cursorX < scrollX + textStartX) {
        newScrollX = std::max(0.0f, cursorX - textStartX);
    } else if (cursorX > scrollX + viewWidth - charWidth * 2) {
        newScrollX = cursorX - viewWidth + charWidth * 4;
    }
    newScrollX = std::max(0.0f, newScrollX);
    if (newScrollX != scrollX) {
        scrollX = newScrollX;
        ImGui::SetScrollX(newScrollX);
    }
}

void CustomTextEditor::moveSelectedText(const TextPosition& destPos) {
    if (readOnly || cursors.empty()) return;

    mergeCursors();

    std::string text = GetSelectedText();
    if (text.empty()) return;

    TextPosition clampedDest = destPos;
    clampedDest.line = std::clamp(clampedDest.line, 0, static_cast<int>(lines.size()) - 1);
    clampedDest.column = std::clamp(clampedDest.column, 0, static_cast<int>(lines[clampedDest.line].size()));

    // Dropping inside the selection itself moves nothing
    for (const auto& cursor : cursors) {
        if (!cursor.selection.isEmpty()) {
            TextPosition min = cursor.selection.getMin();
            TextPosition max = cursor.selection.getMax();
            if (clampedDest >= min && clampedDest < max) return;
        }
    }

    // Deleting one selection shifts the others, so only a single one is dragged
    if (cursors.size() != 1) return;

    addUndoRecord();

    TextPosition start = cursors[0].selection.getMin();
    TextPosition end = cursors[0].selection.getMax();

    start.line = std::clamp(start.line, 0, static_cast<int>(lines.size()) - 1);
    start.column = std::clamp(start.column, 0, static_cast<int>(lines[start.line].size()));
    end.line = std::clamp(end.line, 0, static_cast<int>(lines.size()) - 1);
    end.column = std::clamp(end.column, 0, static_cast<int>(lines[end.line].size()));

    TextPosition insertPos = clampedDest;

    deleteRange(start, end);

    // The removed range shifts a destination that came after it
    if (insertPos > start) {
        if (insertPos.line != end.line) {
            insertPos.line -= (end.line - start.line);
        } else if (start.line == end.line) {
            insertPos.column -= (end.column - start.column);
        } else {
            // The tail of the end line is now appended to the start line
            insertPos.line = start.line;
            insertPos.column = start.column + (insertPos.column - end.column);
        }
    }

    insertPos.line = std::clamp(insertPos.line, 0, static_cast<int>(lines.size()) - 1);
    insertPos.column = std::clamp(insertPos.column, 0, static_cast<int>(lines[insertPos.line].size()));

    cursors.clear();
    Cursor cursor;
    cursor.position = insertPos;
    cursor.selection.start = insertPos;
    cursor.selection.end = insertPos;
    cursors.push_back(cursor);
    primaryCursor = 0;

    insertTextAtCursor(cursors[0], text, false);

    cursors[0].selection.start = insertPos;
    cursors[0].selection.end = cursors[0].position;

    tokenizeAll();
    finalizeUndoRecord();
}

std::vector<size_t> CustomTextEditor::cursorEditOrder() const {
    std::vector<size_t> order(cursors.size());
    std::iota(order.begin(), order.end(), 0);

    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        auto editStart = [](const Cursor& cursor) {
            return cursor.selection.isEmpty() ? cursor.position : cursor.selection.getMin();
        };
        return editStart(cursors[a]) < editStart(cursors[b]);
    });

    return order;
}

void CustomTextEditor::mergeCursors() {
    if (cursors.size() <= 1) return;

    sortCursors();

    std::vector<Cursor> merged;
    for (const auto& cursor : cursors) {
        if (merged.empty()) {
            merged.push_back(cursor);
        } else {
            auto& last = merged.back();
            if (cursor.position == last.position) {
                TextPosition minStart = std::min(last.selection.getMin(), cursor.selection.getMin());
                TextPosition maxEnd = std::max(last.selection.getMax(), cursor.selection.getMax());
                last.selection.start = minStart;
                last.selection.end = maxEnd;
            } else {
                merged.push_back(cursor);
            }
        }
    }

    cursors = merged;
    primaryCursor = std::min(primaryCursor, static_cast<int>(cursors.size()) - 1);
}

void CustomTextEditor::sortCursors() {
    std::sort(cursors.begin(), cursors.end(), [](const Cursor& a, const Cursor& b) {
        return a.position < b.position;
    });
}

TextPosition CustomTextEditor::clampPosition(const TextPosition& pos) const {
    TextPosition result = pos;
    result.line = std::clamp(result.line, 0, static_cast<int>(lines.size()) - 1);
    result.column = std::clamp(result.column, 0, static_cast<int>(lines[result.line].size()));
    while (result.column > 0 && result.column < static_cast<int>(lines[result.line].size()) &&
           isUtf8Continuation(static_cast<unsigned char>(lines[result.line][result.column]))) {
        --result.column;
    }
    return result;
}

void CustomTextEditor::moveCursor(Cursor& cursor, int deltaLine, int deltaCol, bool shift) {
    TextPosition newPos = cursor.position;

    if (deltaLine != 0) {
        int preferredVisualColumn = cursor.preferredColumn;
        if (preferredVisualColumn < 0) {
            preferredVisualColumn = byteOffsetToVisualColumn(newPos.line, newPos.column);
        }

        newPos.line = std::clamp(newPos.line + deltaLine, 0, static_cast<int>(lines.size()) - 1);

        cursor.preferredColumn = preferredVisualColumn;
        newPos.column = visualColumnToByteOffset(newPos.line, preferredVisualColumn);
    }

    if (deltaCol != 0) {
        cursor.preferredColumn = -1;

        if (deltaCol < 0) {
            for (int i = 0; i < -deltaCol; ++i) {
                if (newPos.column > 0) {
                    newPos.column = prevUtf8Column(newPos.line, newPos.column);
                } else if (newPos.line > 0) {
                    --newPos.line;
                    newPos.column = static_cast<int>(lines[newPos.line].size());
                }
            }
        } else {
            for (int i = 0; i < deltaCol; ++i) {
                if (newPos.column < static_cast<int>(lines[newPos.line].size())) {
                    newPos.column = nextUtf8Column(newPos.line, newPos.column);
                } else if (newPos.line < static_cast<int>(lines.size()) - 1) {
                    ++newPos.line;
                    newPos.column = 0;
                }
            }
        }

        newPos = clampPosition(newPos);
    }

    cursor.position = newPos;

    if (shift) {
        cursor.selection.end = newPos;
    } else {
        cursor.selection.start = newPos;
        cursor.selection.end = newPos;
    }
}

void CustomTextEditor::moveCursorWord(Cursor& cursor, int direction, bool shift) {
    TextPosition pos = cursor.position;

    if (direction > 0) {
        // Move to end of current/next word
        while (pos.column < static_cast<int>(lines[pos.line].size()) && 
               !isWordChar(lines[pos.line][pos.column])) {
            ++pos.column;
        }
        while (pos.column < static_cast<int>(lines[pos.line].size()) && 
               isWordChar(lines[pos.line][pos.column])) {
            ++pos.column;
        }
    } else {
        // Move to start of current/previous word
        if (pos.column > 0) --pos.column;
        while (pos.column > 0 && !isWordChar(lines[pos.line][pos.column])) {
            --pos.column;
        }
        while (pos.column > 0 && isWordChar(lines[pos.line][pos.column - 1])) {
            --pos.column;
        }
    }

    cursor.position = pos;

    if (shift) {
        cursor.selection.end = pos;
    } else {
        cursor.selection.start = pos;
        cursor.selection.end = pos;
    }

    cursor.preferredColumn = -1;
}

void CustomTextEditor::moveCursorToLineStart(Cursor& cursor, bool shift) {
    int firstNonSpace = 0;
    const std::string& line = lines[cursor.position.line];
    while (firstNonSpace < static_cast<int>(line.size()) && std::isspace(static_cast<unsigned char>(line[firstNonSpace]))) {
        ++firstNonSpace;
    }

    if (cursor.position.column == firstNonSpace) {
        cursor.position.column = 0;
    } else {
        cursor.position.column = firstNonSpace;
    }

    if (shift) {
        cursor.selection.end = cursor.position;
    } else {
        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }

    cursor.preferredColumn = -1;
}

void CustomTextEditor::moveCursorToLineEnd(Cursor& cursor, bool shift) {
    cursor.position.column = static_cast<int>(lines[cursor.position.line].size());

    if (shift) {
        cursor.selection.end = cursor.position;
    } else {
        cursor.selection.start = cursor.position;
        cursor.selection.end = cursor.position;
    }

    cursor.preferredColumn = -1;
}

bool CustomTextEditor::isWordChar(char c) const {
    unsigned char value = static_cast<unsigned char>(c);
    return std::isalnum(value) || c == '_' || value >= 0x80;
}

int CustomTextEditor::prevUtf8Column(int lineIndex, int column) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;
    const std::string& line = lines[lineIndex];
    int result = std::clamp(column, 0, static_cast<int>(line.size()));
    if (result <= 0) return 0;
    --result;
    while (result > 0 && isUtf8Continuation(static_cast<unsigned char>(line[result]))) {
        --result;
    }
    return result;
}

int CustomTextEditor::nextUtf8Column(int lineIndex, int column) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;
    const std::string& line = lines[lineIndex];
    int result = std::clamp(column, 0, static_cast<int>(line.size()));
    if (result >= static_cast<int>(line.size())) return static_cast<int>(line.size());
    ++result;
    while (result < static_cast<int>(line.size()) &&
           isUtf8Continuation(static_cast<unsigned char>(line[result]))) {
        ++result;
    }
    return result;
}

int CustomTextEditor::byteOffsetToVisualColumn(int lineIndex, int byteOffset) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;
    const std::string& line = lines[lineIndex];
    int clampedOffset = std::clamp(byteOffset, 0, static_cast<int>(line.size()));
    int visualColumn = 0;
    for (int i = 0; i < clampedOffset; ++i) {
        if (!isUtf8Continuation(static_cast<unsigned char>(line[i]))) {
            ++visualColumn;
        }
    }
    return visualColumn;
}

int CustomTextEditor::visualColumnToByteOffset(int lineIndex, int visualColumn) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;
    const std::string& line = lines[lineIndex];
    int targetColumn = std::max(0, visualColumn);
    int currentColumn = 0;
    for (int i = 0; i < static_cast<int>(line.size()); i = nextUtf8Column(lineIndex, i)) {
        if (currentColumn >= targetColumn) return i;
        ++currentColumn;
    }
    return static_cast<int>(line.size());
}

float CustomTextEditor::measureText(const char* begin, const char* end) const {
    ImFont* font = measureFont ? measureFont : ImGui::GetFont();
    float fontSize = measureFont ? measureFontSize : ImGui::GetFontSize();
    return font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, begin, end).x;
}

float CustomTextEditor::byteOffsetToPixelX(int lineIndex, int byteOffset) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0.0f;
    const std::string& line = lines[lineIndex];
    int clampedOffset = std::clamp(byteOffset, 0, static_cast<int>(line.size()));
    while (clampedOffset > 0 &&
           clampedOffset < static_cast<int>(line.size()) &&
           isUtf8Continuation(static_cast<unsigned char>(line[clampedOffset]))) {
        --clampedOffset;
    }
    return measureText(line.c_str(), line.c_str() + clampedOffset);
}

int CustomTextEditor::pixelXToByteOffset(int lineIndex, float pixelX) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return 0;
    const std::string& line = lines[lineIndex];
    if (pixelX <= 0.0f || line.empty()) return 0;

    int previous = 0;
    float previousX = 0.0f;
    for (int current = nextUtf8Column(lineIndex, 0);
         current <= static_cast<int>(line.size());
         current = nextUtf8Column(lineIndex, current)) {
        float currentX = previousX + measureText(line.c_str() + previous, line.c_str() + current);
        if (pixelX < (previousX + currentX) * 0.5f) {
            return previous;
        }
        if (current == static_cast<int>(line.size())) {
            return current;
        }
        previous = current;
        previousX = currentX;
    }

    return static_cast<int>(line.size());
}

float CustomTextEditor::computeMaxLineWidth() const {
    float widest = 0.0f;
    for (const auto& line : lines) {
        float lineWidth = measureText(line.c_str(), line.c_str() + line.size());
        if (lineWidth > widest) widest = lineWidth;
    }
    return widest;
}

TextPosition CustomTextEditor::findWordStart(const TextPosition& pos) const {
    TextPosition result = pos;
    const std::string& line = lines[result.line];

    while (result.column > 0 && isWordChar(line[result.column - 1])) {
        --result.column;
    }

    return result;
}

TextPosition CustomTextEditor::findWordEnd(const TextPosition& pos) const {
    TextPosition result = pos;
    const std::string& line = lines[result.line];

    while (result.column < static_cast<int>(line.size()) && isWordChar(line[result.column])) {
        ++result.column;
    }

    return result;
}

TextPosition CustomTextEditor::findDeleteWordStart(const TextPosition& pos) const {
    if (pos.column == 0) {
        if (pos.line == 0) return pos;
        return TextPosition(pos.line - 1, static_cast<int>(lines[pos.line - 1].size()));
    }

    const std::string& line = lines[pos.line];
    TextPosition result = pos;

    // A whitespace run goes as a whole, so one press clears an indentation
    while (result.column > 0 && std::isspace(static_cast<unsigned char>(line[result.column - 1]))) {
        --result.column;
    }
    if (pos.column - result.column > 1) return result;

    if (result.column > 0 && !isWordChar(line[result.column - 1])) {
        --result.column;
        return result;
    }

    return findWordStart(result);
}

TextPosition CustomTextEditor::findDeleteWordEnd(const TextPosition& pos) const {
    const std::string& line = lines[pos.line];
    const int lineSize = static_cast<int>(line.size());

    if (pos.column == lineSize) {
        if (pos.line == static_cast<int>(lines.size()) - 1) return pos;
        return TextPosition(pos.line + 1, 0);
    }

    TextPosition result = pos;

    while (result.column < lineSize && std::isspace(static_cast<unsigned char>(line[result.column]))) {
        ++result.column;
    }
    if (result.column - pos.column > 1) return result;

    if (result.column < lineSize && !isWordChar(line[result.column])) {
        ++result.column;
        return result;
    }

    return findWordEnd(result);
}

char CustomTextEditor::getCharAt(const TextPosition& pos) const {
    if (pos.line < 0 || pos.line >= static_cast<int>(lines.size())) return '\0';
    if (pos.column < 0 || pos.column >= static_cast<int>(lines[pos.line].size())) return '\0';
    return lines[pos.line][pos.column];
}

bool CustomTextEditor::isOpenBracket(char c) const {
    return c == '(' || c == '[' || c == '{';
}

bool CustomTextEditor::isCloseBracket(char c) const {
    return c == ')' || c == ']' || c == '}';
}

char CustomTextEditor::getMatchingBracket(char c) const {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        default: return '\0';
    }
}

char CustomTextEditor::findMatchingBracket(const TextPosition& pos, TextPosition& matchPos) const {
    char bracket = getCharAt(pos);
    if (!isOpenBracket(bracket) && !isCloseBracket(bracket)) return '\0';

    char target = getMatchingBracket(bracket);
    int direction = isOpenBracket(bracket) ? 1 : -1;
    int depth = 1;

    TextPosition current = pos;
    while (depth > 0) {
        if (direction > 0) {
            ++current.column;
            if (current.column >= static_cast<int>(lines[current.line].size())) {
                ++current.line;
                current.column = 0;
                if (current.line >= static_cast<int>(lines.size())) return '\0';
            }
        } else {
            --current.column;
            if (current.column < 0) {
                --current.line;
                if (current.line < 0) return '\0';
                current.column = static_cast<int>(lines[current.line].size()) - 1;
                if (current.column < 0) current.column = 0;
            }
        }

        char c = getCharAt(current);
        if (c == bracket) ++depth;
        else if (c == target) --depth;
    }

    matchPos = current;
    return target;
}

std::string CustomTextEditor::inferTypeOfVariable(const std::string& varName, int currentLine) const {
    if (varName.empty()) return "";

    // [const] Type [*&] varName
    std::regex cppDeclRegex("(?:const\\s+)?([A-Za-z0-9_:]+)(?:<[^>]*>)?\\s*(?:[*&]+\\s*)?" + varName + "\\b");
    // auto varName = new Type(...)
    std::regex cppNewRegex("\\bauto\\s*[*&]*\\s*" + varName + "\\s*=\\s*new\\s+([A-Za-z0-9_:]+)");
    // auto varName = Type(...) or Type{...}
    std::regex cppAutoCtorRegex("\\bauto\\s*[*&]*\\s*" + varName + "\\s*=\\s*([A-Z][A-Za-z0-9_:]*)\\s*[({]");

    std::regex luaVarRegex("\\b" + varName + "\\s*=\\s*([A-Za-z0-9_\\.:]+)(?:\\(|\\{)");
    std::regex luaLocalVarRegex("\\blocal\\s+" + varName + "\\s*=\\s*([A-Za-z0-9_\\.:]+)(?:\\(|\\{)");

    for (int i = currentLine; i >= 0; --i) {
        if (i >= static_cast<int>(lines.size())) continue;
        const std::string& line = lines[i];
        // Every pattern needs varName, so lines without it can't match any of them
        if (line.find(varName) == std::string::npos) continue;
        std::smatch match;

        if (language == SyntaxLanguage::Cpp) {
            // Most specific first
            if (std::regex_search(line, match, cppNewRegex)) {
                return match[1].str();
            }
            if (std::regex_search(line, match, cppAutoCtorRegex)) {
                return match[1].str();
            }
            if (std::regex_search(line, match, cppDeclRegex)) {
                std::string typeMatch = match[1].str();
                if (typeMatch != "return" && typeMatch != "new" && typeMatch != "delete" && typeMatch != "auto") {
                    return typeMatch;
                }
            }
        } else if (language == SyntaxLanguage::Lua) {
            if (std::regex_search(line, match, luaLocalVarRegex) ||
                std::regex_search(line, match, luaVarRegex)) {
                std::string typeName = match[1].str();
                // Constructor-style call: "local p = Player.new()" / "Player.create()" -> Player
                size_t sep = typeName.find_first_of(".:");
                if (sep != std::string::npos) {
                    std::string suffix = typeName.substr(sep + 1);
                    if (suffix == "new" || suffix == "create") {
                        typeName = typeName.substr(0, sep);
                    }
                }
                return typeName;
            }
        }
    }
    return "";
}

std::string CustomTextEditor::inferTypeBefore(int lineIndex, int endColumn) const {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) return "";

    std::string name = wordEndingAt(lines[lineIndex], endColumn);
    if (name.empty()) return "";

    // A class or enum name used directly is a static access (Engine.setScene, Scaling.NATIVE)
    if (suggestions && suggestions->IsKnownClassOrEnum(name)) return name;

    // Lua's "self" and C++'s "this" stand for the class the cursor is in
    if (name == ((language == SyntaxLanguage::Lua) ? "self" : "this")) return findEnclosingType(lineIndex);

    std::string type = inferTypeOfVariable(name, lineIndex);
    if (type.empty() && suggestions) {
        type = suggestions->FindSymbolType(name);
    }

    // Engine API parent types are stored without the namespace
    size_t lastColon = type.rfind(':');
    return (lastColon != std::string::npos) ? type.substr(lastColon + 1) : type;
}

std::string CustomTextEditor::findEnclosingType(int lineIndex) const {
    for (int i = std::min(lineIndex, static_cast<int>(lines.size()) - 1); i >= 0; --i) {
        const std::string& line = lines[i];
        // Definitions and class declarations start at column 0, so an indented
        // "Engine::getDeltatime()" never passes for one
        if (line.empty() || std::isspace(static_cast<unsigned char>(line[0]))) continue;

        if (language == SyntaxLanguage::Lua) {
            // "function BoxScript:onUpdate()" — the table before '.' or ':' owns the
            // method, while a plain function encloses nothing
            if (line.rfind("function ", 0) != 0 && line.rfind("local function ", 0) != 0) continue;

            size_t start = line.find_first_not_of(' ', line.find("function ") + 9);
            size_t sep = (start != std::string::npos) ? line.find_first_of(".:", start) : std::string::npos;
            if (sep == std::string::npos || sep > line.find('(')) return "";
            return line.substr(start, sep - start);
        }

        // "void BoxScript::onUpdate() {" — the qualifier right before the name is the
        // class, while "quantum::Vector3 helper()" only names a return type
        size_t paren = line.find('(');
        if (paren != std::string::npos) {
            size_t nameStart = paren;
            while (nameStart > 0 && std::isspace(static_cast<unsigned char>(line[nameStart - 1]))) nameStart--;
            while (nameStart > 0 && (std::isalnum(static_cast<unsigned char>(line[nameStart - 1])) || line[nameStart - 1] == '_')) nameStart--;

            if (nameStart >= 2 && line[nameStart - 1] == ':' && line[nameStart - 2] == ':') {
                std::string owner = wordEndingAt(line, static_cast<int>(nameStart) - 3);
                if (!owner.empty()) return owner;
            }

            // An unqualified definition is a free function, no class encloses the cursor
            size_t comment = line.find("//");
            std::string code = (comment != std::string::npos) ? line.substr(0, comment) : line;
            size_t last = code.find_last_not_of(" \t");
            if (last != std::string::npos && code[last] == '{') return "";
        }

        // "class BoxScript final : public quantum::Mesh {" — the last word before the base list
        if (line.rfind("class ", 0) == 0 || line.rfind("struct ", 0) == 0) {
            std::string head = line.substr(0, line.find_first_of(":{"));
            std::string name = wordEndingAt(head, static_cast<int>(head.size()) - 1);
            if (name == "final") {
                head.resize(head.rfind(name));
                name = wordEndingAt(head, static_cast<int>(head.size()) - 1);
            }
            if (!name.empty() && name != "class" && name != "struct") return name;
        }
    }

    return "";
}

SuggestionContext CustomTextEditor::buildSuggestionContext() const {
    SuggestionContext ctx;

    if (cursors.empty()) return ctx;

    ctx.isCpp = (language == SyntaxLanguage::Cpp);

    const TextPosition& pos = cursors[primaryCursor].position;

    TextPosition wordStart = findWordStart(pos);
    ctx.currentWord = getRange(wordStart, pos);

    if (pos.line >= 0 && pos.line < static_cast<int>(lines.size())) {
        ctx.lineContent = lines[pos.line];
    }

    ctx.cursorColumn = pos.column;
    ctx.lineNumber = pos.line;

    // Check for member access operators
    int checkCol = wordStart.column - 1;
    if (checkCol >= 0 && ctx.lineContent.size() > 0) {
        if (checkCol < static_cast<int>(ctx.lineContent.size())) {
            char c = ctx.lineContent[checkCol];
            if (c == '.') {
                ctx.afterDot = true;
            } else if (c == '>' && checkCol > 0 && ctx.lineContent[checkCol - 1] == '-') {
                ctx.afterArrow = true;
                checkCol--; // move past '-'
            } else if (c == ':' && checkCol > 0 && ctx.lineContent[checkCol - 1] == ':') {
                ctx.afterDoubleColon = true;
                checkCol--; // move past first ':'
            } else if (c == ':') {
                if (language == SyntaxLanguage::Lua) {
                    ctx.afterColon = true;
                }
            }
        }
    }

    // The word before the accessor is what member completion resolves against
    if (checkCol > 0) {
        TextPosition prevEnd(pos.line, checkCol - 1);
        while (prevEnd.column > 0 && std::isspace(static_cast<unsigned char>(ctx.lineContent[prevEnd.column]))) {
            prevEnd.column--;
        }
        if (prevEnd.column >= 0 && (std::isalnum(static_cast<unsigned char>(ctx.lineContent[prevEnd.column])) || ctx.lineContent[prevEnd.column] == '_')) {
            TextPosition prevStart = findWordStart(TextPosition(pos.line, prevEnd.column + 1));
            // getRange uses end column generically as exclusive bound, so we use prevEnd.column+1
            ctx.previousWord = getRange(prevStart, TextPosition(pos.line, prevEnd.column + 1));
        }
    }

    const bool memberAccess = ctx.afterDot || ctx.afterArrow || ctx.afterDoubleColon || ctx.afterColon;
    if (memberAccess && !ctx.previousWord.empty()) {
        if (ctx.afterDoubleColon && ctx.isCpp && suggestions && suggestions->IsKnownNamespace(ctx.previousWord)) {
            ctx.targetNamespace = ctx.previousWord;
        } else {
            // After '::' the previous word already is the type name (Vector3::ZERO)
            ctx.targetType = ctx.afterDoubleColon ? ctx.previousWord : inferTypeBefore(pos.line, checkCol - 1);
        }
    } else if (!memberAccess && ctx.isCpp) {
        // Inside a method the class's own members are reached without a receiver.
        // Lua has no such scope, there every member goes through "self"
        ctx.enclosingType = findEnclosingType(pos.line);
    }

    return ctx;
}

void CustomTextEditor::TriggerAutoComplete(bool manualInvoke) {
    if (!autoComplete || readOnly || !suggestions) return;

    autoCompleteAnchor = cursors[primaryCursor].position;
    updateSuggestions(manualInvoke);
    showAutoComplete = !currentSuggestions.empty();
    suggestionIndex = 0;
    scrollToSuggestion = true; // scroll back to top when the popup opens or refilters
}

void CustomTextEditor::CloseAutoComplete() {
    showAutoComplete = false;
    currentSuggestions.clear();
}

void CustomTextEditor::triggerParamHint() {
    if (!suggestions || cursors.empty()) return;

    const TextPosition& pos = cursors[primaryCursor].position;
    if (pos.line < 0 || pos.line >= static_cast<int>(lines.size())) return;

    const std::string& line = lines[pos.line];

    // Find the matching '(' by scanning backwards from cursor, handling nesting
    int parenDepth = 0;
    int openParenCol = -1;
    int searchLine = pos.line;
    int searchCol = pos.column - 1;

    while (searchLine >= 0) {
        const std::string& sLine = lines[searchLine];
        for (int c = searchCol; c >= 0; --c) {
            char ch = sLine[c];
            if (ch == ')') parenDepth++;
            else if (ch == '(') {
                if (parenDepth == 0) {
                    openParenCol = c;
                    break;
                }
                parenDepth--;
            }
        }
        if (openParenCol >= 0) break;
        searchLine--;
        if (searchLine >= 0) searchCol = static_cast<int>(lines[searchLine].size()) - 1;
    }

    if (openParenCol < 0 || searchLine < 0) {
        closeParamHint();
        return;
    }

    // Extract function name before the '('
    const std::string& funcLine = lines[searchLine];
    int nameEnd = openParenCol;
    while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(funcLine[nameEnd - 1]))) nameEnd--;
    int nameStart = nameEnd;
    while (nameStart > 0 && (std::isalnum(static_cast<unsigned char>(funcLine[nameStart - 1])) || funcLine[nameStart - 1] == '_')) nameStart--;
    std::string funcName = funcLine.substr(nameStart, nameEnd - nameStart);

    if (funcName.empty()) {
        closeParamHint();
        return;
    }

    // The accessor before the name gives the parent type
    std::string parentType;
    int beforeFunc = nameStart - 1;
    while (beforeFunc >= 0 && std::isspace(static_cast<unsigned char>(funcLine[beforeFunc]))) beforeFunc--;

    if (beforeFunc >= 0) {
        char accessor = funcLine[beforeFunc];
        if (accessor == ':' && beforeFunc > 0 && funcLine[beforeFunc - 1] == ':') {
            // Static call, the name before '::' is the type itself
            parentType = wordEndingAt(funcLine, beforeFunc - 2);
        } else if (accessor == '.' || accessor == ':') {
            parentType = inferTypeBefore(searchLine, beforeFunc - 1);
        } else if (accessor == '>' && beforeFunc > 0 && funcLine[beforeFunc - 1] == '-') {
            parentType = inferTypeBefore(searchLine, beforeFunc - 2);
        }
    }

    auto signatures = suggestions->FindSignatures(funcName, parentType);
    if (signatures.empty()) {
        closeParamHint();
        return;
    }

    paramHintFuncName = funcName;
    paramHintSignatures = signatures;
    paramHintOverloadIndex = 0;
    paramHintAnchor = TextPosition(searchLine, openParenCol);
    showParamHint = true;

    updateParamHint();
}

void CustomTextEditor::updateParamHint() {
    if (!showParamHint || cursors.empty()) return;

    const TextPosition& pos = cursors[primaryCursor].position;

    // Validate anchor: the '(' must still exist at the anchor position
    if (paramHintAnchor.line < 0 || paramHintAnchor.line >= static_cast<int>(lines.size())) {
        closeParamHint();
        return;
    }
    const std::string& anchorLine = lines[paramHintAnchor.line];
    if (paramHintAnchor.column < 0 || paramHintAnchor.column >= static_cast<int>(anchorLine.size()) || anchorLine[paramHintAnchor.column] != '(') {
        closeParamHint();
        return;
    }

    // Verify the function name before the anchor is still the same
    int nameEnd = paramHintAnchor.column;
    while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(anchorLine[nameEnd - 1]))) nameEnd--;
    int nameStart = nameEnd;
    while (nameStart > 0 && (std::isalnum(static_cast<unsigned char>(anchorLine[nameStart - 1])) || anchorLine[nameStart - 1] == '_')) nameStart--;
    std::string currentFuncName = anchorLine.substr(nameStart, nameEnd - nameStart);

    if (currentFuncName != paramHintFuncName) {
        closeParamHint();
        return;
    }

    if (pos < paramHintAnchor || (pos.line == paramHintAnchor.line && pos.column <= paramHintAnchor.column)) {
        closeParamHint();
        return;
    }

    // Count commas between paramHintAnchor '(' and cursor, respecting nesting
    int commaCount = 0;
    int parenDepth = 0;
    int startLine = paramHintAnchor.line;
    int startCol = paramHintAnchor.column + 1; // after '('
    bool inString = false;
    char stringQuote = '\0';

    for (int ln = startLine; ln <= pos.line && ln < static_cast<int>(lines.size()); ++ln) {
        const std::string& line = lines[ln];
        int colStart = (ln == startLine) ? startCol : 0;
        int colEnd = (ln == pos.line) ? pos.column : static_cast<int>(line.size());
        for (int c = colStart; c < colEnd && c < static_cast<int>(line.size()); ++c) {
            char ch = line[c];

            if (inString) {
                if (ch == stringQuote) {
                    if (c == 0 || line[c - 1] != '\\') {
                        inString = false;
                    }
                }
                continue;
            } else if (ch == '"' || ch == '\'') {
                inString = true;
                stringQuote = ch;
                continue;
            }

            if (ch == '(' || ch == '[' || ch == '{') parenDepth++;
            else if (ch == ')' || ch == ']' || ch == '}') {
                if (parenDepth > 0) parenDepth--;
                else {
                    closeParamHint();
                    return;
                }
            }
            else if (ch == ',' && parenDepth == 0) commaCount++;
        }
    }

    paramHintActiveParam = commaCount;
}

void CustomTextEditor::closeParamHint() {
    showParamHint = false;
    paramHintSignatures.clear();
    paramHintFuncName.clear();
    paramHintActiveParam = 0;
    paramHintOverloadIndex = 0;
}

void CustomTextEditor::renderParamHint(const ImVec2& origin) {
    if (!showParamHint || paramHintSignatures.empty()) return;

    if (paramHintOverloadIndex >= static_cast<int>(paramHintSignatures.size())) {
        paramHintOverloadIndex = 0;
    }

    const std::string& sig = paramHintSignatures[paramHintOverloadIndex];

    // Extract params from "ClassName:funcName(Type1 param1, Type2 param2)"
    size_t openParen = sig.find('(');
    size_t closeParen = sig.rfind(')');
    if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen) return;

    std::string funcPrefix = sig.substr(0, openParen + 1); // "ClassName:funcName("
    std::string paramStr = sig.substr(openParen + 1, closeParen - openParen - 1);

    // Split params by comma, respecting nested angle brackets
    std::vector<std::string> params;
    int depth = 0;
    std::string current;
    for (char ch : paramStr) {
        if (ch == '<') depth++;
        else if (ch == '>') depth--;
        else if (ch == ',' && depth == 0) {
            params.push_back(current);
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.empty() || !paramStr.empty()) {
        params.push_back(current);
    }

    for (auto& p : params) {
        size_t start = p.find_first_not_of(' ');
        size_t end = p.find_last_not_of(' ');
        if (start != std::string::npos) p = p.substr(start, end - start + 1);
    }

    // Position above the '(' anchor
    ImVec2 screenPos = textToScreen(paramHintAnchor, origin);
    screenPos.y -= lineHeight + 4.0f;

    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    std::string fullText = funcPrefix;
    for (size_t i = 0; i < params.size(); i++) {
        if (i > 0) fullText += ", ";
        fullText += params[i];
    }
    fullText += ")";

    std::string overloadText;
    if (paramHintSignatures.size() > 1) {
        overloadText = std::to_string(paramHintOverloadIndex + 1) + "/" + std::to_string(paramHintSignatures.size());
    }

    float textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0, fullText.c_str()).x;
    float overloadWidth = overloadText.empty() ? 0 : font->CalcTextSizeA(fontSize, FLT_MAX, 0, overloadText.c_str()).x + 16.0f;
    float padding = Theme::dpi(8.0f);
    float popupWidth = textWidth + overloadWidth + padding * 2;
    float popupHeight = lineHeight + padding;

    // Ensure popup doesn't go off-screen
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (screenPos.x + popupWidth > displaySize.x) {
        screenPos.x = displaySize.x - popupWidth - Theme::dpi(10.0f);
    }
    if (screenPos.x < 0) screenPos.x = Theme::dpi(10.0f);
    if (screenPos.y < 0) {
        screenPos.y = textToScreen(paramHintAnchor, origin).y + lineHeight + 2.0f;
    }

    ImGui::SetNextWindowPos(screenPos);
    ImGui::SetNextWindowSize(ImVec2(popupWidth, popupHeight));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));

    if (ImGui::Begin("##ParamHint", nullptr, flags)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 textPos = ImGui::GetCursorScreenPos();

        ImU32 normalColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImU32 activeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImU32 activeUnderline = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.65f, 0.95f, 1.0f));

        float x = textPos.x;

        drawList->AddText(font, fontSize, ImVec2(x, textPos.y), normalColor, funcPrefix.c_str());
        x += font->CalcTextSizeA(fontSize, FLT_MAX, 0, funcPrefix.c_str()).x;

        for (size_t i = 0; i < params.size(); i++) {
            if (i > 0) {
                drawList->AddText(font, fontSize, ImVec2(x, textPos.y), normalColor, ", ");
                x += font->CalcTextSizeA(fontSize, FLT_MAX, 0, ", ").x;
            }

            const std::string& param = params[i];
            bool isActive = (static_cast<int>(i) == paramHintActiveParam);
            ImU32 color = isActive ? activeColor : normalColor;

            drawList->AddText(font, fontSize, ImVec2(x, textPos.y), color, param.c_str());
            float paramWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0, param.c_str()).x;

            if (isActive) {
                float underlineY = textPos.y + fontSize + 1.0f;
                drawList->AddLine(ImVec2(x, underlineY), ImVec2(x + paramWidth, underlineY), activeUnderline, 2.0f);
            }

            x += paramWidth;
        }

        drawList->AddText(font, fontSize, ImVec2(x, textPos.y), normalColor, ")");
        x += font->CalcTextSizeA(fontSize, FLT_MAX, 0, ")").x;

        if (!overloadText.empty()) {
            x += 12.0f;
            ImU32 dimColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            drawList->AddText(font, fontSize, ImVec2(x, textPos.y), dimColor, overloadText.c_str());
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void CustomTextEditor::UpdateProjectSymbols(const std::vector<ProjectSymbol>& symbols) {
    if (!suggestions) return;

    suggestions->ClearSymbols();
    suggestions->ClearInheritance();

    if (language == SyntaxLanguage::Lua || language == SyntaxLanguage::Cpp) {
        addEngineAPISuggestions();
    }

    // Engine API symbols are authoritative (real signatures). Engine headers may live
    // inside the project tree, so the project scanner can produce duplicates of the
    // same members — skip those.
    static const std::unordered_set<std::string> engineKeys = [] {
        std::unordered_set<std::string> keys;
        for (const auto& sym : getEngineAPISymbols()) {
            if (isEngineApiConstructorSymbol(sym)) continue;
            keys.insert(std::string(sym.name) + "\x1f" + (sym.parent ? sym.parent : ""));
        }
        return keys;
    }();

    for (const auto& sym : symbols) {
        if (engineKeys.find(sym.name + "\x1f" + sym.parentType) != engineKeys.end()) continue;
        suggestions->AddSymbol(sym.name, sym.kind, sym.detail, sym.parentType, sym.typeInfo);

        // A script derives from an engine class, so its members complete through it
        if (sym.kind == SuggestionKind::Class) {
            suggestions->SetClassParent(sym.name, baseClassFromDetail(sym.detail));
        }
    }
}

void CustomTextEditor::SetDiffBase(const std::string& text) {
    splitLines(text, diffBaseLines);
    hasDiffBase = true;
    diffDirty = true;
    diffNextTime = 0.0;
}

void CustomTextEditor::ClearDiffBase() {
    hasDiffBase = false;
    diffBaseLines.clear();
    diffChanges.clear();
    diffDirty = false;
    diffVersion++;
    diffPeekChange = -1;
}

void CustomTextEditor::updateDiff() {
    if (!diffDirty) return;

    const double now = ImGui::GetTime();
    if (now < diffNextTime) {
        // The loop may be idling
        Backend::getApp().requestFrame();
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    diffChanges = LineDiff::compute(diffBaseLines, lines);
    diffVersion++;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // A slow diff runs at most 5% of the time
    diffNextTime = now + seconds * 20.0;
    diffDirty = false;
}

bool CustomTextEditor::isInCommentOrString(const TextPosition& pos) const {
    if (pos.line < 0 || pos.line >= static_cast<int>(lineTokens.size())) return false;

    // Look at the character just typed (left of the cursor)
    int col = pos.column > 0 ? pos.column - 1 : 0;
    for (const auto& token : lineTokens[pos.line]) {
        if (col >= token.start && col < token.start + token.length) {
            return token.type == TokenType::Comment ||
                   token.type == TokenType::MultiLineComment ||
                   token.type == TokenType::String;
        }
    }
    return false;
}

void CustomTextEditor::updateSuggestions(bool manualInvoke) {
    if (!suggestions || cursors.empty()) return;

    if (isInCommentOrString(cursors[primaryCursor].position)) {
        currentSuggestions.clear();
        showAutoComplete = false;
        return;
    }

    suggestions->UpdateDocumentWords(lines);

    SuggestionContext ctx = buildSuggestionContext();
    ctx.manualInvoke = manualInvoke;
    autoCompleteAnchor = findWordStart(cursors[primaryCursor].position);

    currentSuggestions = suggestions->GetSuggestions(ctx);

    // Close the popup if filtering (e.g. backspace) left nothing to show
    if (showAutoComplete && currentSuggestions.empty()) {
        showAutoComplete = false;
    }

    // Every refilter re-ranks the list, so the selection snaps back to the best
    // match at the top, like VSCode does on each keystroke
    if (!currentSuggestions.empty()) {
        suggestionIndex = 0;
        scrollToSuggestion = true;
    }
}

void CustomTextEditor::applySuggestion() {
    if (!showAutoComplete || suggestionIndex >= static_cast<int>(currentSuggestions.size())) return;

    addUndoRecord();

    const auto& item = currentSuggestions[suggestionIndex];

    // Delete the current word being typed
    TextPosition pos = cursors[primaryCursor].position;
    TextPosition delStart = autoCompleteAnchor < pos ? autoCompleteAnchor : pos;
    TextPosition delEnd = autoCompleteAnchor < pos ? pos : autoCompleteAnchor;
    deleteRange(delStart, delEnd);
    cursors[primaryCursor].position = delStart;
    cursors[primaryCursor].selection.start = delStart;
    cursors[primaryCursor].selection.end = delStart;

    for (size_t i = 0; i < cursors.size(); ++i) {
        if (static_cast<int>(i) != primaryCursor) {
            adjustCursorAfterDelete(cursors[i], delStart, delEnd);
        }
    }

    // Snippet placeholders are reduced to their default text, ${n:text} -> text
    std::string textToInsert = item.insertText;
    std::string processed;
    size_t i = 0;
    while (i < textToInsert.size()) {
        if (textToInsert[i] == '$' && i + 1 < textToInsert.size() && textToInsert[i + 1] == '{') {
            size_t start = i + 2;
            size_t end = textToInsert.find('}', start);
            if (end != std::string::npos) {
                std::string placeholder = textToInsert.substr(start, end - start);
                size_t colonPos = placeholder.find(':');
                if (colonPos != std::string::npos) {
                    processed += placeholder.substr(colonPos + 1);
                }
                i = end + 1;
                continue;
            }
        }
        processed += textToInsert[i];
        ++i;
    }

    // Insert the completion directly (avoid InsertText which creates its own undo record)
    TextPosition insertPos = cursors[primaryCursor].position;
    insertTextAtCursor(cursors[primaryCursor], processed, false);
    TextPosition afterPos = cursors[primaryCursor].position;

    for (size_t i = 0; i < cursors.size(); ++i) {
        if (static_cast<int>(i) != primaryCursor) {
            adjustCursorAfterInsert(cursors[i], insertPos, afterPos);
        }
    }

    mergeCursors();
    tokenizeAll();
    finalizeUndoRecord();
    CloseAutoComplete();
}

TextPosition CustomTextEditor::screenToText(const ImVec2& screenPos, const ImVec2& origin) const {
    float y = screenPos.y - origin.y;
    float x = screenPos.x - origin.x - textStartX;

    int line = std::clamp(static_cast<int>(y / lineHeight), 0, static_cast<int>(lines.size()) - 1);
    int column = pixelXToByteOffset(line, x);

    return TextPosition(line, column);
}

ImVec2 CustomTextEditor::textToScreen(const TextPosition& pos, const ImVec2& origin) const {
    float x = origin.x + textStartX + byteOffsetToPixelX(pos.line, pos.column);
    float y = origin.y + pos.line * lineHeight;
    return ImVec2(x, y);
}

void CustomTextEditor::handleKeyboardInput() {
    ImGuiIO& io = ImGui::GetIO();

    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;
    bool alt = io.KeyAlt;

    // Handle autocomplete navigation first - must return to prevent editor from also processing keys
    if (showAutoComplete && !currentSuggestions.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            CloseAutoComplete();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            suggestionIndex = std::max(0, suggestionIndex - 1);
            scrollToSuggestion = true;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            suggestionIndex = std::min(static_cast<int>(currentSuggestions.size()) - 1, suggestionIndex + 1);
            scrollToSuggestion = true;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            applySuggestion();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
            suggestionIndex = std::max(0, suggestionIndex - 5);
            scrollToSuggestion = true;
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
            suggestionIndex = std::min(static_cast<int>(currentSuggestions.size()) - 1, suggestionIndex + 5);
            scrollToSuggestion = true;
            return;
        }
    }

    if (showParamHint && !showAutoComplete) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            closeParamHint();
            return;
        }
        // Ctrl+Up/Down to cycle overloads when multiple exist
        if (paramHintSignatures.size() > 1 && ctrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                paramHintOverloadIndex = (paramHintOverloadIndex + static_cast<int>(paramHintSignatures.size()) - 1) % static_cast<int>(paramHintSignatures.size());
                return;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                paramHintOverloadIndex = (paramHintOverloadIndex + 1) % static_cast<int>(paramHintSignatures.size());
                return;
            }
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ClearExtraCursors();
        ClearSelection();
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        SelectAll();
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        Copy();
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
        Cut();
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
        Paste();
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (shift) {
            Redo();
        } else {
            Undo();
        }
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        Redo();
    }

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
        OpenFind();
    }

    // Ctrl+G is VSCode's "Go to Line/Column"; the box takes the number and jumps
    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_G)) {
        OpenGoToLine();
    }

    // Ctrl+L selects the current line and keeps adding lines on every press
    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_L)) {
        SelectLines();
    }

    // Ctrl+Shift+K deletes the line, Ctrl+Shift+D duplicates it. Ctrl+D on its own is the
    // multi-cursor "add next occurrence" below, so the shift has to be told apart here.
    if (ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_K)) {
        DeleteLines();
    }
    if (ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_D)) {
        DuplicateLines();
    }

    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) {
        if (onFontZoom) onFontZoom(1);
    }

    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) {
        if (onFontZoom) onFontZoom(-1);
    }

    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0))) {
        if (onFontZoom) onFontZoom(0);
    }

    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_D)) {
        // Select word at cursor or add cursor at next occurrence
        if (!cursors.empty()) {
            std::string word = getRange(cursors.back().selection.getMin(), cursors.back().selection.getMax());
            if (word.empty()) {
                TextPosition start = findWordStart(cursors.back().position);
                TextPosition end = findWordEnd(cursors.back().position);
                cursors.back().selection.start = start;
                cursors.back().selection.end = end;
                cursors.back().position = end;
                primaryCursor = cursors.size() - 1;
            } else {
                // Find next occurrence and add cursor (with wrap-around)
                TextPosition searchStart = cursors.back().selection.getMax();
                bool found = false;
                for (int line = searchStart.line; line < static_cast<int>(lines.size()) && !found; ++line) {
                    size_t startCol = (line == searchStart.line) ? searchStart.column : 0;
                    size_t pos = lines[line].find(word, startCol);
                    if (pos != std::string::npos) {
                        Cursor newCursor;
                        newCursor.selection.start = TextPosition(line, static_cast<int>(pos));
                        newCursor.selection.end = TextPosition(line, static_cast<int>(pos + word.size()));
                        newCursor.position = newCursor.selection.end;
                        cursors.push_back(newCursor);
                        primaryCursor = cursors.size() - 1;
                        found = true;
                    }
                }
                // Wrap around from beginning
                if (!found) {
                    TextPosition wrapEnd = cursors.front().selection.getMin();
                    for (int line = 0; line <= wrapEnd.line && !found; ++line) {
                        size_t endCol = (line == wrapEnd.line) ? wrapEnd.column : lines[line].size();
                        size_t pos = lines[line].find(word, 0);
                        if (pos != std::string::npos && static_cast<int>(pos) < static_cast<int>(endCol)) {
                            Cursor newCursor;
                            newCursor.selection.start = TextPosition(line, static_cast<int>(pos));
                            newCursor.selection.end = TextPosition(line, static_cast<int>(pos + word.size()));
                            newCursor.position = newCursor.selection.end;
                            cursors.push_back(newCursor);
                            primaryCursor = cursors.size() - 1;
                            found = true;
                        }
                    }
                }
                scrollToCursor();
            }
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        for (auto& cursor : cursors) {
            if (!shift && !cursor.selection.isEmpty()) {
                // Collapse selection to start (VSCode behavior)
                TextPosition minPos = cursor.selection.getMin();
                cursor.position = minPos;
                cursor.selection.start = minPos;
                cursor.selection.end = minPos;
                cursor.preferredColumn = -1;
                if (ctrl) {
                    moveCursorWord(cursor, -1, false);
                }
            } else if (ctrl) {
                moveCursorWord(cursor, -1, shift);
            } else {
                moveCursor(cursor, 0, -1, shift);
            }
        }
        mergeCursors();
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        for (auto& cursor : cursors) {
            if (!shift && !cursor.selection.isEmpty()) {
                // Collapse selection to end (VSCode behavior)
                TextPosition maxPos = cursor.selection.getMax();
                cursor.position = maxPos;
                cursor.selection.start = maxPos;
                cursor.selection.end = maxPos;
                cursor.preferredColumn = -1;
                if (ctrl) {
                    moveCursorWord(cursor, 1, false);
                }
            } else if (ctrl) {
                moveCursorWord(cursor, 1, shift);
            } else {
                moveCursor(cursor, 0, 1, shift);
            }
        }
        mergeCursors();
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        if (alt && ctrl) {
            // Add cursor above
            if (!cursors.empty()) {
                TextPosition pos = cursors[0].position;
                if (pos.line > 0) {
                    int visualCol = byteOffsetToVisualColumn(pos.line, pos.column);
                    AddCursor(pos.line - 1, visualColumnToByteOffset(pos.line - 1, visualCol));
                    sortCursors();
                }
            }
        } else if (alt) {
            MoveLines(-1);   // VSCode's "Move Line Up"
        } else {
            for (auto& cursor : cursors) {
                moveCursor(cursor, -1, 0, shift);
            }
            mergeCursors();
            scrollToCursor();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        if (alt && ctrl) {
            // Add cursor below
            if (!cursors.empty()) {
                TextPosition pos = cursors.back().position;
                if (pos.line < static_cast<int>(lines.size()) - 1) {
                    int visualCol = byteOffsetToVisualColumn(pos.line, pos.column);
                    AddCursor(pos.line + 1, visualColumnToByteOffset(pos.line + 1, visualCol));
                }
            }
        } else if (alt) {
            MoveLines(1);    // VSCode's "Move Line Down"
        } else {
            for (auto& cursor : cursors) {
                moveCursor(cursor, 1, 0, shift);
            }
            mergeCursors();
            scrollToCursor();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        for (auto& cursor : cursors) {
            if (ctrl) {
                cursor.position = TextPosition(0, 0);
                if (!shift) {
                    cursor.selection.start = cursor.position;
                    cursor.selection.end = cursor.position;
                } else {
                    cursor.selection.end = cursor.position;
                }
            } else {
                moveCursorToLineStart(cursor, shift);
            }
        }
        mergeCursors();
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        for (auto& cursor : cursors) {
            if (ctrl) {
                cursor.position = TextPosition(static_cast<int>(lines.size()) - 1, 
                                               static_cast<int>(lines.back().size()));
                if (!shift) {
                    cursor.selection.start = cursor.position;
                    cursor.selection.end = cursor.position;
                } else {
                    cursor.selection.end = cursor.position;
                }
            } else {
                moveCursorToLineEnd(cursor, shift);
            }
        }
        mergeCursors();
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        int pageSize = std::max(1, static_cast<int>(ImGui::GetWindowHeight() / lineHeight) - 2);
        for (auto& cursor : cursors) {
            moveCursor(cursor, -pageSize, 0, shift);
        }
        mergeCursors();
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        int pageSize = std::max(1, static_cast<int>(ImGui::GetWindowHeight() / lineHeight) - 2);
        for (auto& cursor : cursors) {
            moveCursor(cursor, pageSize, 0, shift);
        }
        mergeCursors();
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !readOnly) {
        if (ctrl) {
            DeleteWordLeft();
        } else {
            Backspace();
        }
        scrollToCursor();
        if (showAutoComplete) updateSuggestions();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !readOnly) {
        if (ctrl) {
            DeleteWordRight();
        } else {
            Delete();
        }
        scrollToCursor();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !readOnly) {
        if (!showAutoComplete) {
            InsertText("\n");
            scrollToCursor();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !readOnly) {
        if (!showAutoComplete) {
            bool multiLineSelection = false;
            for (const auto& cursor : cursors) {
                if (cursor.selection.getMin().line != cursor.selection.getMax().line) {
                    multiLineSelection = true;
                    break;
                }
            }

            if (shift || multiLineSelection) {
                // Block indent / unindent
                addUndoRecord();
                bool changed = false;
                std::unordered_set<int> linesProcessed;

                for (auto& cursor : cursors) {
                    TextPosition minPos = cursor.selection.getMin();
                    TextPosition maxPos = cursor.selection.getMax();
                    int endLine = maxPos.line;
                    if (maxPos.column == 0 && endLine > minPos.line) {
                        endLine--; // Don't indent if selection just touches the start of the next line
                    }

                    for (int l = minPos.line; l <= endLine; ++l) {
                        if (linesProcessed.count(l)) continue;
                        linesProcessed.insert(l);

                        if (shift) {
                            // Unindent
                            const std::string& line = lines[l];
                            if (static_cast<int>(line.size()) >= tabSize) {
                                bool canUnindent = true;
                                for (int i = 0; i < tabSize; ++i) {
                                    if (line[i] != ' ') {
                                        canUnindent = false;
                                        break;
                                    }
                                }
                                if (canUnindent) {
                                    lines[l].erase(0, tabSize);
                                    TextPosition delStart(l, 0);
                                    TextPosition delEnd(l, tabSize);
                                    for (auto& c : cursors) {
                                        adjustCursorAfterDelete(c, delStart, delEnd);
                                    }
                                    if (showParamHint) paramHintAnchor = adjustPosAfterDelete(paramHintAnchor, delStart, delEnd);
                                    changed = true;
                                }
                            }
                        } else {
                            // Indent
                            lines[l].insert(0, tabSize, ' ');
                            TextPosition insStart(l, 0);
                            TextPosition insEnd(l, tabSize);
                            for (auto& c : cursors) {
                                adjustCursorAfterInsert(c, insStart, insEnd);
                            }
                            if (showParamHint) paramHintAnchor = adjustPosAfterInsert(paramHintAnchor, insStart, insEnd);
                            changed = true;
                        }
                    }
                }
                if (changed) {
                    tokenizeAll();
                    finalizeUndoRecord();
                }
            } else {
                InsertText("\t"); // expanded to the next tab stop
            }
        }
    }

    // Trigger auto-complete with Ctrl+Space (manual invoke shows the full list).
    // Shift+Space does the same, the other binding VSCode documents for "Trigger Suggest"
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Space)) {
        TriggerAutoComplete(true);
    }
    if (shift && !ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Space)) {
        TriggerAutoComplete(true);
        // Windows still queues the space that Shift+Space produced, and handleTextInput()
        // runs after this: without dropping it the binding would type a space as the list
        // opens, which then lands inside the moment a suggestion is accepted.
        ImGuiIO& io = ImGui::GetIO();
        for (int i = io.InputQueueCharacters.Size - 1; i >= 0; --i) {
            if (io.InputQueueCharacters[i] == ' ') io.InputQueueCharacters.erase(&io.InputQueueCharacters[i]);
        }
    }

    // The three code commands the menu and the palette also expose. Read through the
    // registry so a remap in Editor Settings > Shortcuts moves this too; the defaults are
    // VSCode's (Ctrl+/ plus its numpad twin, Ctrl+Shift+A, Ctrl+Shift+F).
    if (KeyBindings::pressed("code.comment")) {
        ToggleComment();
    }
    if (KeyBindings::pressed("code.blockComment")) {
        ToggleBlockComment();
    }
    if (KeyBindings::pressed("code.format")) {
        FormatDocument();
    }

    if (showParamHint) {
        updateParamHint();
    }
}

void CustomTextEditor::handleMouseInput() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;
    ImVec2 contentPos = ImGui::GetCursorScreenPos();

    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;
    bool alt = io.KeyAlt;

    // Ctrl+mouse wheel zooms the font (ImGui skips scrolling while Ctrl is held)
    if (ctrl && io.MouseWheel != 0.0f) {
        if (onFontZoom) onFontZoom(io.MouseWheel > 0.0f ? 1 : -1);
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        showContextMenu = false;
        TextPosition clickPos = screenToText(mousePos, contentPos);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClickTime).count();

        if (elapsed < 400 && clickPos == lastClickPos) {
            ++clickCount;
        } else {
            clickCount = 1;
        }

        lastClickTime = now;
        lastClickPos = clickPos;

        if (clickCount == 3) {
            // Triple-click: select line
            Cursor cursor;
            cursor.selection.start = TextPosition(clickPos.line, 0);
            cursor.selection.end = TextPosition(clickPos.line, static_cast<int>(lines[clickPos.line].size()));
            cursor.position = cursor.selection.end;

            if (!ctrl) {
                cursors.clear();
                primaryCursor = 0;
            }
            cursors.push_back(cursor);
            clickCount = 0;
        } else if (clickCount == 2) {
            // Double-click: select word
            TextPosition wordStart = findWordStart(clickPos);
            TextPosition wordEnd = findWordEnd(clickPos);

            Cursor cursor;
            cursor.selection.start = wordStart;
            cursor.selection.end = wordEnd;
            cursor.position = wordEnd;

            if (!ctrl) {
                cursors.clear();
                primaryCursor = 0;
            }
            cursors.push_back(cursor);
        } else {
             // Check for drag start on existing selection
            bool insideSelection = false;
            if (!ctrl && !shift && !alt) {
                for (const auto& cursor : cursors) {
                    if (!cursor.selection.isEmpty()) {
                        TextPosition min = cursor.selection.getMin();
                        TextPosition max = cursor.selection.getMax();
                        if (clickPos >= min && clickPos < max) {
                            insideSelection = true;
                            break;
                        }
                    }
                }
            }

            if (insideSelection) {
                mayDragText = true;
            } else {
                // Single click
                if (alt && shift && !cursors.empty()) {
                    // Box selection / Column selection
                    TextPosition anchor = cursors[primaryCursor].selection.start;
                    int minLine = std::min(anchor.line, clickPos.line);
                    int maxLine = std::max(anchor.line, clickPos.line);

                    cursors.clear();
                    primaryCursor = 0;

                    int anchorVisual = byteOffsetToVisualColumn(anchor.line, anchor.column);
                    int clickVisual = byteOffsetToVisualColumn(clickPos.line, clickPos.column);

                    for (int l = minLine; l <= maxLine; ++l) {
                        Cursor cursor;
                        cursor.position = TextPosition(l, visualColumnToByteOffset(l, clickVisual));
                        cursor.selection.start = TextPosition(l, visualColumnToByteOffset(l, anchorVisual));
                        cursor.selection.end = cursor.position;

                        cursors.push_back(cursor);
                        if (l == clickPos.line) {
                            primaryCursor = cursors.size() - 1;
                        }
                    }
                } else if (ctrl && !shift) {
                    // Ctrl+Click: Add cursor
                    Cursor cursor;
                    cursor.position = clickPos;
                    cursor.selection.start = clickPos;
                    cursor.selection.end = clickPos;
                    cursors.push_back(cursor);
                } else if (shift && !alt && !cursors.empty()) {
                    // Extend selection
                    cursors[primaryCursor].selection.end = clickPos;
                    cursors[primaryCursor].position = clickPos;
                } else {
                    // Move cursor
                    cursors.clear();
                    Cursor cursor;
                    cursor.position = clickPos;
                    cursor.selection.start = clickPos;
                    cursor.selection.end = clickPos;
                    cursors.push_back(cursor);
                    primaryCursor = 0;
                }

                isDragging = true;
            }
        }

        CloseAutoComplete();
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (mayDragText && !isDraggingText) {
             isDraggingText = true;
             mayDragText = false;
        }
    }

    if (isDraggingText) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        if (!cursors.empty()) {
            cursors[primaryCursor].position = screenToText(mousePos, contentPos);
        }
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && isDragging) {
        TextPosition dragPos = screenToText(mousePos, contentPos);
        if (!cursors.empty()) {
            cursors[primaryCursor].selection.end = dragPos;
            cursors[primaryCursor].position = dragPos;
        }
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        ImGui::SetWindowFocus(); // ImGui only focuses on left click
        isMiddleDragging = true;
        middleDragLine = screenToText(mousePos, contentPos).line;
        middleDragX = mousePos.x - contentPos.x - textStartX;
        CloseAutoComplete();
    }

    if (isMiddleDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            columnSelect(mousePos, contentPos);
        } else {
            isMiddleDragging = false;
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (isDraggingText) {
            TextPosition dropPos = screenToText(mousePos, contentPos);
            moveSelectedText(dropPos);
            isDraggingText = false;
        } else if (mayDragText) {
            // Was just a click inside selection
            cursors.clear();
            Cursor cursor;
            cursor.position = lastClickPos;
            cursor.selection.start = lastClickPos;
            cursor.selection.end = lastClickPos;
            cursors.push_back(cursor);
            primaryCursor = 0;
            mayDragText = false;
        }

        isDragging = false;
    }

    if (showParamHint && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        updateParamHint();
    }

    if (!suggestionsHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        placeCursorAtClick(screenToText(mousePos, contentPos));
        CloseAutoComplete();
        showContextMenu = true;
        contextMenuPos = mousePos;
    }
}

void CustomTextEditor::handleTextInput() {
    if (readOnly) return;

    ImGuiIO& io = ImGui::GetIO();

    if (io.InputQueueCharacters.Size > 0) {
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            ImWchar c = io.InputQueueCharacters[i];

            if (c < 32) continue; // X11 sends Tab as a character, but the key handler indents
            if (c == 127 || (c >= 0x80 && c <= 0x9F)) continue; // DEL and C1 controls

            bool handled = false;

            // Check for overtyping closing brackets
            char ch = static_cast<char>(c);
            if (c < 128 && (ch == ')' || ch == ']' || ch == '}')) {
                bool allMatch = true;
                for (const auto& cursor : cursors) {
                    if (getCharAt(cursor.position) != ch) {
                        allMatch = false;
                        break;
                    }
                }

                if (allMatch && !cursors.empty()) {
                    for (auto& cursor : cursors) {
                        moveCursor(cursor, 0, 1, false);
                    }
                    mergeCursors();
                    handled = true;

                    if (ch == ')' && showParamHint) {
                        updateParamHint();
                    }
                }
            }

            if (!handled) {
                char utf8[5] = {};
                if (c < 0x80) {
                    utf8[0] = static_cast<char>(c);
                } else if (c < 0x800) {
                    utf8[0] = static_cast<char>(0xC0 | (c >> 6));
                    utf8[1] = static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    utf8[0] = static_cast<char>(0xE0 | (c >> 12));
                    utf8[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    utf8[2] = static_cast<char>(0x80 | (c & 0x3F));
                }

                InsertText(utf8);
                reindentAfterTyping(static_cast<int>(std::strlen(utf8)));

                // Auto-close brackets
                if (c == '(' || c == '[' || c == '{') {
                    char closing = getMatchingBracket(static_cast<char>(c));

                    // The cursors go back between the brackets after the closing one is typed
                    auto positions = cursors;
                    InsertText(std::string(1, closing), false);
                    for (size_t j = 0; j < cursors.size() && j < positions.size(); ++j) {
                        cursors[j] = positions[j];
                    }

                    if (c == '(') {
                        triggerParamHint();
                    }
                } else if (showParamHint) {
                    updateParamHint();
                }

                bool shouldTrigger = false;
                if (autoComplete) {
                    if ((c < 128 && std::isalnum(static_cast<unsigned char>(c))) || c == '_' || c == '.' || c == ':') {
                        shouldTrigger = true;
                    } else if (c == '>' && language == SyntaxLanguage::Cpp) {
                        // Trigger for '->' operator
                        const auto& pos = cursors[primaryCursor].position;
                        if (pos.column >= 2 && pos.line < static_cast<int>(lines.size())) {
                            if (lines[pos.line][pos.column - 2] == '-') {
                                shouldTrigger = true;
                            }
                        }
                    }
                }
                if (shouldTrigger) {
                    TriggerAutoComplete();
                } else if (showAutoComplete) {
                    CloseAutoComplete();
                }
            }
        }

        scrollToCursor();
    }
}

void CustomTextEditor::renderLineNumbers(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine) {
    if (!showLineNumbers) return;

    ImU32 color = ImGui::ColorConvertFloat4ToU32(lineNumberColor);

    for (int i = startLine; i <= endLine && i < static_cast<int>(lines.size()); ++i) {
        float y = origin.y + i * lineHeight;

        char lineNum[16];
        snprintf(lineNum, sizeof(lineNum), "%*d", lineNumberDigits, i + 1);

        drawList->AddText(ImVec2(origin.x + leftMargin, y + textOffsetY), color, lineNum);
    }
}

float CustomTextEditor::diffGutterX(const ImVec2& origin) const {
    const float numbersEnd = showLineNumbers ? leftMargin + lineNumberDigits * charWidth : 0.0f;
    return std::round(origin.x + numbersEnd + leftMargin * 0.5f);
}

int CustomTextEditor::diffChangeAt(const ImVec2& point, const ImVec2& origin) const {
    const float x = diffGutterX(origin);
    const float barWidth = std::round(Theme::dpi(3.0f));
    // The bar alone is hard to hit
    const float reach = Theme::dpi(3.0f);
    if (point.x < x - reach || point.x > x + barWidth + reach) return -1;

    const float triangleSize = std::round(Theme::dpi(4.0f));
    for (int i = 0; i < static_cast<int>(diffChanges.size()); i++) {
        const LineChange& change = diffChanges[i];
        float top = origin.y + change.modifiedStart * lineHeight;
        float bottom = origin.y + change.modifiedEnd * lineHeight;
        if (change.modifiedStart == change.modifiedEnd) {
            const float y = std::max(top, origin.y + triangleSize);
            top = y - triangleSize;
            bottom = y + triangleSize;
        }
        if (point.y >= top && point.y < bottom) return i;
    }
    return -1;
}

void CustomTextEditor::renderDiffGutter(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine) {
    if (diffChanges.empty()) return;

    // VSCode's 3px bars and 4px triangles, twice as large under the mouse
    const float x = diffGutterX(origin);
    for (int i = 0; i < static_cast<int>(diffChanges.size()); i++) {
        const LineChange& change = diffChanges[i];
        if (change.modifiedEnd < startLine) continue;
        if (change.modifiedStart > endLine + 1) break;

        const float scale = (i == diffHoverChange || i == diffPeekChange) ? 2.0f : 1.0f;
        const float top = origin.y + change.modifiedStart * lineHeight;

        if (change.modifiedStart == change.modifiedEnd) {
            // On the edge where lines were removed
            const float size = std::round(Theme::dpi(4.0f) * scale);
            const float y = std::max(top, origin.y + size);
            drawList->AddTriangleFilled(ImVec2(x, y - size), ImVec2(x + size, y), ImVec2(x, y + size), DIFF_DELETED_COLOR);
            continue;
        }

        const ImVec2 min(x, top);
        const ImVec2 max(x + std::round(Theme::dpi(3.0f) * scale), origin.y + change.modifiedEnd * lineHeight);
        if (change.originalStart == change.originalEnd) {
            drawList->AddRectFilled(min, max, DIFF_ADDED_COLOR);
        } else {
            drawHatchedBar(drawList, min, max, DIFF_MODIFIED_COLOR);
        }
    }
}

void CustomTextEditor::renderDiffOverview() {
    if (diffChanges.empty()) return;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window->ScrollbarY) return;

    // The scrollbar doubles as an overview ruler, the changes in its left third
    const ImRect bar = ImGui::GetWindowScrollbarRect(window, ImGuiAxis_Y);
    const float contentHeight = window->ContentSize.y + window->WindowPadding.y * 2.0f;
    if (contentHeight <= 0.0f || bar.GetHeight() <= 0.0f) return;

    const float scale = bar.GetHeight() / contentHeight;
    const float laneEnd = bar.Min.x + std::round(bar.GetWidth() / 3.0f);
    const float minHeight = Theme::dpi(2.0f);

    // The content clip rect leaves the scrollbar out
    ImRect clip = bar;
    clip.ClipWithFull(window->OuterRectClipped);
    if (clip.GetWidth() <= 0.0f || clip.GetHeight() <= 0.0f) return;

    ImDrawList* drawList = window->DrawList;
    drawList->PushClipRect(clip.Min, clip.Max, false);
    for (const LineChange& change : diffChanges) {
        float top = bar.Min.y + change.modifiedStart * lineHeight * scale;
        float bottom = bar.Min.y + change.modifiedEnd * lineHeight * scale;
        if (bottom - top < minHeight) {
            const float center = (top + bottom) * 0.5f;
            top = center - minHeight * 0.5f;
            bottom = center + minHeight * 0.5f;
        }

        // 60% opaque, as in VSCode
        const ImU32 color = (diffColor(change) & ~IM_COL32_A_MASK) | (0x99u << IM_COL32_A_SHIFT);
        drawList->AddRectFilled(ImVec2(bar.Min.x, top), ImVec2(laneEnd, bottom), color);
    }
    drawList->PopClipRect();
}

void CustomTextEditor::updateDiffPeek(const ImVec2& origin) {
    // An edit that moved the change closes its popup
    if (diffPeekChange >= 0 && (diffPeekChange >= static_cast<int>(diffChanges.size()) || diffChanges[diffPeekChange] != diffPeekKey)) {
        diffPeekChange = -1;
    }

    const double now = ImGui::GetTime();
    const bool overEditor = ImGui::IsWindowHovered() && !suggestionsHovered && !showContextMenu;
    const int hovered = overEditor ? diffChangeAt(ImGui::GetIO().MousePos, origin) : -1;
    if (hovered >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }

    if (hovered != diffHoverChange) {
        diffHoverChange = hovered;
        diffHoverStart = now;
    }

    // A mouse just crossing the gutter opens nothing
    const bool dragging = isDragging || isDraggingText || isMiddleDragging || ImGui::IsAnyMouseDown();
    if (hovered >= 0 && hovered != diffPeekChange && !dragging && now - diffHoverStart >= DIFF_PEEK_DELAY) {
        diffPeekChange = hovered;
        diffPeekKey = diffChanges[hovered];
        diffPeekVersion = 0;
        diffPeekHovered = false;
        diffPeekLeaveTime = -1.0;
    }

    // Open while the mouse is on the marker or the popup, with time to go from one to the other
    if (diffPeekChange >= 0) {
        if (hovered == diffPeekChange || diffPeekHovered) {
            diffPeekLeaveTime = -1.0;
        } else if (diffPeekLeaveTime < 0.0) {
            diffPeekLeaveTime = now;
        } else if (now - diffPeekLeaveTime > DIFF_PEEK_GRACE) {
            diffPeekChange = -1;
        }
    }

    // The loop may be idling while a wait runs out
    if ((hovered >= 0 && hovered != diffPeekChange) || (diffPeekChange >= 0 && diffPeekLeaveTime >= 0.0)) {
        Backend::getApp().requestFrame();
    }
}

void CustomTextEditor::buildDiffPeekRows(const LineChange& change) {
    diffPeekRows.clear();

    // The base lines need tokens of their own
    OpenToken open;
    for (int i = change.originalStart; i < change.originalEnd && i < static_cast<int>(diffBaseLines.size()); i++) {
        DiffPeekRow row{false, diffBaseLines[i], {}, 0, 0};
        tokenizeText(row.text, open, row.tokens);
        diffPeekRows.push_back(std::move(row));
    }
    const int removed = static_cast<int>(diffPeekRows.size());

    for (int i = change.modifiedStart; i < change.modifiedEnd && i < static_cast<int>(lines.size()); i++) {
        diffPeekRows.push_back({true, lines[i], i < static_cast<int>(lineTokens.size()) ? lineTokens[i] : std::vector<Token>(), 0, 0});
    }
    const int added = static_cast<int>(diffPeekRows.size()) - removed;

    // Paired lines mark what changed between them
    for (int i = 0; i < std::min(removed, added); i++) {
        DiffPeekRow& before = diffPeekRows[i];
        DiffPeekRow& after = diffPeekRows[removed + i];
        changedBytes(before.text, after.text, before.changedStart, before.changedEnd, after.changedEnd);
        after.changedStart = before.changedStart;
    }

    diffPeekTextWidth = 0.0f;
    for (const DiffPeekRow& row : diffPeekRows) {
        diffPeekTextWidth = std::max(diffPeekTextWidth, measureText(row.text.c_str(), row.text.c_str() + row.text.size()));
    }
}

void CustomTextEditor::renderDiffPeek(const ImVec2& origin) {
    if (diffPeekChange < 0 || diffPeekChange >= static_cast<int>(diffChanges.size())) {
        diffPeekHovered = false;
        return;
    }

    const LineChange change = diffChanges[diffPeekChange];
    if (diffPeekVersion != diffVersion) {
        buildDiffPeekRows(change);
        diffPeekVersion = diffVersion;
    }

    const int removed = change.originalEnd - change.originalStart;
    const int added = change.modifiedEnd - change.modifiedStart;
    auto lineRange = [](int start, int end) {
        return end - start == 1 ? "line " + std::to_string(start + 1)
                                 : "lines " + std::to_string(start + 1) + "-" + std::to_string(end);
    };
    std::string title;
    if (removed == 0) {
        title = "Added " + lineRange(change.modifiedStart, change.modifiedEnd);
    } else if (added == 0) {
        title = "Removed " + std::to_string(removed) + (removed == 1 ? " line" : " lines");
    } else {
        title = "Changed " + lineRange(change.modifiedStart, change.modifiedEnd);
    }
    const char* revertLabel = ICON_FA_ROTATE_LEFT;

    const ImRect editorRect = ImGui::GetCurrentWindow()->InnerRect;

    // Whole pixels, ImGui truncates window sizes and a lost fraction shows scrollbars
    const float spacing = std::round(Theme::dpi(6.0f));
    const float spacingY = std::round(Theme::dpi(3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(spacing, spacingY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacingY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::round(Theme::dpi(10.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, backgroundColor);

    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::PushFont(ImGui::GetIO().FontDefault);
    const float headerHeight = std::ceil(readOnly ? ImGui::GetTextLineHeight() : ImGui::GetFrameHeight());
    const float revertWidth = readOnly ? 0.0f : ImGui::CalcTextSize(revertLabel).x + 4.0f;
    const float headerWidth = ImGui::CalcTextSize(title.c_str()).x + style.ItemSpacing.x * 4.0f + revertWidth;
    ImGui::PopFont();

    const float signWidth = charWidth * 2.0f;
    const float contentWidth = std::ceil(signWidth + diffPeekTextWidth + charWidth);
    const float contentHeight = diffPeekRows.size() * lineHeight;
    const float maxBodyWidth = std::floor(std::max(Theme::dpi(240.0f), editorRect.GetWidth() - Theme::dpi(8.0f)) - spacing * 2.0f);

    float bodyHeight = std::min(contentHeight, std::floor(std::max(lineHeight * 3.0f, editorRect.GetHeight() * 0.45f)));
    float bodyWidth = std::ceil(std::max(contentWidth + (contentHeight > bodyHeight ? style.ScrollbarSize : 0.0f), headerWidth));
    if (bodyWidth > maxBodyWidth) {
        bodyWidth = maxBodyWidth;
        bodyHeight += style.ScrollbarSize;
    }
    const float width = bodyWidth + spacing * 2.0f;
    const float height = headerHeight + bodyHeight + spacingY * 3.0f;

    float top = origin.y + change.modifiedStart * lineHeight;
    float bottom = origin.y + change.modifiedEnd * lineHeight;
    if (added == 0) {
        const float triangleSize = std::round(Theme::dpi(4.0f));
        const float y = std::max(top, origin.y + triangleSize);
        top = y - triangleSize;
        bottom = y + triangleSize;
    }

    // Below the change, above it when there is no room
    ImVec2 pos(diffGutterX(origin), bottom);
    if (pos.y + height > editorRect.Max.y) {
        pos.y = top - height;
        if (pos.y < editorRect.Min.y) pos.y = std::max(editorRect.Min.y, editorRect.Max.y - height);
    }
    pos.x = std::max(editorRect.Min.x, std::min(pos.x, editorRect.Max.x - width));

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(width, height));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;

    // One per editor, two can show side by side
    char windowName[48];
    snprintf(windowName, sizeof(windowName), "##DiffPeek%p", static_cast<void*>(this));

    bool revert = false;
    if (ImGui::Begin(windowName, nullptr, flags)) {
        diffPeekHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        ImGui::PushFont(ImGui::GetIO().FontDefault);
        if (!readOnly) {
            ImGui::AlignTextToFramePadding();
        }
        ImGui::TextUnformatted(title.c_str());
        if (!readOnly) {
            // Same arrow as the reset to default buttons of the Properties
            ImGui::SameLine(ImGui::GetContentRegionMax().x - revertWidth);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, style.FramePadding.y));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            revert = ImGui::Button(revertLabel);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            ImGui::SetItemTooltip("Revert to the staged lines");
        }
        ImGui::PopFont();

        if (ImGui::BeginChild("##DiffPeekBody", ImVec2(bodyWidth, bodyHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav)) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float rowWidth = std::max(contentWidth, ImGui::GetContentRegionAvail().x);
            ImGui::Dummy(ImVec2(contentWidth, contentHeight));

            // Removed lines first, then what replaced them
            const int first = std::max(0, static_cast<int>(ImGui::GetScrollY() / lineHeight));
            const int last = std::min(static_cast<int>(diffPeekRows.size()), first + static_cast<int>(bodyHeight / lineHeight) + 2);
            for (int i = first; i < last; i++) {
                const DiffPeekRow& row = diffPeekRows[i];
                const float y = start.y + i * lineHeight;
                const float textX = start.x + signWidth;

                drawList->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + rowWidth, y + lineHeight),
                                        row.added ? DIFF_ADDED_LINE_BG : DIFF_REMOVED_LINE_BG);
                if (row.changedEnd > row.changedStart) {
                    const char* text = row.text.c_str();
                    drawList->AddRectFilled(ImVec2(textX + measureText(text, text + row.changedStart), y),
                                            ImVec2(textX + measureText(text, text + row.changedEnd), y + lineHeight),
                                            row.added ? DIFF_ADDED_TEXT_BG : DIFF_REMOVED_TEXT_BG);
                }
                drawList->AddText(ImVec2(start.x + charWidth * 0.5f, y + textOffsetY),
                                  row.added ? DIFF_ADDED_COLOR : DIFF_DELETED_COLOR, row.added ? "+" : "-");
                renderLineTokens(drawList, ImVec2(textX, y + textOffsetY), row.text, row.tokens);
            }
        }
        ImGui::EndChild();

        // A click here gives the keyboard back to the editor
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyMouseDown()) {
            pendingFocus = true;
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);

    if (revert) {
        revertDiffChange(change);
    }
}

void CustomTextEditor::revertDiffChange(const LineChange& change) {
    // A diff that has not caught up with the last edit has stale ranges
    if (readOnly || diffDirty || change.modifiedEnd > static_cast<int>(lines.size()) ||
        change.originalEnd > static_cast<int>(diffBaseLines.size())) {
        return;
    }

    if (showParamHint) closeParamHint();
    CloseAutoComplete();

    std::vector<std::string> reverted(lines.begin(), lines.begin() + change.modifiedStart);
    reverted.insert(reverted.end(), diffBaseLines.begin() + change.originalStart, diffBaseLines.begin() + change.originalEnd);
    reverted.insert(reverted.end(), lines.begin() + change.modifiedEnd, lines.end());

    addUndoRecord();
    lines = std::move(reverted);

    // Carets below the change follow their text, those inside go to its first line
    const int shift = (change.originalEnd - change.originalStart) - (change.modifiedEnd - change.modifiedStart);
    auto follow = [&](TextPosition& pos) {
        if (pos.line >= change.modifiedEnd) {
            pos.line += shift;
        } else if (pos.line >= change.modifiedStart) {
            pos = TextPosition(change.modifiedStart, 0);
        }
    };
    for (Cursor& cursor : cursors) {
        follow(cursor.position);
        follow(cursor.selection.start);
        follow(cursor.selection.end);
    }
    ensureValidCursors();
    mergeCursors();

    tokenizeAll();
    finalizeUndoRecord();

    diffPeekChange = -1;
    pendingFocus = true;
}

void CustomTextEditor::renderSelections(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine) {
    ImU32 selColor = ImGui::ColorConvertFloat4ToU32(selectionColor);

    for (const auto& cursor : cursors) {
        if (cursor.selection.isEmpty()) continue;

        TextPosition selStart = cursor.selection.getMin();
        TextPosition selEnd = cursor.selection.getMax();

        for (int line = std::max(startLine, selStart.line); 
             line <= std::min(endLine, selEnd.line) && line < static_cast<int>(lines.size()); 
             ++line) {
            int startCol = (line == selStart.line) ? selStart.column : 0;
            int endCol = (line == selEnd.line) ? selEnd.column : static_cast<int>(lines[line].size());

            float x1 = origin.x + textStartX + byteOffsetToPixelX(line, startCol);
            float x2 = origin.x + textStartX + byteOffsetToPixelX(line, endCol);

            // If the selection extends past this line, highlight the newline character
            if (line < selEnd.line) {
                x2 += charWidth;
            }

            float y = origin.y + line * lineHeight;

            if (x2 > x1) {
                drawList->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + lineHeight), selColor);
            }
        }
    }
}

void CustomTextEditor::renderSearchHighlights(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine) {
    if (searchResults.empty()) return;

    ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(searchHighlightColor);

    for (const auto& result : searchResults) {
        if (result.line < startLine || result.line > endLine) continue;

        int endColumn = result.column + static_cast<int>(searchText.size());
        float x = origin.x + textStartX + byteOffsetToPixelX(result.line, result.column);
        float y = origin.y + result.line * lineHeight;
        float width = byteOffsetToPixelX(result.line, endColumn) -
                      byteOffsetToPixelX(result.line, result.column);

        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + lineHeight), highlightColor);
    }
}

void CustomTextEditor::renderText(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine) {
    static const std::vector<Token> noTokens;

    for (int i = startLine; i <= endLine && i < static_cast<int>(lines.size()); ++i) {
        const auto& tokens = (i < static_cast<int>(lineTokens.size())) ? lineTokens[i] : noTokens;
        renderLineTokens(drawList, ImVec2(origin.x + textStartX, origin.y + i * lineHeight + textOffsetY), lines[i], tokens);
    }
}

void CustomTextEditor::renderLineTokens(ImDrawList* drawList, ImVec2 pos, const std::string& line, const std::vector<Token>& tokens) const {
    const ImU32 defaultColor = ImGui::ColorConvertFloat4ToU32(palette[static_cast<int>(TokenType::Default)]);
    const char* text = line.c_str();

    int lastEnd = 0;
    for (const auto& token : tokens) {
        if (token.start > lastEnd) {
            drawList->AddText(pos, defaultColor, text + lastEnd, text + token.start);
            pos.x += measureText(text + lastEnd, text + token.start);
        }

        lastEnd = token.start + token.length;
        drawList->AddText(pos, ImGui::ColorConvertFloat4ToU32(palette[static_cast<int>(token.type)]), text + token.start, text + lastEnd);
        pos.x += measureText(text + token.start, text + lastEnd);
    }

    if (lastEnd < static_cast<int>(line.size())) {
        drawList->AddText(pos, defaultColor, text + lastEnd, text + line.size());
    }
}

void CustomTextEditor::renderCursors(ImDrawList* drawList, const ImVec2& origin) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;

    float time = ImGui::GetTime();
    bool showCursor = isDraggingText || isMiddleDragging || (fmod(time, 1.0f) < 0.5f);

    // The loop idles without input, so the blink has to ask for its frames
    if (showCursor != cursorBlinkOn) {
        cursorBlinkOn = showCursor;
        Backend::getApp().requestFrame();
    }

    if (!showCursor) return;

    ImU32 color = ImGui::ColorConvertFloat4ToU32(cursorColor);

    for (const auto& cursor : cursors) {
        ImVec2 pos = textToScreen(cursor.position, origin);
        drawList->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x, pos.y + lineHeight), color, 2.0f);
    }
}

void CustomTextEditor::renderMatchingBrackets(ImDrawList* drawList, const ImVec2& origin) {
    if (!matchBrackets || cursors.empty()) return;

    TextPosition cursorPos = cursors[primaryCursor].position;

    // Check character at cursor and before cursor
    TextPosition checkPos = cursorPos;
    char c = getCharAt(checkPos);

    if (!isOpenBracket(c) && !isCloseBracket(c)) {
        if (cursorPos.column > 0) {
            checkPos.column--;
            c = getCharAt(checkPos);
        }
    }

    if (!isOpenBracket(c) && !isCloseBracket(c)) return;

    TextPosition matchPos;
    if (findMatchingBracket(checkPos, matchPos) != '\0') {
        ImU32 color = ImGui::ColorConvertFloat4ToU32(matchingBracketColor);

        ImVec2 pos1 = textToScreen(checkPos, origin);
        drawList->AddRectFilled(pos1, ImVec2(pos1.x + charWidth, pos1.y + lineHeight), color);

        ImVec2 pos2 = textToScreen(matchPos, origin);
        drawList->AddRectFilled(pos2, ImVec2(pos2.x + charWidth, pos2.y + lineHeight), color);
    }
}

void CustomTextEditor::renderSuggestions(const ImVec2& origin) {
    suggestionsHovered = false;

    if (!showAutoComplete || currentSuggestions.empty()) return;

    // Position popup directly under the current word being typed
    TextPosition pos = autoCompleteAnchor;
    ImVec2 screenPos = textToScreen(pos, origin);
    screenPos.y += lineHeight + 1.0f;

    float popupWidth = 350.0f;
    float itemHeight = ImGui::GetTextLineHeight() + 4.0f;
    float maxVisibleItems = 10.0f;
    float popupMaxHeight = itemHeight * maxVisibleItems + 8.0f;
    float popupActualHeight = std::min(popupMaxHeight, itemHeight * currentSuggestions.size() + 8.0f);

    // Ensure popup doesn't go off-screen
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    if (screenPos.x + popupWidth > displaySize.x) {
        screenPos.x = displaySize.x - popupWidth - 10;
    }
    if (screenPos.x < 0) {
        screenPos.x = 10;
    }

    // If popup would go below screen, show it above the cursor instead
    if (screenPos.y + popupActualHeight > displaySize.y) {
        screenPos.y = textToScreen(pos, origin).y - popupActualHeight - 2.0f;
    }

    ImGui::SetNextWindowPos(screenPos);
    ImGui::SetNextWindowSize(ImVec2(popupWidth, popupActualHeight));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(4.0f, 4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Theme::dpi(ImVec2(4.0f, 0.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Theme::dpi(10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.12f, 0.12f, 0.12f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.4f, 0.4f, 0.4f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.6f, 0.6f, 0.6f, 0.8f));

    if (ImGui::Begin("##SemanticSuggestions", nullptr, flags)) {
        // Check if mouse is hovering this window to block editor scroll
        suggestionsHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | 
                                                     ImGuiHoveredFlags_ChildWindows |
                                                     ImGuiHoveredFlags_RootAndChildWindows);

        // Applying clears currentSuggestions, so it waits until the list is done being read
        int clickedIndex = -1;

        for (int i = 0; i < static_cast<int>(currentSuggestions.size()); ++i) {
            const auto& item = currentSuggestions[i];

            bool selected = (i == suggestionIndex);

            ImGui::PushID(i);

            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            float availWidth = ImGui::GetContentRegionAvail().x;

            // VSCode-like dark blue selection color
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.016f, 0.224f, 0.369f, 1.0f)); // #04395e
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.17f, 0.27f, 0.44f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.016f, 0.224f, 0.369f, 1.0f));

            if (ImGui::Selectable("##item", selected, ImGuiSelectableFlags_None, ImVec2(availWidth, itemHeight))) {
                clickedIndex = i;
            }
            // Hover moves the selection, as it does in VSCode, so the side panel follows the mouse
            if (ImGui::IsItemHovered()) suggestionIndex = i;

            ImGui::PopStyleColor(3);

            // Scroll to selected item only if navigation occurred
            if (selected && scrollToSuggestion) {
                ImGui::SetScrollHereY();
            }

            ImVec2 nextItemPos = ImGui::GetCursorPos();

            // Draw content on top of selectable - center vertically
            float textY = cursorPos.y + (itemHeight - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 4.0f, textY));

            ImVec4 iconColor = SemanticSuggestions::GetKindColor(item.kind);
            const char* icon = SemanticSuggestions::GetKindIcon(item.kind);
            ImGui::TextColored(iconColor, "%s", icon);
            ImGui::SameLine();

            ImGui::TextUnformatted(item.label.c_str());

            // Detail (right-aligned, dimmed), without its "Parent.name" start when too long
            if (!item.detail.empty()) {
                float labelWidth = ImGui::CalcTextSize(item.label.c_str()).x;
                float iconWidth = ImGui::CalcTextSize(icon).x + ImGui::GetStyle().ItemSpacing.x;
                std::string detail = item.detail;
                float detailWidth = ImGui::CalcTextSize(detail.c_str()).x;
                float spacing = availWidth - labelWidth - iconWidth - detailWidth - 20;

                if (spacing <= 20) {
                    detail = detailAfterName(detail, item.parentType, item.label);
                    spacing += detailWidth - ImGui::CalcTextSize(detail.c_str()).x;
                }

                if (!detail.empty() && spacing > 20) {
                    ImGui::SameLine(0, spacing);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::TextUnformatted(detail.c_str());
                    ImGui::PopStyleColor();
                }
            }

            ImGui::SetCursorPos(nextItemPos);

            ImGui::PopID();

            if (ImGui::IsItemHovered() && !item.documentation.empty()) {
                ImGui::SetTooltip("%s", item.documentation.c_str());
            }
        }

        if (clickedIndex >= 0) {
            suggestionIndex = clickedIndex;
            applySuggestion();
        }

        scrollToSuggestion = false;
    }
    ImGui::End();

    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(4);

    renderSuggestionDetails(screenPos, popupWidth, popupActualHeight);
}

void CustomTextEditor::renderSuggestionDetails(const ImVec2& listPos, float listWidth, float listHeight) {
    if (!showAutoComplete || currentSuggestions.empty()) return;
    if (suggestionIndex < 0 || suggestionIndex >= static_cast<int>(currentSuggestions.size())) return;

    const SuggestionItem& item = currentSuggestions[suggestionIndex];

    const float width = Theme::dpi(280.0f);
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImVec2 pos(listPos.x + listWidth + 2.0f, std::min(listPos.y, displaySize.y - listHeight - Theme::dpi(8.0f)));
    // The panel reads better beside the list, so when there is no room on the right it
    // goes to the left, and when there is none there either the list alone stays
    if (pos.x + width > displaySize.x) pos.x = listPos.x - width - 2.0f;
    if (pos.x < 0) return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushID("SuggestionDetails");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(8.0f, 6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Theme::dpi(ImVec2(4.0f, 4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));

    if (ImGui::Begin("##SuggestionDetails", nullptr, flags)) {
        ImGui::PushTextWrapPos(pos.x + width - Theme::dpi(8.0f));

        ImGui::TextColored(SemanticSuggestions::GetKindColor(item.kind), "%s %s",
                           SemanticSuggestions::GetKindIcon(item.kind), item.label.c_str());

        std::string origin = SemanticSuggestions::GetKindName(item.kind);
        if (!item.namespaceName.empty()) origin = item.namespaceName + "::" + origin;
        if (!item.parentType.empty()) origin = item.parentType + "." + origin;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        ImGui::TextUnformatted(origin.c_str());

        if (!item.detail.empty()) ImGui::TextUnformatted(item.detail.c_str());
        ImGui::PopStyleColor();

        // The prose only when there is some: the generated API header carries signatures, not comments
        if (!item.documentation.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(item.documentation.c_str());
        }

        ImGui::PopTextWrapPos();
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
}

void CustomTextEditor::renderAutoComplete(const ImVec2& origin) {
    renderSuggestions(origin);
    renderParamHint(origin);
}

void CustomTextEditor::renderFindDialog(const ImVec2& editorPos, const ImVec2& editorSize) {
    if (!showFindDialog) return;

    const float padding = Theme::dpi(8.0f);
    const float spacing = Theme::dpi(4.0f);

    ImVec2 dialogPos(editorPos.x + editorSize.x - Theme::dpi(15.0f), editorPos.y + padding);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(8.0f, 6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, Theme::dpi(4.0f)));

    // Calculate dialog size first, then position it from the right edge
    ImGui::SetNextWindowPos(dialogPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f)); // Pivot at right edge

    if (ImGui::Begin("##FindDialog", nullptr, flags)) {

        const float inputWidth = Theme::dpi(180.0f);
        const float controlHeight = ImGui::GetFrameHeight();

        bool focusInput = ImGui::IsWindowAppearing();

        // Toggle replace — match find-field height so SameLine keeps the icon centered.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::IconButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::IconButtonActive);

        const float chevronWidth = Theme::dpi(16.0f);
        if (ImGui::Button(showReplaceInput ? ICON_FA_CHEVRON_DOWN "##ToggleReplace" : ICON_FA_CHEVRON_RIGHT "##ToggleReplace", ImVec2(chevronWidth, controlHeight))) {
            showReplaceInput = !showReplaceInput;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Replace");

        ImGui::SameLine();

        if (findRefocusInput) {
            ImGui::SetKeyboardFocusHere();
            findRefocusInput = false;
        } else if (focusInput) {
            ImGui::SetKeyboardFocusHere();
        }

        float findInputStartX = ImGui::GetCursorPosX();
        ImGui::BeginGroup();

        // Use UIUtils::searchInput for consistent look with Output window
        if (UIUtils::searchInput("##FindInput", "Find...", findInputBuffer, sizeof(findInputBuffer), false, &findCaseSensitive, inputWidth)) {
            if (strcmp(searchText.c_str(), findInputBuffer) != 0) {
                SetSearchText(findInputBuffer);
            }
        }

        bool enterPressed = ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter);
        bool shiftEnterPressed = ImGui::IsWindowFocused() && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Enter);

        ImGui::EndGroup();

        // Check if case sensitivity changed via the search input popup
        if (findLastCaseSensitive != findCaseSensitive) {
            updateSearchResults();
            findLastCaseSensitive = findCaseSensitive;
        }

        if (shiftEnterPressed) {
            FindPrevious();
            findRefocusInput = true;
        } else if (enterPressed) {
            FindNext();
            findRefocusInput = true;
        }

        ImGui::SameLine();

        char countBuf[32];
        if (!searchResults.empty()) {
            int current = currentSearchResult >= 0 ? currentSearchResult + 1 : 0;
            snprintf(countBuf, sizeof(countBuf), "%d/%d", current, static_cast<int>(searchResults.size()));
        } else {
            snprintf(countBuf, sizeof(countBuf), "0/0");
        }

        float countWidth = 50.0f;
        float textWidth = ImGui::CalcTextSize(countBuf).x;
        float offsetX = (countWidth - textWidth) * 0.5f;

        ImVec2 cursorPos = ImGui::GetCursorPos();
        ImGui::SetCursorPosX(cursorPos.x + offsetX);

        if (!searchResults.empty()) {
            ImGui::Text("%s", countBuf);
        } else if (strlen(findInputBuffer) > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", countBuf);
        } else {
            ImGui::TextDisabled("%s", countBuf);
        }

        ImGui::SameLine();
        ImGui::SetCursorPosX(cursorPos.x + countWidth);

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_CHEVRON_UP "##Prev") || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_UpArrow))) {
            FindPrevious();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous (Shift+Enter)");

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_CHEVRON_DOWN "##Next") || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
            FindNext();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next (Enter)");

        ImGui::SameLine(0.0f, 15.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::IconButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::IconButtonActive);
        if (ImGui::Button(ICON_FA_XMARK "##Close", ImVec2(controlHeight, controlHeight)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            CloseFind();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (Escape)");

        if (showReplaceInput) {
            ImGui::SetCursorPosX(findInputStartX);

            ImGui::SetNextItemWidth(inputWidth);
            ImGui::InputTextWithHint("##ReplaceInput", "Replace", replaceInputBuffer, sizeof(replaceInputBuffer));
            bool replaceInputFocused = ImGui::IsItemFocused();
            InputTextContextMenu::drawForLastItem(replaceInputBuffer, sizeof(replaceInputBuffer));

            bool replaceEnter = replaceInputFocused && ImGui::IsKeyPressed(ImGuiKey_Enter);
            bool replaceShiftEnter = replaceInputFocused && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Enter);

            ImGui::SameLine();

            if (ImGui::Button("Replace") || (replaceEnter && !replaceShiftEnter)) {
                ReplaceNext();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace Next (Enter)");

            ImGui::SameLine();

            if (ImGui::Button("All") || replaceShiftEnter) {
                ReplaceAll(findInputBuffer, replaceInputBuffer, findCaseSensitive);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace All (Shift+Enter)");
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
}

// One cursor per line of the box from the middle-drag anchor to the mouse
void CustomTextEditor::columnSelect(const ImVec2& mousePos, const ImVec2& contentPos) {
    float toX = mousePos.x - contentPos.x - textStartX;
    int toLine = screenToText(mousePos, contentPos).line;
    int firstLine = std::min(middleDragLine, toLine);
    int lastLine = std::min(std::max(middleDragLine, toLine), static_cast<int>(lines.size()) - 1);

    cursors.clear();
    for (int line = firstLine; line <= lastLine; ++line) {
        if (byteOffsetToPixelX(line, static_cast<int>(lines[line].size())) < std::min(middleDragX, toX)) continue;

        Cursor cursor;
        cursor.selection.start = TextPosition(line, pixelXToByteOffset(line, middleDragX));
        cursor.selection.end = TextPosition(line, pixelXToByteOffset(line, toX));
        cursor.position = cursor.selection.end;
        cursors.push_back(cursor);
    }

    if (cursors.empty()) AddCursor(toLine, pixelXToByteOffset(toLine, toX));

    primaryCursor = toLine < middleDragLine ? 0 : static_cast<int>(cursors.size()) - 1;
}

void CustomTextEditor::placeCursorAtClick(const TextPosition& clickPos) {
    for (const auto& cursor : cursors) {
        if (!cursor.selection.isEmpty()) {
            TextPosition min = cursor.selection.getMin();
            TextPosition max = cursor.selection.getMax();
            if (clickPos >= min && clickPos < max) {
                return;
            }
        }
    }

    cursors.clear();
    Cursor cursor;
    cursor.position = clickPos;
    cursor.selection.start = clickPos;
    cursor.selection.end = clickPos;
    cursors.push_back(cursor);
    primaryCursor = 0;
}

void CustomTextEditor::renderContextMenu() {
    if (!showContextMenu) {
        return;
    }

    ImGui::SetNextWindowPos(contextMenuPos, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushID(this);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(4.0f, 4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Theme::dpi(ImVec2(8.0f, 6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));

    if (ImGui::Begin("##ContextMenu", nullptr, flags)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            showContextMenu = false;
        } else if (!ImGui::IsWindowHovered() &&
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            showContextMenu = false;
        } else {
            const bool canUseClipboard = ImGui::GetClipboardText() != nullptr;
            const bool canEdit = !readOnly;
            const bool hasLine = !lines.empty();

            auto menuItem = [&](const char* label, const char* shortcut, bool enabled, auto action) {
                if (ImGui::MenuItem(label, shortcut, false, enabled)) {
                    action();
                    showContextMenu = false;
                }
            };

            menuItem(ICON_FA_ROTATE_LEFT " Undo", "Ctrl+Z", canEdit && CanUndo(), [&] { Undo(); });
            menuItem(ICON_FA_ROTATE_RIGHT " Redo", "Ctrl+Y", canEdit && CanRedo(), [&] { Redo(); });
            ImGui::Separator();
            menuItem(ICON_FA_SCISSORS " Cut", "Ctrl+X", canEdit && hasLine, [&] { Cut(); });
            menuItem(ICON_FA_COPY " Copy", "Ctrl+C", hasLine, [&] { Copy(); });
            menuItem(ICON_FA_PASTE " Paste", "Ctrl+V", canEdit && canUseClipboard, [&] { Paste(); });
            ImGui::Separator();
            menuItem(ICON_FA_I_CURSOR " Select All", "Ctrl+A", hasLine, [&] { SelectAll(); });
            ImGui::Separator();
            // The three rows the registry owns: the hint has to name the chord that
            // actually fires, not the VSCode default this file used to hardcode.
            const std::string commentHint = KeyBindings::primaryText("code.comment");
            const std::string blockHint = KeyBindings::primaryText("code.blockComment");
            const std::string formatHint = KeyBindings::primaryText("code.format");
            menuItem(ICON_FA_COMMENT_SLASH " Toggle Comment",
                     commentHint.empty() ? nullptr : commentHint.c_str(),
                     canEdit && !lineCommentPrefix().empty(), [&] { ToggleComment(); });
            menuItem(ICON_FA_COMMENT " Toggle Block Comment",
                     blockHint.empty() ? nullptr : blockHint.c_str(),
                     canEdit && !blockCommentStart().empty(), [&] { ToggleBlockComment(); });
            menuItem(ICON_FA_WAND_MAGIC_SPARKLES " Format Document",
                     formatHint.empty() ? nullptr : formatHint.c_str(),
                     canEdit && language != SyntaxLanguage::None, [&] { FormatDocument(); });
            ImGui::Separator();
            menuItem(ICON_FA_MAGNIFYING_GLASS " Find", "Ctrl+F", true, [&] { OpenFind(); });
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
}

void CustomTextEditor::Render(const char* title, const ImVec2& size, bool border) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, backgroundColor);

    ImGuiWindowFlags flags = ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoMove;

    // Without this, Up/Down in the suggestions popup would also scroll the editor
    flags |= ImGuiWindowFlags_NoNavInputs;

    ImVec2 contentSize = size;
    if (contentSize.x == 0) contentSize.x = ImGui::GetContentRegionAvail().x;
    if (contentSize.y == 0) contentSize.y = ImGui::GetContentRegionAvail().y;

    ImGui::PushID(this);
    if (ImGui::BeginChild(title, contentSize, border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None, flags)) {
        // Focus the child when only the parent is focused (a click on the tab or title bar).
        // A popup is skipped because stealing focus would close it.
        if (!ImGui::IsWindowFocused() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            ImGui::SetWindowFocus();
        }

        if (pendingFocus) {
            ImGui::SetWindowFocus();
            pendingFocus = false;
        }
        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize();
        // Cached widths are in pixels, so a font change expires them
        if (font != measureFont || fontSize != measureFontSize) {
            measureFont = font;
            measureFontSize = fontSize;
            maxLineWidthDirty = true;
            diffPeekVersion = 0;
        }
        charWidth = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, "X").x;
        lineHeight = std::round(fontSize * lineHeightFactor);
        // Center the font's real line box (ascent-descent), which can differ from fontSize
        ImFontBaked* baked = ImGui::GetFontBaked();
        textOffsetY = (lineHeight - (baked->Ascent - baked->Descent)) * 0.5f;

        if (showLineNumbers) {
            lineNumberDigits = 1;
            for (size_t n = lines.size(); n >= 10; n /= 10) lineNumberDigits++;
            lineNumberDigits = std::max(lineNumberDigits, 4);
            lineNumberWidth = lineNumberDigits * charWidth + leftMargin * 2;
        } else {
            lineNumberWidth = 0;
        }

        textStartX = lineNumberWidth + leftMargin;

        if (maxLineWidthDirty) {
            maxLineWidth = computeMaxLineWidth();
            maxLineWidthDirty = false;
        }
        updateDiff();
        float totalWidth = textStartX + maxLineWidth + 50.0f;
        float totalHeight = lines.size() * lineHeight + lineHeight;

        // The dummy gives the child its scrollable content size
        ImVec2 cursorBackup = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::Dummy(ImVec2(totalWidth, totalHeight));
        ImGui::SetCursorPos(cursorBackup);

        // A hovered suggestions popup keeps its own scroll, restoring ours stops the drift
        if (!suggestionsHovered) {
            scrollY = ImGui::GetScrollY();
            scrollX = ImGui::GetScrollX();
        } else {
            ImGui::SetScrollY(scrollY);
            ImGui::SetScrollX(scrollX);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();

        // Render autocomplete popup first (so it can capture mouse events)
        ImGui::PushFont(ImGui::GetIO().FontDefault);
        renderAutoComplete(origin);
        ImGui::PopFont();

        if (pendingScrollToCursor) {
            scrollToCursor();
            pendingScrollToCursor = false;
        }

        // Handle input (but not mouse input if suggestions popup is hovered)
        bool isHovered = ImGui::IsWindowHovered() && !suggestionsHovered;
        if (ImGui::IsWindowFocused()) {
            handleKeyboardInput();
            handleTextInput();
        }
        if ((isHovered || isDragging || isDraggingText || isMiddleDragging) && !suggestionsHovered) {
            handleMouseInput();
        }

        if (isDraggingText) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        } else if (isHovered) {
            // Change to arrow cursor only when hovering the scrollbars
            ImGuiStyle& style = ImGui::GetStyle();
            ImVec2 mPos = ImGui::GetIO().MousePos;
            ImVec2 wPos = ImGui::GetWindowPos();
            ImVec2 wSize = ImGui::GetWindowSize();

            float scrollbarSize = style.ScrollbarSize;
            float right = wPos.x + wSize.x;
            float bottom = wPos.y + wSize.y;

            bool overVScrollbar = (mPos.x >= right - scrollbarSize && mPos.x <= right);
            bool overHScrollbar = (mPos.y >= bottom - scrollbarSize && mPos.y <= bottom);

            ImGui::SetMouseCursor((overVScrollbar || overHScrollbar) ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_TextInput);
        }

        updateDiffPeek(origin);

        int startLine = static_cast<int>(scrollY / lineHeight);
        int endLine = startLine + static_cast<int>(contentSize.y / lineHeight) + 2;

        if (highlightCurrentLine && !cursors.empty()) {
            ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(currentLineColor);
            float y = origin.y + cursors[primaryCursor].position.line * lineHeight;
            drawList->AddRectFilled(
                ImVec2(origin.x, y),
                ImVec2(origin.x + contentSize.x, y + lineHeight),
                lineColor
            );
        }

        renderSearchHighlights(drawList, origin, startLine, endLine);
        renderSelections(drawList, origin, startLine, endLine);
        renderMatchingBrackets(drawList, origin);
        renderLineNumbers(drawList, origin, startLine, endLine);
        renderDiffGutter(drawList, origin, startLine, endLine);
        renderText(drawList, origin, startLine, endLine);
        renderCursors(drawList, origin);
        renderDiffOverview();
        renderDiffPeek(origin);

        ImGui::PushFont(ImGui::GetIO().FontDefault);
        renderFindDialog(ImGui::GetWindowPos(), contentSize);
        ImGui::PopFont();

    }
    ImGui::EndChild();
    ImGui::PopID();

    ImGui::PopStyleColor();

    // Context menu is a top-level window (ImGui popups do not work reliably inside this child).
    ImGui::PushFont(ImGui::GetIO().FontDefault);
    renderContextMenu();
    ImGui::PopFont();
}

} // namespace quantum::editor
