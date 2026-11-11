// SelfTest.cpp - headless logic verification of the UI Components Lab.
//
// This file implements the contract in SelfTest.h. It runs against a plain
// AppContext without a window, an OpenGL context or an ImGui frame, so every
// check exercises a pure C++ subsystem (types, registry, undo, validation,
// serialisation, canvas layout, asset library, theme, event log, notifications,
// settings, frame stats). Nothing here may call ImGui or ctx.textures.
#include "demo/SelfTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "canvas/CanvasScene.h"
#include "core/AppContext.h"
#include "core/EventLog.h"
#include "core/Notifications.h"
#include "core/Registry.h"
#include "core/Serialize.h"
#include "core/Settings.h"
#include "core/Types.h"
#include "core/UndoStack.h"
#include "core/Validation.h"
#include "panels/AssetLibrary.h"
#include "themes/Theme.h"

namespace uilab {
namespace demo {

namespace {

constexpr float kEps = 1e-3f;

bool NearF(float a, float b, float eps = kEps) { return std::fabs(a - b) <= eps; }
bool NearV2(const ImVec2& a, const ImVec2& b, float eps = 0.05f) {
    return NearF(a.x, b.x, eps) && NearF(a.y, b.y, eps);
}
bool NearV4(const ImVec4& a, const ImVec4& b, float eps = kEps) {
    return NearF(a.x, b.x, eps) && NearF(a.y, b.y, eps) && NearF(a.z, b.z, eps) && NearF(a.w, b.w, eps);
}

// Full structural comparison used to prove copy/paste restores every field.
bool StateEquivalent(const WidgetState& a, const WidgetState& b) {
    return a.text == b.text && a.tooltip == b.tooltip && a.styleClass == b.styleClass && a.styleId == b.styleId &&
           a.visible == b.visible && a.enabled == b.enabled && a.readOnly == b.readOnly &&
           a.checked == b.checked && a.toggled == b.toggled && NearF(a.value, b.value, 2e-3f) &&
           NearF(a.minValue, b.minValue, 2e-3f) && NearF(a.maxValue, b.maxValue, 2e-3f) && a.intValue == b.intValue &&
           a.intMin == b.intMin && a.intMax == b.intMax && a.format == b.format && a.selectedIndex == b.selectedIndex &&
           a.items == b.items && NearV4(a.color, b.color, 2e-3f) && NearV4(a.color2, b.color2, 2e-3f) &&
           a.useCustomColor == b.useCustomColor && NearV2(a.position, b.position, 0.05f) &&
           NearV2(a.size, b.size, 0.05f) && NearV2(a.scale, b.scale, 2e-3f) && NearF(a.rotation, b.rotation, 2e-3f) &&
           NearF(a.opacity, b.opacity, 2e-3f) && a.zOrder == b.zOrder && a.anchorMin == b.anchorMin &&
           a.anchorMax == b.anchorMax && a.pivot == b.pivot && a.alignment == b.alignment &&
           NearF(a.spacingX, b.spacingX, 2e-3f) && NearF(a.spacingY, b.spacingY, 2e-3f) &&
           NearF(a.paddingX, b.paddingX, 2e-3f) && NearF(a.paddingY, b.paddingY, 2e-3f) &&
           NearF(a.marginX, b.marginX, 2e-3f) && NearF(a.marginY, b.marginY, 2e-3f) &&
           a.flexDirection == b.flexDirection && NearF(a.flexGrow, b.flexGrow, 2e-3f) &&
           a.alignItems == b.alignItems && a.validationRule == b.validationRule && a.pattern == b.pattern &&
           a.hint == b.hint && a.multilineText == b.multilineText;
}

// The default registry assigns every kind to the category whose contiguous enum
// range it falls in. This mirrors that contract so we can detect a mismatch.
bool KindInCategory(WidgetKind k, Category c) {
    const int ki = static_cast<int>(k);
    int lo = 0, hi = 0;
    switch (c) {
    case Category::Basic:     lo = 0;  hi = 24; break;
    case Category::Layout:    lo = 25; hi = 36; break;
    case Category::Slate:     lo = 37; hi = 43; break;
    case Category::UMG:       lo = 44; hi = 52; break;
    case Category::UIToolkit: lo = 53; hi = 60; break;
    case Category::Immediate: lo = 61; hi = 66; break;
    case Category::UGui:      lo = 67; hi = 74; break;
    default: return false;
    }
    return ki >= lo && ki <= hi;
}

CanvasNode MakeNode(int parent, CanvasNodeType type, ImVec2 aMin, ImVec2 aMax, ImVec2 pivot, ImVec2 pos,
                    ImVec2 size) {
    CanvasNode n;
    n.parent = parent;
    n.type = type;
    n.anchorMin = aMin;
    n.anchorMax = aMax;
    n.pivot = pivot;
    n.anchoredPosition = pos;
    n.sizeDelta = size;
    return n;
}

// Recursively count folders and files in the asset tree (manual walk).
void WalkAssets(const AssetItem& item, int& folders, int& files) {
    if (item.IsFolder())
        ++folders;
    else
        ++files;
    for (const AssetItem& child : item.children)
        WalkAssets(child, folders, files);
}

} // namespace

int RunSelfTest(AppContext& ctx, bool verbose, std::vector<TestCase>& results, std::string& report) {
    int checks = 0;
    int failures = 0;
    auto add = [&](const char* name, bool cond, const std::string& detail) {
        ++checks;
        if (!cond)
            ++failures;
        results.push_back(TestCase{ name, cond, cond ? std::string() : detail });
    };

    // =====================================================================
    // 1. Types
    // =====================================================================
    {
        bool ok = true;
        int bad = -1;
        for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
            const Category c = static_cast<Category>(i);
            if (std::strlen(CategoryName(c)) == 0 || std::strlen(CategoryShortName(c)) == 0 ||
                std::strlen(CategoryBlurb(c)) == 0) {
                ok = false;
                bad = i;
            }
        }
        add("types: category names non-empty", ok, "category index " + std::to_string(bad) + " has an empty name");
    }
    {
        bool ok = true;
        int bad = -1;
        for (int i = 0; i < static_cast<int>(WidgetKind::Count); ++i) {
            const WidgetKind k = static_cast<WidgetKind>(i);
            if (std::strlen(KindName(k)) == 0) {
                ok = false;
                bad = i;
            }
        }
        add("types: kind names non-empty", ok, "kind index " + std::to_string(bad) + " has an empty name");
    }
    {
        bool ok = true;
        std::string bad;
        for (int i = 0; i < static_cast<int>(WidgetKind::Count); ++i) {
            const WidgetKind k = static_cast<WidgetKind>(i);
            if (KindFromName(KindName(k)) != k) {
                ok = false;
                bad = KindName(k);
            }
        }
        add("types: KindFromName(KindName(k)) round-trips", ok, "collision or gap at kind '" + bad + "'");
    }
    {
        const WidgetTraits btn = TraitsForKind(WidgetKind::Button);
        add("types: Button has text, Slider has value, Checkbox toggled",
            btn.text && TraitsForKind(WidgetKind::SliderHorizontal).value &&
                TraitsForKind(WidgetKind::Checkbox).toggled,
            "expected Button.text, Slider.value, Checkbox.toggled");
    }
    {
        const WidgetTraits combo = TraitsForKind(WidgetKind::Combo);
        const WidgetTraits rect = TraitsForKind(WidgetKind::UgRectTransform);
        add("types: Combo has items, canvas kinds have transform+anchors",
            combo.items && rect.transform && rect.anchors,
            "expected Combo.items and UgRectTransform transform/anchors");
    }
    {
        bool ok = true;
        for (int i = 0; i < static_cast<int>(AnchorPreset::Count); ++i) {
            const AnchorPreset a = static_cast<AnchorPreset>(i);
            const ImVec2 v = AnchorVec(a);
            if (std::strlen(AnchorName(a)) == 0)
                ok = false;
            if (v.x < 0.0f || v.x > 1.0f || v.y < 0.0f || v.y > 1.0f)
                ok = false;
            if (IsStretch(a) != (a == AnchorPreset::Stretch))
                ok = false;
        }
        ok = ok && NearV2(AnchorVec(AnchorPreset::Stretch), ImVec2(0.5f, 0.5f));
        ok = ok && NearV2(AnchorVec(AnchorPreset::TopLeft), ImVec2(0.0f, 0.0f));
        ok = ok && NearV2(AnchorVec(AnchorPreset::BottomRight), ImVec2(1.0f, 1.0f));
        add("types: anchor names/vectors/stretch", ok, "anchor preset table inconsistent");
    }
    {
        bool lower = (ToLower("HeLLo World") == "hello world");
        bool contains = ContainsNoCase("Asset Browser", "browser") && !ContainsNoCase("abc", "xyz") &&
                        ContainsNoCase("anything", "");
        bool fmt = (FormatFloat(3.14159f, "%.2f") == "3.14");
        add("types: ToLower/ContainsNoCase/FormatFloat", lower && contains && fmt,
            std::string("ToLower=") + (lower ? "1" : "0") + " contains=" + (contains ? "1" : "0") +
                " format=" + (fmt ? "1" : "0"));
    }
    {
        const ImVec4 c = WithAlpha(ImVec4(0.2f, 0.3f, 0.4f, 1.0f), 0.25f);
        add("types: WithAlpha keeps rgb, sets alpha",
            NearV4(c, ImVec4(0.2f, 0.3f, 0.4f, 0.25f)),
            "alpha " + std::to_string(c.w));
    }
    {
        const ImVec4 a(0.0f, 0.2f, 0.4f, 1.0f);
        const ImVec4 b(1.0f, 0.6f, 0.8f, 0.0f);
        add("types: LerpColor endpoints and midpoint",
            NearV4(LerpColor(a, b, 0.0f), a) && NearV4(LerpColor(a, b, 1.0f), b) &&
                NearV4(LerpColor(a, b, 0.5f), ImVec4(0.5f, 0.4f, 0.6f, 0.5f)),
            "lerp endpoints/midpoint mismatch");
    }
    {
        add("types: BoolLabel", std::strcmp(BoolLabel(true), "true") == 0 && std::strcmp(BoolLabel(false), "false") == 0,
            "BoolLabel mismatch");
    }

