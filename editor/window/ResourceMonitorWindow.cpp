// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ResourceMonitorWindow.h"

#include "external/IconsFontAwesome6.h"
#include "Theme.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cmath>

#include "imgui.h"

using namespace doriax::editor;

namespace {
    // Fixed scratch so the value/label strings cost no allocation per drawn row.
    char* fmtBytes(char* buffer, size_t size, uint64_t bytes) {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mb >= 1024.0) {
            std::snprintf(buffer, size, "%.2f GB", mb / 1024.0);
        } else if (mb >= 1.0) {
            std::snprintf(buffer, size, "%.1f MB", mb);
        } else {
            std::snprintf(buffer, size, "%.0f KB", static_cast<double>(bytes) / 1024.0);
        }
        return buffer;
    }

    char* fmtDuration(char* buffer, size_t size, double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        const long total = static_cast<long>(seconds);
        const long h = total / 3600;
        const long m = (total % 3600) / 60;
        const long s = total % 60;
        if (h > 0) {
            std::snprintf(buffer, size, "%ldh %02ldm", h, m);
        } else if (m > 0) {
            std::snprintf(buffer, size, "%ldm %02lds", m, s);
        } else {
            std::snprintf(buffer, size, "%lds", s);
        }
        return buffer;
    }

    double clamp01(double v) {
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }
}

ResourceMonitorWindow::ResourceMonitorWindow() {
    cpuHistory.assign(HISTORY_SAMPLES, 0.0f);
    ramHistory.assign(HISTORY_SAMPLES, 0.0f);
    historyHead = 0;
    editorCpuPercent = 0.0;
    editorRssBytes = 0;
    windowOpen = false;
    focusRequested = false;
    paused = false;
    lastSampleTime = -1.0;
}

void ResourceMonitorWindow::setOpen(bool open) {
    if (open) {
        if (!windowOpen) {
            focusRequested = true;
        }
        windowOpen = true;
        return;
    }

    windowOpen = false;
    focusRequested = false;
}

bool ResourceMonitorWindow::isOpen() const {
    return windowOpen;
}

void ResourceMonitorWindow::setPaused(bool value) {
    paused = value;
}

bool ResourceMonitorWindow::isPaused() const {
    return paused;
}

void ResourceMonitorWindow::resetHistory() {
    std::fill(cpuHistory.begin(), cpuHistory.end(), 0.0f);
    std::fill(ramHistory.begin(), ramHistory.end(), 0.0f);
    historyHead = 0;
}

size_t ResourceMonitorWindow::findChild(unsigned long long pid) const {
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].pid == pid) {
            return i;
        }
    }
    return children.size();
}

void ResourceMonitorWindow::addChildProcess(unsigned long long pid, const std::string& label) {
    const size_t index = findChild(pid);
    if (index < children.size()) {
        children[index].label = label;
        return;
    }

    ChildProcess child;
    child.pid = pid;
    child.label = label;
    child.registeredTime = ImGui::GetTime();
    children.push_back(std::move(child));
}

