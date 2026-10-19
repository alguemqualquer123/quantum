#include "core/AppContext.h"

#include "core/Serialize.h"
#include "core/Validation.h"

#include <algorithm>
#include <imgui.h>

namespace uilab {

// ---------------------------------------------------------------------------
// FrameStats
// ---------------------------------------------------------------------------
void FrameStats::Reset() {
    frameMs.clear();
    fpsHistory.clear();
    vertices.clear();
    drawCalls.clear();
    currentFrameMs = 16.6f;
    averageFrameMs = 16.6f;
    minFrameMs = 0.0f;
    maxFrameMs = 0.0f;
    currentFps = 60.0f;
    averageFps = 60.0f;
    uptime = 0.0;
    frameCount = 0;
}

void FrameStats::PushFrame(float ms) {
    currentFrameMs = ms;
    currentFps = ms > 0.0001f ? 1000.0f / ms : 0.0f;
    ++frameCount;

    frameMs.push_back(ms);
    fpsHistory.push_back(currentFps);
    while (frameMs.size() > kMaxSamples)
        frameMs.erase(frameMs.begin());
    while (fpsHistory.size() > kMaxSamples)
        fpsHistory.erase(fpsHistory.begin());

    float sum = 0.0f;
    float mn = ms;
    float mx = ms;
    for (float v : frameMs) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    averageFrameMs = frameMs.empty() ? ms : sum / static_cast<float>(frameMs.size());
    averageFps = averageFrameMs > 0.0001f ? 1000.0f / averageFrameMs : 0.0f;
    minFrameMs = frameMs.size() > 1 ? mn : 0.0f;
    maxFrameMs = frameMs.size() > 1 ? mx : 0.0f;
}

void FrameStats::CaptureImGuiStats() {
    ImGuiIO& io = ImGui::GetIO();
    const ImDrawData* dd = ImGui::GetDrawData();
    uiDrawCalls = dd ? dd->CmdListsCount : 0;
    uiVertices = dd ? dd->TotalVtxCount : 0;
    uiWindows = io.MetricsRenderWindows;
    uiTables = io.MetricsActiveWindows;
    if (dd) {
        drawCalls.push_back(static_cast<float>(dd->CmdListsCount));
        vertices.push_back(static_cast<float>(dd->TotalVtxCount));
        while (drawCalls.size() > kMaxSamples)
            drawCalls.erase(drawCalls.begin());
        while (vertices.size() > kMaxSamples)
            vertices.erase(vertices.begin());
    }
    logEntries = 0;
    toasts = 0;
}

const char* FrameStats::FrameBudgetLabel() const {
    if (currentFrameMs <= 8.0f)  return "fast";
    if (currentFrameMs <= 16.7f) return "60 fps budget";
    if (currentFrameMs <= 33.3f) return "30 fps budget";
    return "over budget";
}

// ---------------------------------------------------------------------------
// WidgetState (de)serialisation used by copy/paste of properties
// ---------------------------------------------------------------------------
namespace {

void SerializeState(const WidgetState& s, KeyValueDoc& d) {
    d.Set("text", s.text);
    d.Set("tooltip", s.tooltip);
    d.Set("styleClass", s.styleClass);
    d.Set("styleId", s.styleId);
    d.SetBool("visible", s.visible);
    d.SetBool("enabled", s.enabled);
    d.SetBool("readOnly", s.readOnly);
    d.SetBool("checked", s.checked);
    d.SetBool("toggled", s.toggled);
    d.SetFloat("value", s.value);
    d.SetFloat("minValue", s.minValue);
    d.SetFloat("maxValue", s.maxValue);
    d.SetFloat("step", s.step);
    d.SetInt("intValue", s.intValue);
    d.SetInt("intMin", s.intMin);
    d.SetInt("intMax", s.intMax);
    d.Set("format", s.format);
    d.SetInt("selectedIndex", s.selectedIndex);
    std::string items;
    for (size_t i = 0; i < s.items.size(); ++i) {
        if (i)
            items += "|";
        items += s.items[i];
    }
    d.Set("items", items);
    d.SetVec4("color", s.color);
    d.SetVec4("color2", s.color2);
    d.SetBool("useCustomColor", s.useCustomColor);
    d.SetVec2("position", s.position);
    d.SetVec2("size", s.size);
    d.SetVec2("scale", s.scale);
    d.SetFloat("rotation", s.rotation);
    d.SetFloat("opacity", s.opacity);
    d.SetInt("zOrder", s.zOrder);
    d.SetInt("anchorMin", s.anchorMin);
    d.SetInt("anchorMax", s.anchorMax);
    d.SetInt("pivot", s.pivot);
    d.SetInt("alignment", s.alignment);
    d.SetFloat("spacingX", s.spacingX);
    d.SetFloat("spacingY", s.spacingY);
    d.SetFloat("paddingX", s.paddingX);
    d.SetFloat("paddingY", s.paddingY);
    d.SetFloat("marginX", s.marginX);
    d.SetFloat("marginY", s.marginY);
    d.SetInt("flexDirection", s.flexDirection);
    d.SetFloat("flexGrow", s.flexGrow);
    d.SetInt("alignItems", s.alignItems);
    d.Set("validationRule", s.validationRule);
    d.Set("pattern", s.pattern);
    d.Set("hint", s.hint);
    d.Set("multilineText", s.multilineText);
}

void DeserializeState(const KeyValueDoc& d, WidgetState& s) {
    s.text = d.Get("text", s.text);
    s.tooltip = d.Get("tooltip", s.tooltip);
    s.styleClass = d.Get("styleClass", s.styleClass);
    s.styleId = d.Get("styleId", s.styleId);
    s.visible = d.GetBool("visible", s.visible);
    s.enabled = d.GetBool("enabled", s.enabled);
    s.readOnly = d.GetBool("readOnly", s.readOnly);
    s.checked = d.GetBool("checked", s.checked);
    s.toggled = d.GetBool("toggled", s.toggled);
    s.value = d.GetFloat("value", s.value);
    s.minValue = d.GetFloat("minValue", s.minValue);
    s.maxValue = d.GetFloat("maxValue", s.maxValue);
    s.step = d.GetFloat("step", s.step);
    s.intValue = d.GetInt("intValue", s.intValue);
    s.intMin = d.GetInt("intMin", s.intMin);
    s.intMax = d.GetInt("intMax", s.intMax);
    s.format = d.Get("format", s.format);
    s.selectedIndex = d.GetInt("selectedIndex", s.selectedIndex);
    const std::string items = d.Get("items");
    if (d.Has("items")) {
        s.items.clear();
        size_t start = 0;
        while (true) {
            const size_t bar = items.find('|', start);
            if (bar == std::string::npos) {
                s.items.push_back(items.substr(start));
                break;
            }
            s.items.push_back(items.substr(start, bar - start));
            start = bar + 1;
        }
    }
    s.color = d.GetVec4("color", s.color);
    s.color2 = d.GetVec4("color2", s.color2);
    s.useCustomColor = d.GetBool("useCustomColor", s.useCustomColor);
    s.position = d.GetVec2("position", s.position);
    s.size = d.GetVec2("size", s.size);
    s.scale = d.GetVec2("scale", s.scale);
    s.rotation = d.GetFloat("rotation", s.rotation);
    s.opacity = d.GetFloat("opacity", s.opacity);
    s.zOrder = d.GetInt("zOrder", s.zOrder);
    s.anchorMin = d.GetInt("anchorMin", s.anchorMin);
    s.anchorMax = d.GetInt("anchorMax", s.anchorMax);
    s.pivot = d.GetInt("pivot", s.pivot);
    s.alignment = d.GetInt("alignment", s.alignment);
    s.spacingX = d.GetFloat("spacingX", s.spacingX);
    s.spacingY = d.GetFloat("spacingY", s.spacingY);
    s.paddingX = d.GetFloat("paddingX", s.paddingX);
    s.paddingY = d.GetFloat("paddingY", s.paddingY);
    s.marginX = d.GetFloat("marginX", s.marginX);
    s.marginY = d.GetFloat("marginY", s.marginY);
    s.flexDirection = d.GetInt("flexDirection", s.flexDirection);
    s.flexGrow = d.GetFloat("flexGrow", s.flexGrow);
    s.alignItems = d.GetInt("alignItems", s.alignItems);
    s.validationRule = d.Get("validationRule", s.validationRule);
    s.pattern = d.Get("pattern", s.pattern);
    s.hint = d.Get("hint", s.hint);
    s.multilineText = d.Get("multilineText", s.multilineText);
}

} // namespace

// ---------------------------------------------------------------------------
// AppContext
// ---------------------------------------------------------------------------
WidgetInstance* AppContext::SelectedWidget() { return registry.Find(selection.widgetId); }

const WidgetInstance* AppContext::SelectedWidget() const {
    return const_cast<AppContext*>(this)->registry.Find(selection.widgetId);
}

void AppContext::SelectWidget(const std::string& id) {
    if (selection.widgetId == id)
        return;
    selection.widgetId = id;
    if (const WidgetInstance* w = SelectedWidget())
        activeCategory = w->category;
}

void AppContext::SelectCategory(Category c) {
    activeCategory = c;
}

void AppContext::LogConsole(LogLevel level, const std::string& source, const std::string& message) {
    log.Push(LogChannel::Console, level, source, message);
}

void AppContext::LogEvent(const std::string& source, const std::string& message) {
    log.Push(LogChannel::Event, LogLevel::Info, source, message);
    ++demo.eventsThisSecond;
}

void AppContext::LogDebug(const std::string& source, const std::string& message) {
    log.Push(LogChannel::Debug, LogLevel::Trace, source, message);
}

void AppContext::Interaction(const std::string& widgetName, const std::string& action) {
    if (!settings.logInteractions)
        return;
    LogEvent(widgetName, action);
}

void AppContext::Notify(ToastKind kind, const std::string& title, const std::string& body, float ttl) {
    toasts.Push(kind, title, body, ttl);
}

void AppContext::RecordEdit(const WidgetInstance& inst, const std::string& label, const WidgetState& before) {
    KeyValueDoc was, now;
    SerializeState(before, was);
    SerializeState(inst.state, now);
    if (was.ToText({}) == now.ToText({}))
        return;
    undo.Push(inst.id, inst.displayName, label, before, inst.state);
    if (settings.logAllWidgetEdits)
        LogDebug("Inspector", inst.displayName + ": " + label + " changed");
}

void AppContext::ResetWidgetState(WidgetInstance& inst) {
    const WidgetState before = inst.state;
    inst.state = DefaultStateOf(inst.kind);
    // Keep naming/content that identify the component in the tree.
    inst.state.text = before.text;
    inst.state.multilineText = before.multilineText;
    undo.Push(inst.id, inst.displayName, "Reset properties", before, inst.state);
    LogConsole(LogLevel::Warning, "Inspector", inst.displayName + ": properties reset to defaults");
}

void AppContext::DuplicateSelectedWidget() {
    WidgetInstance* src = SelectedWidget();
    if (!src) {
        LogConsole(LogLevel::Warning, "Registry", "Select a component before duplicating.");
        return;
    }
    WidgetInstance copy = *src;
    copy.id = registry.MakeUniqueId(src->id + "_copy");
    copy.displayName = src->displayName + " Copy";
    WidgetInstance& created = registry.Add(std::move(copy));
    SelectWidget(created.id);
    LogConsole(LogLevel::Success, "Registry", "Duplicated component: " + created.displayName);
}

void AppContext::CopySelectedProperties() {
    WidgetInstance* inst = SelectedWidget();
    if (!inst)
        return;
    KeyValueDoc doc;
    SerializeState(inst->state, doc);
    propertyClipboard = doc.ToText("properties of " + inst->displayName);
    LogConsole(LogLevel::Info, "Inspector", "Copied properties of " + inst->displayName);
}

void AppContext::PastePropertiesIntoSelected() {
    WidgetInstance* inst = SelectedWidget();
    if (!inst) {
        LogConsole(LogLevel::Warning, "Inspector", "Nothing selected to paste into.");
        return;
    }
    if (propertyClipboard.empty()) {
        LogConsole(LogLevel::Warning, "Inspector", "Property clipboard is empty.");
        return;
    }
    KeyValueDoc doc;
    std::string error;
    if (!doc.Parse(propertyClipboard, error)) {
        LogConsole(LogLevel::Error, "Inspector", "Cannot paste properties: " + error);
        return;
    }
    const WidgetState before = inst->state;
    DeserializeState(doc, inst->state);
    undo.Push(inst->id, inst->displayName, "Paste properties", before, inst->state);
    LogConsole(LogLevel::Success, "Inspector", "Pasted properties into " + inst->displayName);
}

void AppContext::CaptureFrameStats(float dt) {
    demo.wallTime += dt;
    if (demo.playing)
        demo.time += dt * demo.speed;

    stats.PushFrame(dt * 1000.0f);
    stats.uptime += dt;
    stats.logEntries = log.TotalCount();
    stats.toasts = toasts.Count();

    demo.eventsTimer += dt;
    if (demo.eventsTimer >= 1.0f) {
        demo.eventsTimer = 0.0f;
        demo.eventsThisSecond = 0;
    }
    ++frameIndex;
}

std::string AppContext::StatusLine() const {
    char buf[512];
    const WidgetInstance* w = SelectedWidget();
    std::snprintf(buf, sizeof(buf),
                  "%s v%s | %s %s | %d components | selection: %s | theme: %s | %.1f fps",
                  appName.c_str(), appVersion.c_str(), renderer.c_str(), rendererVersion.c_str(),
                  static_cast<int>(registry.Count()), w ? w->displayName.c_str() : "(none)",
                  theme.ActiveName(), static_cast<double>(stats.currentFps));
    return std::string(buf);
}

} // namespace uilab