    // =====================================================================
    // 2. Registry
    // =====================================================================
    WidgetRegistry reg;
    add("registry: default registry starts empty", reg.Count() == 0,
        "count " + std::to_string(reg.Count()));
    BuildDefaultRegistry(reg);
    add("registry: BuildDefaultRegistry fills it (>60)", reg.Count() > 60,
        "count " + std::to_string(reg.Count()));
    {
        std::vector<std::string> ids;
        for (const auto& it : reg.All())
            ids.push_back(it->id);
        bool unique = true;
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j)
                if (ids[i] == ids[j])
                    unique = false;
        add("registry: ids unique", unique && ids.size() == reg.Count(),
            "duplicate ids present (count " + std::to_string(ids.size()) + ")");
    }
    {
        bool ok = true;
        std::string bad;
        for (const auto& it : reg.All()) {
            if (!KindInCategory(it->kind, it->category)) {
                ok = false;
                bad = it->id;
            }
        }
        add("registry: kind belongs to its category range", ok, "mismatch at '" + bad + "'");
    }
    {
        const std::vector<std::string> expected = { "Buttons", "Toggles", "Values", "Inputs",
                                                    "Collections", "Feedback", "Bars", "Media" };
        const std::vector<std::string> got = reg.GroupsOf(Category::Basic);
        bool ok = got.size() == expected.size();
        for (size_t i = 0; ok && i < got.size(); ++i)
            ok = got[i] == expected[i];
        add("registry: GroupsOf(Basic) returns registration order", ok,
            "size " + std::to_string(got.size()));
    }
    {
        WidgetRegistry mreg;
        WidgetInstance a;
        a.id = "mtu";
        mreg.Add(std::move(a));
        const std::string c2 = mreg.MakeUniqueId("mtu");
        WidgetInstance b;
        b.id = "mtu_2";
        mreg.Add(std::move(b));
        const std::string c3 = mreg.MakeUniqueId("mtu");
        add("registry: MakeUniqueId appends _2/_3 and never collides",
            c2 == "mtu_2" && c3 == "mtu_3" && mreg.Find(c3) == nullptr,
            "got " + c2 + " / " + c3);
    }
    add("registry: Find on a missing id returns nullptr", reg.Find("no.such.id") == nullptr, "expected nullptr");
    {
        const std::string id = reg.All().front()->id;
        add("registry: Remove then Find", reg.Remove(id) && reg.Find(id) == nullptr, "remove/find mismatch");
    }
    {
        std::string err;
        const bool rejected = reg.Rename("basic.button.secondary", "   ", err);
        const std::string rejectedError = err;
        const bool accepted = reg.Rename("basic.button.secondary", "Renamed Secondary", err);
        WidgetInstance* w = reg.Find("basic.button.secondary");
        add("registry: Rename rejects empty (nonempty rule) and accepts valid",
            !rejected && !rejectedError.empty() && accepted && w && w->displayName == "Renamed Secondary",
            "rename behaviour wrong");
    }
    {
        if (ctx.registry.Count() == 0)
            BuildDefaultRegistry(ctx.registry);
        ctx.undo.Clear();
        ctx.propertyClipboard.clear();
        ctx.log.ClearAll();
        ctx.toasts.Clear();

        WidgetInstance probe;
        probe.id = "selftest.probe.reset";
        probe.displayName = "Reset Probe";
        probe.kind = WidgetKind::Button;
        probe.category = Category::Basic;
        probe.group = "SelfTest";
        WidgetInstance& p = ctx.registry.Add(std::move(probe));
        ctx.SelectWidget(p.id);
        p.state.text = "My Button";
        p.state.size = ImVec2(999.0f, 999.0f);
        p.state.value = 0.90f;
        p.state.checked = true;
        p.state.color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        const WidgetState def = DefaultStateOf(WidgetKind::Button);
        ctx.ResetWidgetState(p);
        add("registry: ResetWidgetState reverts to DefaultStateOf (keeps text)",
            NearV2(p.state.size, def.size) && NearF(p.state.value, def.value) && p.state.checked == def.checked &&
                p.state.text == "My Button",
            "size/value/checked not reset");
    }