void ResourceMonitorWindow::removeChildProcess(unsigned long long pid) {
    const size_t index = findChild(pid);
    if (index < children.size()) {
        children.erase(children.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void ResourceMonitorWindow::clearChildProcesses() {
    children.clear();
}

void ResourceMonitorWindow::setCounter(const std::string& group, const std::string& label, double value, double max) {
    auto existing = counters.find(group);
    if (existing == counters.end()) {
        counterGroups.push_back(group);
        existing = counters.emplace(group, std::vector<CounterEntry>{}).first;
    }

    std::vector<CounterEntry>& entries = existing->second;
    for (CounterEntry& entry : entries) {
        if (entry.label == label) {
            entry.value = value;
            entry.max = max;
            return;
        }
    }

    CounterEntry entry;
    entry.label = label;
    entry.value = value;
    entry.max = max;
    entries.push_back(std::move(entry));
}

void ResourceMonitorWindow::sampleOnce() {
    ProcessStats::selfUsage(editorCpuPercent, editorRssBytes);
    pushHistory();

    if (children.empty()) {
        return;
    }

    std::vector<unsigned long long> pids;
    pids.reserve(children.size());
    for (const ChildProcess& child : children) {
        pids.push_back(child.pid);
    }

    // snapshot() returns one entry per pid in the requested order.
    const std::vector<ProcessSample> samples = ProcessStats::snapshot(pids);
    for (size_t i = 0; i < children.size() && i < samples.size(); ++i) {
        children[i].sample = samples[i];
    }
}

void ResourceMonitorWindow::pushHistory() {
    cpuHistory[historyHead] = static_cast<float>(editorCpuPercent);
    // Ring slots stay in MB so the graph reads at a glance and the float range is sane.
    ramHistory[historyHead] = static_cast<float>(static_cast<double>(editorRssBytes) / (1024.0 * 1024.0));
    historyHead = (historyHead + 1) % HISTORY_SAMPLES;
}

void ResourceMonitorWindow::drawEditorSection() {
    ImGui::TextUnformatted(ICON_FA_MICROCHIP " Editor process");

    char cpuText[64];
    char ramText[64];
    fmtBytes(ramText, sizeof(ramText), editorRssBytes);
    std::snprintf(cpuText, sizeof(cpuText), "%.1f%%", editorCpuPercent);

    // Amber once the editor is chewing through more than half a full machine.
    const bool hot = editorCpuPercent >= 50.0;
    if (hot) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
    }
    ImGui::Text("CPU  %s", cpuText);
    if (hot) {
        ImGui::PopStyleColor();
    }
    ImGui::Text("RAM  %s", ramText);

    const ImVec2 graphSize(0.0f, Theme::dpi(80.0f));
    // Min 0 keeps a true baseline; FLT_MAX lets ImGui autoscale the max to the data.
    ImGui::PlotLines("##cpu_history", cpuHistory.data(), HISTORY_SAMPLES, historyHead,
                     "CPU %", 0.0f, FLT_MAX, graphSize);
    ImGui::PlotLines("##ram_history", ramHistory.data(), HISTORY_SAMPLES, historyHead,
                     "RAM MB", 0.0f, FLT_MAX, graphSize);
}

void ResourceMonitorWindow::drawChildSection() {
    ImGui::Spacing();
    ImGui::TextUnformatted(ICON_FA_MEMORY " Child processes");

    if (children.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextDisabled("None running");
        ImGui::PopStyleColor();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##resource_children", 6, flags)) {
        return;
    }

    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(70.0f));
    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(64.0f));
    ImGui::TableSetupColumn("RAM", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(80.0f));
    ImGui::TableSetupColumn("Private", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(80.0f));
    ImGui::TableSetupColumn("Uptime", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(70.0f));
    ImGui::TableHeadersRow();

    const double now = ImGui::GetTime();
    char nameText[256];
    char cpuText[32];
    char ramText[32];
    char privText[32];
    char upText[32];
    for (const ChildProcess& child : children) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        const std::string& probed = child.sample.name;
        if (!child.label.empty() && !probed.empty()) {
            std::snprintf(nameText, sizeof(nameText), "%s (%s)", child.label.c_str(), probed.c_str());
        } else if (!child.label.empty()) {
            std::snprintf(nameText, sizeof(nameText), "%s", child.label.c_str());
        } else if (!probed.empty()) {
            std::snprintf(nameText, sizeof(nameText), "%s", probed.c_str());
        } else {
            std::snprintf(nameText, sizeof(nameText), "pid %llu", child.pid);
        }
        ImGui::TextUnformatted(nameText);

        ImGui::TableNextColumn();
        ImGui::Text("%llu", child.pid);

        ImGui::TableNextColumn();
        std::snprintf(cpuText, sizeof(cpuText), "%.1f%%", child.sample.cpuPercent);
        ImGui::TextUnformatted(cpuText);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(fmtBytes(ramText, sizeof(ramText), child.sample.workingSetBytes));

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(fmtBytes(privText, sizeof(privText), child.sample.privateBytes));

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(fmtDuration(upText, sizeof(upText), now - child.registeredTime));
    }

    ImGui::EndTable();
}

void ResourceMonitorWindow::drawCounterSection() {
    ImGui::Spacing();
    ImGui::TextUnformatted(ICON_FA_GAUGE_HIGH " Assets / GPU");

    if (counterGroups.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextDisabled("No counters reported yet");
        ImGui::PopStyleColor();
        return;
    }

    char overlay[160];
    for (const std::string& group : counterGroups) {
        auto it = counters.find(group);
        if (it == counters.end() || it->second.empty()) {
            continue;
        }

        // Headers persist their open state across frames, so the panel remembers what
        // the user collapsed without any extra bookkeeping from App.
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::CollapsingHeader(group.c_str())) {
            for (const CounterEntry& entry : it->second) {
                if (entry.max > 0.0) {
                    const float fraction = static_cast<float>(clamp01(entry.value / entry.max));
                    std::snprintf(overlay, sizeof(overlay), "%s: %.0f / %.0f",
                                  entry.label.c_str(), entry.value, entry.max);
                    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
                } else {
                    ImGui::Text("%s: %.0f", entry.label.c_str(), entry.value);
                }
            }
        }
    }
}

void ResourceMonitorWindow::drawSamplerFooter() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char* toggleLabel = paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause";
    if (ImGui::Button(toggleLabel)) {
        paused = !paused;
        if (!paused) {
            // Drop the stale gap so the resumed graph does not hold one long flat segment.
            lastSampleTime = -1.0;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s sampling", paused ? "Resume" : "Pause");
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT " Reset")) {
        resetHistory();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear the history graphs");
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Text("(every %.2fs, %d points)", REFRESH_INTERVAL, HISTORY_SAMPLES);
    ImGui::PopStyleColor();
}

void ResourceMonitorWindow::show() {
    if (!windowOpen) {
        return;
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    const bool wasOpen = windowOpen;

    if (!ImGui::Begin(ResourceMonitorWindow::WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }

    // Time-gate on the accumulated UI clock: draw stays cheap, sampling happens ~4x/second
    // even when the editor renders at full frame rate.
    if (!paused) {
        const double now = ImGui::GetTime();
        if (lastSampleTime < 0.0 || (now - lastSampleTime) >= REFRESH_INTERVAL) {
            sampleOnce();
            lastSampleTime = now;
        }
    }

    drawEditorSection();
    drawChildSection();
    drawCounterSection();
    drawSamplerFooter();

    ImGui::End();

    if (wasOpen && !windowOpen) {
        setOpen(false);
    }
}