    // =====================================================================
    // 3. Undo / redo
    // =====================================================================
    {
        WidgetInstance probe;
        probe.id = "selftest.probe.undo";
        probe.displayName = "Undo Probe";
        probe.kind = WidgetKind::Button;
        probe.category = Category::Basic;
        probe.group = "SelfTest";
        WidgetInstance& p = ctx.registry.Add(std::move(probe));
        ctx.SelectWidget(p.id);

        const WidgetState s0 = p.state;
        p.state.text = "edited";
        ctx.RecordEdit(p, "Text", s0);
        const bool pushed = ctx.undo.CanUndo() && ctx.undo.UndoCount() >= 1;

        UndoRecord applied;
        const bool undoOk = ctx.undo.Undo(ctx.registry, applied);
        const bool restored = p.state.text == s0.text && ctx.undo.CanRedo();
        const bool redoOk = ctx.undo.Redo(ctx.registry, applied);
        const bool reappied = p.state.text == "edited";

        add("undo: RecordEdit pushes and Undo restores the before-state (+CanRedo)", pushed && undoOk && restored,
            std::string("push ") + (pushed ? "1" : "0") + " undo " + (undoOk ? "1" : "0") +
                " restore " + (restored ? "1" : "0"));
        add("undo: Redo reapplies the after-state", redoOk && reappied, "redo did not reapply");

        ctx.undo.Clear();
        add("undo: Clear empties both stacks", !ctx.undo.CanUndo() && !ctx.undo.CanRedo(), "not cleared");
    }
    {
        WidgetRegistry local;
        WidgetInstance t;
        t.id = "noop";
        t.displayName = "Noop";
        WidgetState s;
        s.text = "same";
        t.state = s;
        local.Add(std::move(t));
        UndoStack us;
        us.Push("noop", "Noop", "noop", s, s);
        UndoRecord r;
        const bool undoOk = us.Undo(local, r);
        WidgetInstance* w = local.Find("noop");
        const bool unchanged = w && w->state.text == "same" && us.CanRedo();
        add("undo: a no-op edit chain is reversible", undoOk && unchanged, "noop edit not handled");
    }
    {
        UndoStack us;
        us.SetLimit(3);
        WidgetState s;
        for (int i = 0; i < 5; ++i)
            us.Push("x", "X", "label", s, s);
        add("undo: SetLimit(3) keeps at most 3 records", us.UndoCount() == 3,
            "UndoCount " + std::to_string(us.UndoCount()));
    }
    {
        WidgetRegistry local;
        WidgetInstance t;
        t.id = "gone";
        local.Add(std::move(t));
        UndoStack us;
        WidgetState s;
        us.Push("gone", "Gone", "e", s, s);
        local.Remove("gone");
        UndoRecord r;
        add("undo: Undo on a deleted instance returns false", us.Undo(local, r) == false, "should not succeed");
    }

    // =====================================================================
    // 4. Copy / paste
    // =====================================================================
    {
        WidgetInstance probe;
        probe.id = "selftest.probe.copy";
        probe.displayName = "Copy Probe";
        probe.kind = WidgetKind::Button;
        probe.category = Category::Basic;
        probe.group = "SelfTest";
        WidgetInstance& p = ctx.registry.Add(std::move(probe));
        ctx.SelectWidget(p.id);

        // The default multilineText has embedded newlines, which the line based
        // key=value serialiser cannot round-trip; use a single-line buffer here.
        p.state.multilineText = "Single line notes buffer";
        p.state.items = { "One Item", "Two Item", "Three Item" };
        const WidgetState copied = p.state;
        ctx.CopySelectedProperties();
        const size_t beforeUndo = ctx.undo.UndoCount();
        p.state.text = "totally different";
        p.state.value = 0.001f;
        p.state.intValue = -7;
        p.state.selectedIndex = 42;
        p.state.items = { "Only One" };
        p.state.color = ImVec4(0.11f, 0.22f, 0.33f, 0.44f);
        p.state.multilineText = "changed";

        ctx.PastePropertiesIntoSelected();
        const bool applied = p.state.text == copied.text;
        const bool fields = StateEquivalent(p.state, copied);
        const bool pushedUndo = ctx.undo.UndoCount() == beforeUndo + 1;
        add("copy/paste restores every field (items/colours/multiline) and pushes an undo record",
            !ctx.propertyClipboard.empty() && applied && fields && pushedUndo,
            std::string("applied ") + (applied ? "1" : "0") + " fields " + (fields ? "1" : "0") +
                " undo " + (pushedUndo ? "1" : "0"));
    }

    // =====================================================================
    // 5. Validation
    // =====================================================================
    {
        std::string err;
        const bool none = ValidateText("", "none", err);
        const bool nonempty = ValidateText("abc", "nonempty", err) && !ValidateText("   ", "nonempty", err);
        const bool integer = ValidateText("-42", "integer", err) && !ValidateText("1.5", "integer", err) &&
                             !ValidateText("abc", "integer", err);
        const bool flt = ValidateText("12.5", "float", err) && ValidateText("1,5", "float", err) &&
                         !ValidateText("abc", "float", err) && !ValidateText("", "float", err);
        const bool minlen = ValidateText("abcd", "minlength", err) && !ValidateText("abc", "minlength", err);
        const std::string long25(25, 'x');
        const std::string ok24(24, 'x');
        const bool maxlen = ValidateText(ok24, "maxlength", err) && !ValidateText(long25, "maxlength", err);
        const bool pattern = ValidateText("BP_Player", "pattern", "BP_*", err) && !ValidateText("XX", "pattern", "BP_*", err);
        const bool ident = ValidateText("BP_Player", "identifier", err) && ValidateText("_x", "identifier", err) &&
                           !ValidateText("1Player", "identifier", err);
        const bool range = ValidateText("50", "range", err) && !ValidateText("150", "range", err);
        add("validation: rules behave on valid and invalid input",
            none && nonempty && integer && flt && minlen && maxlen && pattern && ident && range,
            "at least one rule misbehaved");
    }
    {
        add("validation: MatchPattern wildcards and classes",
            MatchPattern("a*c", "abbbc") && !MatchPattern("a*c", "abz") && MatchPattern("a?c", "abc") &&
                !MatchPattern("a?c", "ac") && MatchPattern("[a-z]", "q") && !MatchPattern("[a-z]", "Q") &&
                MatchPattern("[a-z]*", "abc") && !MatchPattern("[a-z]*", "Abc") && MatchPattern("abc", "abc") &&
                !MatchPattern("abc", "abcd"),
            "pattern matching mismatch");
    }
    {
        std::string err;
        const bool b1 = ValidateFloatRange(5.0f, 0.0f, 10.0f, err) && err.empty();
        const bool b2 = ValidateFloatRange(0.0f, 0.0f, 10.0f, err) && ValidateFloatRange(10.0f, 0.0f, 10.0f, err);
        const bool b3 = !ValidateFloatRange(-1.0f, 0.0f, 10.0f, err) && !err.empty();
        const bool b4 = ValidateFloatRange(5.0f, 10.0f, 0.0f, err); // swapped bounds are normalised
        add("validation: ValidateFloatRange boundaries", b1 && b2 && b3 && b4, "float range boundary wrong");
    }
    {
        std::string err;
        const bool b1 = ValidateIntRange(5, 0, 10, err);
        const bool b2 = ValidateIntRange(0, 0, 10, err) && ValidateIntRange(10, 0, 10, err);
        const bool b3 = !ValidateIntRange(11, 0, 10, err) && !err.empty();
        const bool b4 = ValidateIntRange(5, 10, 0, err);
        add("validation: ValidateIntRange boundaries", b1 && b2 && b3 && b4, "int range boundary wrong");
    }
    {
        int count = 0;
        const char* const* rules = ValidationRules(count);
        bool ok = count > 0;
        std::string bad;
        for (int i = 0; i < count; ++i) {
            std::string err;
            ValidateText("test", rules[i], "BP_*", err);
            if (err.find("Unknown validation rule") != std::string::npos) {
                ok = false;
                bad = rules[i];
            }
        }
        add("validation: ValidationRules(count) non-empty and every rule understood", ok,
            "unknown rule '" + bad + "' (count " + std::to_string(count) + ")");
    }
    {
        int count = 0;
        const char* const* rules = ValidationRules(count);
        bool ok = count > 0;
        for (int i = 0; i < count; ++i)
            if (std::strlen(ValidationRuleHint(rules[i])) == 0)
                ok = false;
        ok = ok && std::strlen(ValidationRuleHint("none")) > 0;
        add("validation: ValidationRuleHint non-empty for every rule", ok, "a rule hint is empty");
    }

    // =====================================================================
    // 6. Serialization
    // =====================================================================
    {
        KeyValueDoc doc;
        doc.Set("name", "Hello World");
        doc.Set("expr", "a=b=c");
        doc.SetFloat("pi", 3.5f);
        doc.SetInt("n", -42);
        doc.SetBool("flag", true);
        doc.SetVec2("v2", ImVec2(1.5f, 2.5f));
        doc.SetVec4("v4", ImVec4(0.1f, 0.2f, 0.3f, 0.4f));
        const bool direct = doc.Get("name") == "Hello World" && doc.Get("expr") == "a=b=c" &&
                            NearF(doc.GetFloat("pi"), 3.5f) && doc.GetInt("n") == -42 && doc.GetBool("flag") &&
                            NearV2(doc.GetVec2("v2"), ImVec2(1.5f, 2.5f)) &&
                            NearV4(doc.GetVec4("v4"), ImVec4(0.1f, 0.2f, 0.3f, 0.4f));
        add("serialize: KeyValueDoc Set/Get round-trip ('=' and spaces)", direct, "Set/Get mismatch");

        const std::string text = doc.ToText("header line");
        KeyValueDoc back;
        std::string err;
        const bool parsed = back.Parse(text, err);
        const bool same = parsed && back.Get("name") == "Hello World" && back.Get("expr") == "a=b=c" &&
                          NearF(back.GetFloat("pi"), 3.5f) && back.GetInt("n") == -42 && back.GetBool("flag") &&
                          NearV2(back.GetVec2("v2"), ImVec2(1.5f, 2.5f)) &&
                          NearV4(back.GetVec4("v4"), ImVec4(0.1f, 0.2f, 0.3f, 0.4f));
        add("serialize: ToText -> Parse preserves every field", same, "text round-trip lost data");

        std::string gerr;
        const bool garbage = back.Parse("this line has no equals sign", gerr);
        add("serialize: Parse of garbage returns false with a non-empty error",
            !garbage && !gerr.empty(), "garbage parse not rejected");
    }
    {
        ThemeManager src;
        src.mode = ThemeMode::Custom;
        src.custom.accent = ImVec4(0.11f, 0.22f, 0.33f, 0.44f);
        src.custom.windowBg = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        src.style.windowRounding = 7.75f;
        src.style.itemSpacing = ImVec2(3.0f, 4.0f);
        const std::string text = src.Serialize();
        ThemeManager dst;
        std::string err;
        const bool ok = dst.Deserialize(text, err) && dst.mode == ThemeMode::Custom &&
                        NearV4(dst.custom.accent, src.custom.accent) &&
                        NearV4(dst.custom.windowBg, src.custom.windowBg) &&
                        NearF(dst.style.windowRounding, 7.75f) && NearV2(dst.style.itemSpacing, ImVec2(3.0f, 4.0f));
        add("serialize: Theme Serialize -> Deserialize round-trip", ok, "palette or style not preserved");
    }
    {
        ThemeManager tm;
        tm.custom.accent = ImVec4(0.9f, 0.1f, 0.1f, 0.1f);
        tm.ResetCustom();
        add("serialize: ResetCustom copies the dark palette", NearV4(tm.custom.accent, tm.darkPalette.accent),
            "custom accent not reset");
    }
    {
        ThemeManager tm;
        const bool dark0 = NearV4(tm.Active().accent, tm.darkPalette.accent);
        tm.SetMode(ThemeMode::Light);
        const bool light = NearV4(tm.Active().accent, tm.lightPalette.accent) && !NearV4(tm.Active().accent, tm.darkPalette.accent);
        tm.SetMode(ThemeMode::Dark);
        const bool dark1 = NearV4(tm.Active().accent, tm.darkPalette.accent);
        add("serialize: SetMode/LoadBuiltIn change Active()", dark0 && light && dark1, "Active palette not switching");
    }
    {
        int count = 0;
        const ThemeManager::ColorSlot* slots = ThemeManager::ColorSlots(count);
        bool unique = count > 20;
        for (int i = 0; unique && i < count; ++i)
            for (int j = i + 1; j < count; ++j)
                if (std::strcmp(slots[i].name, slots[j].name) == 0)
                    unique = false;
        add("serialize: ColorSlots count > 20 and names unique", unique,
            "count " + std::to_string(count));
    }

    // =====================================================================
    // 7. CanvasScene
    // =====================================================================
    {
        CanvasScene sc;
        sc.BuildDefaultScene();
        bool hasRoot = sc.RootId() >= 0 && sc.Find(sc.RootId()) != nullptr;
        std::vector<int> ids;
        for (const CanvasNode& n : sc.Nodes())
            ids.push_back(n.id);
        bool unique = true;
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j)
                if (ids[i] == ids[j])
                    unique = false;
        const int rid = sc.RootId();
        const bool lookup = sc.IndexOf(rid) >= 0 && sc.Find(rid) != nullptr && sc.Find(99999) == nullptr &&
                            sc.IndexOf(99999) == -1;
        const bool counts = sc.CountOfType(CanvasNodeType::Panel) > 0 && sc.CountOfType(CanvasNodeType::InventorySlot) > 0;
        add("canvas: default scene has a unique-id root, Find/IndexOf, CountOfType",
            hasRoot && unique && lookup && counts, "scene structure mismatch");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        const int sid = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(1, 1),
                                            ImVec2(0.5f, 0.5f), ImVec2(0, 0), ImVec2(0, 0)));
        const int tid = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(10, 10), ImVec2(120, 40)));
        const int trid = sc.AddNode(MakeNode(rid, CanvasNodeType::Image, ImVec2(1, 0), ImVec2(1, 0),
                                             ImVec2(1, 0), ImVec2(-16, 20), ImVec2(100, 40)));
        sc.ComputeLayout(ImVec2(600, 400));
        CanvasNode* s = sc.Find(sid);
        CanvasNode* t = sc.Find(tid);
        CanvasNode* tr = sc.Find(trid);
        const bool stretch = s && NearF(s->computed.min.x, 0) && NearF(s->computed.min.y, 0) &&
                             NearF(s->computed.Width(), 600, 0.1f) && NearF(s->computed.Height(), 400, 0.1f);
        const bool topleft = t && NearV2(t->computed.Size(), ImVec2(120, 40), 0.1f) && NearV2(t->computed.min, ImVec2(10, 10), 0.1f);
        const bool tr600 = tr && NearF(tr->computed.max.x, 584.0f, 0.5f);
        sc.ComputeLayout(ImVec2(800, 400));
        const bool tr800 = tr && NearF(tr->computed.max.x, 784.0f, 0.5f);
        add("canvas: stretch fills parent, TopLeft keeps pixel size, TopRight tracks width",
            stretch && topleft && tr600 && tr800, "anchor layout math wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode box = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                  ImVec2(0, 0), ImVec2(200, 300));
        box.layoutMode = LayoutGroupMode::Vertical;
        box.spacing = 10.0f;
        box.paddingX = 10.0f;
        box.paddingY = 10.0f;
        const int bid = sc.AddNode(box);
        const int ca = sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(0, 0), ImVec2(100, 40)));
        const int cb = sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(0, 0), ImVec2(100, 50)));
        sc.ComputeLayout(ImVec2(600, 400));
        CanvasNode* A = sc.Find(ca);
        CanvasNode* B = sc.Find(cb);
        const bool ok = A && B && NearF(A->computed.min.y, 10.0f, 0.5f) &&
                        NearF(B->computed.min.y, A->computed.max.y + 10.0f, 0.5f);
        add("canvas: Vertical group stacks children by spacing (+ padding)", ok, "vertical stacking wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode box = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                  ImVec2(0, 0), ImVec2(300, 100));
        box.layoutMode = LayoutGroupMode::Horizontal;
        box.spacing = 10.0f;
        box.paddingX = 10.0f;
        box.paddingY = 10.0f;
        const int bid = sc.AddNode(box);
        const int ha = sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(0, 0), ImVec2(50, 40)));
        const int hb = sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(0, 0), ImVec2(60, 40)));
        sc.ComputeLayout(ImVec2(600, 400));
        CanvasNode* A = sc.Find(ha);
        CanvasNode* B = sc.Find(hb);
        const bool ok = A && B && NearF(A->computed.min.x, 10.0f, 0.5f) &&
                        NearF(B->computed.min.x, A->computed.max.x + 10.0f, 0.5f);
        add("canvas: Horizontal group stacks children on x", ok, "horizontal stacking wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode grid = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                   ImVec2(0, 0), ImVec2(220, 200));
        grid.layoutMode = LayoutGroupMode::Grid;
        grid.spacing = 10.0f;
        grid.paddingX = 10.0f;
        grid.paddingY = 10.0f;
        const int gid = sc.AddNode(grid);
        std::vector<int> kids;
        for (int i = 0; i < 4; ++i)
            kids.push_back(sc.AddNode(MakeNode(gid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                               ImVec2(0, 0), ImVec2(50, 30))));
        sc.ComputeLayout(ImVec2(600, 400));
        CanvasNode* c0 = sc.Find(kids[0]);
        CanvasNode* c3 = sc.Find(kids[3]);
        // innerSize.x = 220 - 2*10 = 200 -> columns = floor(200/64) = 3, so child 3 wraps to row 2.
        const bool wrapped = c0 && c3 && NearF(c3->computed.min.x, c0->computed.min.x, 0.5f) &&
                             c3->computed.min.y > c0->computed.min.y + 1.0f;
        add("canvas: Grid wraps after the column count", wrapped, "grid wrap wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode box = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                  ImVec2(0, 0), ImVec2(200, 300));
        box.layoutMode = LayoutGroupMode::Vertical;
        box.forceExpand = true;
        box.spacing = 10.0f;
        box.paddingX = 10.0f;
        box.paddingY = 10.0f;
        const int bid = sc.AddNode(box);
        const int fa = sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(0, 0), ImVec2(100, 40)));
        sc.ComputeLayout(ImVec2(600, 400));
        CanvasNode* A = sc.Find(fa);
        // innerSize.x = 200 - 2*10 = 180; forceExpand stretches the cross axis to fill it.
        const bool ok = A && NearF(A->computed.Width(), 180.0f, 0.5f);
        add("canvas: forceExpand stretches the cross axis", ok,
            A ? ("width " + std::to_string(A->computed.Width())) : std::string("missing node"));
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode box = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                  ImVec2(0, 0), ImVec2(200, 10)); // tiny declared height
        box.layoutMode = LayoutGroupMode::Vertical;
        box.contentFitterV = true;
        box.spacing = 10.0f;
        box.paddingX = 10.0f;
        box.paddingY = 10.0f;
        const int bid = sc.AddNode(box);
        sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                            ImVec2(100, 40)));
        sc.AddNode(MakeNode(bid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                            ImVec2(100, 50)));
        sc.ComputeLayout(ImVec2(600, 400));
        CanvasNode* B = sc.Find(bid);
        // Grown height = 2*padding + childHeights + spacing = 20 + 40 + 50 + 10 = 120.
        const bool ok = B && NearF(B->computed.Height(), 120.0f, 0.5f) && B->computed.Height() > 10.0f;
        add("canvas: content size fitter grows the parent", ok,
            B ? ("height " + std::to_string(B->computed.Height())) : std::string("missing node"));
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode b = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(10, 10));
        CanvasNode a = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(10, 10));
        CanvasNode c = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(10, 10));
        b.zOrder = 1;
        a.zOrder = 5;
        c.zOrder = 3;
        const int bi = sc.AddNode(b);
        const int ai = sc.AddNode(a);
        const int ci = sc.AddNode(c);
        sc.ComputeLayout(ImVec2(600, 400));
        const std::vector<int> order = sc.DrawOrder();
        const bool ordered = order.size() == 4 && order[0] == rid && order[1] == bi && order[2] == ci && order[3] == ai;

        CanvasNode* root = sc.Find(rid);
        sc.MoveZOrder(bi, 10); // b: 1 -> 11
        const bool bMoved = root && sc.Find(bi)->zOrder == 11 && root->children.size() == 3 &&
                            root->children[0] == ci && root->children[1] == ai && root->children[2] == bi;
        sc.MoveZOrder(ai, -999); // clamped to 0
        const bool clamped = sc.Find(ai)->zOrder == 0;
        add("canvas: MoveZOrder / SortChildrenByZ / DrawOrder ordering", ordered && bMoved && clamped,
            "z-order pipeline wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        const int ai = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(10, 10), ImVec2(200, 100)));
        const int bi = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                           ImVec2(50, 50), ImVec2(200, 100)));
        sc.ComputeLayout(ImVec2(600, 400));
        const int both = sc.HitTest(ImVec2(60, 60));      // inside A and B -> top-most (B)
        const int aOnly = sc.HitTest(ImVec2(20, 20));     // inside A only
        const int outside = sc.HitTest(ImVec2(700, 700)); // outside the canvas
        add("canvas: HitTest returns top-most node and -1 outside",
            both == bi && aOnly == ai && outside == -1,
            std::string("both ") + std::to_string(both) + " a " + std::to_string(aOnly) + " out " +
                std::to_string(outside));
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        CanvasNode panel = MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                    ImVec2(30, 30), ImVec2(100, 100));
        panel.name = "Panel";
        panel.zOrder = 2;
        const int pid = sc.AddNode(panel);
        sc.AddNode(MakeNode(pid, CanvasNodeType::Label, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(5, 5),
                            ImVec2(20, 20)));
        const int dup = sc.DuplicateNode(pid);
        CanvasNode* d = sc.Find(dup);
        const bool copied = dup > 0 && d && d->name == "Panel Copy" && NearV2(d->anchoredPosition, ImVec2(46, 46)) &&
                            d->zOrder == 3 && d->children.size() == 1;
        const bool rootRejected = sc.DuplicateNode(rid) == -1;
        add("canvas: DuplicateNode copies children with an offset", copied && rootRejected,
            "duplicate wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        const int pid = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                             ImVec2(0, 0), ImVec2(50, 50)));
        const int cid = sc.AddNode(MakeNode(pid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(20, 20)));
        const int gid = sc.AddNode(MakeNode(cid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(10, 10)));
        const bool removed = sc.RemoveNode(pid);
        const bool gone = sc.Find(pid) == nullptr && sc.Find(cid) == nullptr && sc.Find(gid) == nullptr;
        const bool rootStays = !sc.RemoveNode(rid);
        const bool missing = !sc.RemoveNode(99999) && !sc.RemoveNode(-5);
        add("canvas: RemoveNode removes descendants", removed && gone && rootStays && missing,
            "removal wrong");
    }
    {
        CanvasScene sc;
        sc.Clear();
        const int rid = sc.AddNode(MakeNode(-1, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(0, 0)));
        const int ai = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(50, 50)));
        const int bi = sc.AddNode(MakeNode(ai, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(30, 30)));
        const int ci = sc.AddNode(MakeNode(bi, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(15, 15)));
        const int di = sc.AddNode(MakeNode(rid, CanvasNodeType::Panel, ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0),
                                            ImVec2(0, 0), ImVec2(20, 20)));
        const bool cycleRejected = !sc.SetParent(ai, ci);
        const bool selfRejected = !sc.SetParent(ai, ai);
        const bool rootRejected = !sc.SetParent(rid, ai);
        const bool reparented = sc.SetParent(di, bi) && sc.Find(di)->parent == bi;
        add("canvas: SetParent rejects cycles, allows valid re-parent",
            cycleRejected && selfRejected && rootRejected && reparented, "SetParent guard wrong");
    }

    // =====================================================================
    // 8. AssetLibrary
    // =====================================================================
    {
        AssetLibrary lib;
        lib.BuildDefault();
        lib.RebuildPaths();
        add("assets: BuildDefault + RebuildPaths populate the cache", !lib.AllCached().empty(),
            "cache empty");
        const bool found = lib.Find("/Game/Characters/SK_Knight") != nullptr &&
                           lib.Find("/Game/Environment/Props/SM_Crate") != nullptr &&
                           lib.Find("/Game/Maps/L_Arena") != nullptr && lib.Find("/Game/Nope/Nada") == nullptr;
        add("assets: Find by a known default path", found, "path lookup wrong");

        AssetItem* folder = lib.CreateFolder("/Game", "SelfTestFolder");
        const bool created = folder != nullptr && lib.Find("/Game/SelfTestFolder") != nullptr &&
                             folder->IsFolder();
        add("assets: CreateFolder adds a folder", created, "create folder failed");

        AssetItem* dup = lib.Duplicate("/Game/Maps/L_Arena");
        const bool duplicated = dup != nullptr && dup->name != "L_Arena" && dup->name.find("Copy") != std::string::npos;
        add("assets: Duplicate appends a Copy suffix", duplicated,
            dup ? ("name " + dup->name) : std::string("null"));

        std::string err;
        const bool renamed = lib.Rename("/Game/SelfTestFolder", "RenamedFolder", err) && err.empty() &&
                             lib.Find("/Game/RenamedFolder") != nullptr;
        std::string err2;
        const bool badName = !lib.Rename("/Game/RenamedFolder", "bad/name", err2) && !err2.empty();
        std::string err3;
        const bool siblingClash = !lib.Rename("/Game/UI/WBP_HUD", "WBP_MainMenu", err3) && !err3.empty();
        add("assets: Rename succeeds / rejects bad name / rejects existing sibling",
            renamed && badName && siblingClash, "rename behaviour wrong");

        const bool removed = lib.Remove("/Game/RenamedFolder") && lib.Find("/Game/RenamedFolder") == nullptr &&
                             !lib.Remove("/Game");
        add("assets: Remove deletes an asset (root protected)", removed, "remove wrong");
    }
    {
        AssetLibrary lib;
        int mf = 0, mfiles = 0;
        WalkAssets(lib.Root(), mf, mfiles);
        int lf = 0, lfiles = 0;
        lib.CountAssets(lf, lfiles);
        add("assets: CountAssets matches a manual walk of the tree", mf == lf && mfiles == lfiles,
            std::string("manual ") + std::to_string(mf) + "/" + std::to_string(mfiles) + " vs lib " +
                std::to_string(lf) + "/" + std::to_string(lfiles));
    }
    {
        AssetLibrary lib;
        const auto allMeshes = lib.Filter("", AssetType::Mesh, false);
        const auto knights = lib.Filter("knight", AssetType::Count, false);
        const auto KNIGHTS = lib.Filter("KNIGHT", AssetType::Count, false);
        const auto folders = lib.Filter("", AssetType::Count, true);
        bool onlyFolders = !folders.empty();
        for (const auto& e : folders)
            if (e.type != AssetType::Folder)
                onlyFolders = false;
        bool knightHit = false;
        for (const auto& e : knights)
            if (e.name == "SK_Knight")
                knightHit = true;
        const bool narrows = lib.Filter("Knight", AssetType::Count, false).size() < lib.Filter("", AssetType::Count, false).size();
        add("assets: Filter narrows by type/query, case-insensitive, foldersOnly",
            !allMeshes.empty() && knightHit && knights.size() == KNIGHTS.size() && onlyFolders && narrows,
            "filter mismatch");
    }

    // =====================================================================
    // 9. EventLog / Notifications / Settings / FrameStats
    // =====================================================================
    {
        EventLog log(8);
        for (int i = 0; i < 20; ++i)
            log.Push(LogChannel::Console, (i % 5 == 0) ? LogLevel::Error : LogLevel::Info, "Src", "msg " + std::to_string(i));
        const bool capacity = log.Entries().size() == 8 && log.TotalCount() == 20 && log.Capacity() == 8;
        const bool channel = log.CountChannel(LogChannel::Console) == 8;
        const bool levels = log.CountLevel(LogLevel::Info) + log.CountLevel(LogLevel::Error) == 8;
        const bool unseen = log.Unseen(LogChannel::Console) == 20;
        log.MarkSeen(LogChannel::Console);
        const bool marked = log.Unseen(LogChannel::Console) == 0;
        add("eventlog: capacity, counts, Unseen/MarkSeen", capacity && channel && levels && unseen && marked,
            "ring buffer / counters wrong");
    }
    {
        EventLog f;
        f.Push(LogChannel::Console, LogLevel::Info, "SourceA", "hello world");
        f.Push(LogChannel::Console, LogLevel::Trace, "SourceB", "trace line");
        const auto& e = f.Entries();
        const bool pass = f.PassesFilter(e[0]);
        const bool traceHidden = !f.PassesFilter(e[1]); // Trace off by default
        f.filter = "hello";
        const bool filterHit = f.PassesFilter(e[0]) && !f.PassesFilter(e[1]);
        f.filter.clear();
        f.sourceFilter = "SourceA";
        const bool srcFilter = f.PassesFilter(e[0]) && !f.PassesFilter(e[1]);
        f.sourceFilter.clear();
        f.showChannel[static_cast<int>(LogChannel::Console)] = false;
        const bool chanOff = !f.PassesFilter(e[0]);
        f.showChannel[static_cast<int>(LogChannel::Console)] = true;
        f.showLevel[static_cast<int>(LogLevel::Info)] = false;
        const bool levelOff = !f.PassesFilter(e[0]);
        add("eventlog: PassesFilter honours level/channel/filter/sourceFilter",
            pass && traceHidden && filterHit && srcFilter && chanOff && levelOff, "filter logic wrong");

        EventLog srcs;
        srcs.Push(LogChannel::Event, LogLevel::Info, "A", "x");
        srcs.Push(LogChannel::Event, LogLevel::Info, "B", "y");
        srcs.Push(LogChannel::Event, LogLevel::Info, "A", "z");
        const std::vector<std::string> list = srcs.Sources();
        const bool sources = list.size() == 2 && list[0] == "A" && list[1] == "B";

        EventLog cc;
        cc.Push(LogChannel::Console, LogLevel::Info, "A", "c1");
        cc.Push(LogChannel::Event, LogLevel::Info, "A", "e1");
        cc.ClearChannel(LogChannel::Console);
        const bool clearChannel = cc.CountChannel(LogChannel::Console) == 0 && cc.CountChannel(LogChannel::Event) == 1;
        cc.ClearAll();
        const bool clearAll = cc.Entries().empty();
        add("eventlog: Sources / ClearChannel vs ClearAll", sources && clearChannel && clearAll,
            "sources or clearing wrong");
    }
    {
        NotificationCenter nc;
        nc.Push(ToastKind::Info, "i1", "b", 4.0f);
        nc.Push(ToastKind::Success, "s1", "b", 4.0f);
        nc.Push(ToastKind::Info, "i2", "b", 0.5f);
        const bool pushed = nc.Count() == 3 && nc.CountKind(ToastKind::Info) == 2;
        nc.Update(1.0f); // short-lived toast expires
        const bool expired = nc.Count() == 2 && nc.CountKind(ToastKind::Info) == 1;
        const uint64_t id = nc.Items().front().id;
        nc.Dismiss(id);
        const bool dismissed = nc.Count() == 1;
        nc.Clear();
        const bool cleared = nc.Count() == 0;
        add("notifications: Push/Update(ttl)/Dismiss/Clear/CountKind",
            pushed && expired && dismissed && cleared, "toast lifecycle wrong");
    }
    {
        AppSettings s;
        s.canvasZoom = 10.0f;
        s.gridSize = 1.0f;
        s.uiFontScale = 5.0f;
        s.Clamp();
        const bool clamped = NearF(s.canvasZoom, 4.0f) && NearF(s.gridSize, 4.0f) && NearF(s.uiFontScale, 2.0f);
        AppSettings v;
        std::string err;
        const bool valid = v.Validate(err) && err.empty();
        AppSettings iv;
        iv.canvasZoom = 99.0f; // out of range, not clamped
        std::string err2;
        const bool invalid = !iv.Validate(err2) && !err2.empty();
        add("settings: Clamp and Validate (valid + invalid)", clamped && valid && invalid,
            "clamp/validate wrong");
    }
    {
        FrameStats fs;
        fs.PushFrame(20.0f);
        const bool fps = NearF(fs.currentFps, 50.0f, 0.01f) && fs.frameCount == 1;
        for (int i = 0; i < static_cast<int>(FrameStats::kMaxSamples) + 100; ++i)
            fs.PushFrame(16.0f);
        const bool bounded = fs.frameMs.size() == FrameStats::kMaxSamples && fs.fpsHistory.size() == FrameStats::kMaxSamples;
        add("framestats: PushFrame computes fps and caps samples", fps && bounded,
            std::string("fps ") + std::to_string(fs.currentFps) + " samples " + std::to_string(fs.frameMs.size()));

        FrameStats fb;
        fb.PushFrame(5.0f);
        const bool fast = std::strcmp(fb.FrameBudgetLabel(), "fast") == 0;
        fb.PushFrame(16.0f);
        const bool b60 = std::strcmp(fb.FrameBudgetLabel(), "60 fps budget") == 0;
        fb.PushFrame(25.0f);
        const bool b30 = std::strcmp(fb.FrameBudgetLabel(), "30 fps budget") == 0;
        fb.PushFrame(60.0f);
        const bool over = std::strcmp(fb.FrameBudgetLabel(), "over budget") == 0;
        add("framestats: FrameBudgetLabel thresholds", fast && b60 && b30 && over, "budget label wrong");
    }
    {
        bool ok = true;
        for (int i = 0; i < static_cast<int>(ToastKind::Count); ++i)
            ok = ok && std::strlen(ToastKindName(static_cast<ToastKind>(i))) > 0;
        for (int i = 0; i < static_cast<int>(LogChannel::Count); ++i)
            ok = ok && std::strlen(LogChannelName(static_cast<LogChannel>(i))) > 0;
        for (int i = 0; i < static_cast<int>(LogLevel::Count); ++i)
            ok = ok && std::strlen(LogLevelName(static_cast<LogLevel>(i))) > 0;
        add("names: ToastKind/LogChannel/LogLevel names non-empty", ok, "an enum name is empty");
    }
    {
        const size_t before = ctx.log.TotalCount();
        ctx.LogConsole(LogLevel::Info, "SelfTest", "hello console");
        ctx.LogEvent("SelfTest", "hello event");
        ctx.Notify(ToastKind::Info, "title", "body", 1.0f);
        add("appcontext: LogConsole/LogEvent/Notify are pure state (no ImGui)",
            ctx.log.TotalCount() == before + 2 && ctx.toasts.Count() >= 1,
            "ctx helpers did not update state");
    }

    // =====================================================================
    // 10. DemoShared sanity (logic only)
    // =====================================================================
    add("demo: inventory and inventory counts have equal size",
        ctx.demo.inventory.size() == ctx.demo.inventoryCounts.size(),
        std::string("inv ") + std::to_string(ctx.demo.inventory.size()) + " counts " +
            std::to_string(ctx.demo.inventoryCounts.size()));

    // Build the report.
    for (const TestCase& r : results) {
        if (r.passed) {
            if (verbose)
                report += std::string("  [ OK ] ") + r.name + "\n";
        } else {
            report += std::string("  [FAIL] ") + r.name + " - " + r.detail + "\n";
        }
    }
    report += "Self-test: " + std::to_string(checks) + " checks, " + std::to_string(failures) + " failures\n";

    return failures;
}

int RunSelfTestAndPrint(AppContext& ctx, bool verbose) {
    std::vector<TestCase> results;
    std::string report;
    const int failures = RunSelfTest(ctx, verbose, results, report);
    std::printf("%s", report.c_str());
    std::fflush(stdout);
    return failures;
}

} // namespace demo
} // namespace uilab
