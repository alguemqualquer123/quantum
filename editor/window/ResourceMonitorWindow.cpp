// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ResourceMonitorWindow.h"

#include "external/IconsFontAwesome6.h"
#include "Theme.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

using namespace quantum::editor;

namespace {
    // Sampling cadences offered by the toolbar combo, in seconds; the shared
    // SystemStats sampler is re-armed with the picked one.
    const double kRefreshRates[] = {0.25, 0.5, 1.0, 2.0};
    const int kRefreshRateCount = static_cast<int>(sizeof(kRefreshRates) / sizeof(kRefreshRates[0]));

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

    // Uptime reads in days; fmtDuration is tuned for process lifetimes instead.
    char* fmtUptime(char* buffer, size_t size, double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        const long total = static_cast<long>(seconds);
        const long d = total / 86400;
        const long h = (total % 86400) / 3600;
        const long m = (total % 3600) / 60;
        if (d > 0) {
            std::snprintf(buffer, size, "%ldd %02ldh", d, h);
        } else if (h > 0) {
            std::snprintf(buffer, size, "%ldh %02ldm", h, m);
        } else {
            std::snprintf(buffer, size, "%ldm", m);
        }
        return buffer;
    }

    // Network rates share the KB/s scale of the graphs, so one glance lines the
    // number up with the plot underneath.
    char* fmtRate(char* buffer, size_t size, double bytesPerSecond) {
        if (bytesPerSecond < 0.0) bytesPerSecond = 0.0;
        const double kb = bytesPerSecond / 1024.0;
        if (kb >= 1024.0 * 1024.0) {
            std::snprintf(buffer, size, "%.2f GB/s", kb / (1024.0 * 1024.0));
        } else if (kb >= 1024.0) {
            std::snprintf(buffer, size, "%.1f MB/s", kb / 1024.0);
        } else if (kb >= 1.0) {
            std::snprintf(buffer, size, "%.0f KB/s", kb);
        } else {
            std::snprintf(buffer, size, "%.0f B/s", bytesPerSecond);
        }
        return buffer;
    }

    char* fmtTemperature(char* buffer, size_t size, double celsius) {
        if (celsius < 0.0) {
            std::snprintf(buffer, size, "N/A");
        } else {
            std::snprintf(buffer, size, "%.0f\xC2\xB0" "C", celsius);
        }
        return buffer;
    }

    double clamp01(double v) {
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    // Pressure palette shared by every usage bar: healthy green, busy amber,
    // saturated red. The thresholds are where a machine starts to feel the load.
    ImVec4 pressureColor(double fraction) {
        if (fraction >= 0.90) {
            return Theme::Colors::DangerText;
        }
        if (fraction >= 0.70) {
            return Theme::Colors::WarningText;
        }
        return Theme::Colors::SuccessText;
    }

    ImVec4 temperatureColor(double celsius) {
        if (celsius < 0.0) {
            return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        }
        if (celsius >= 85.0) {
            return Theme::Colors::DangerText;
        }
        if (celsius >= 70.0) {
            return Theme::Colors::WarningText;
        }
        return Theme::Colors::SuccessText;
    }

    // Card chrome: rounded surface + padding + icon/title header, matching the
    // Resources window's file cards. Every beginCard() needs an endCard().
    void beginCard(const char* id, const char* icon, const char* title, const char* trailing = nullptr) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::FileCardBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Theme::dpi(6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(10.0f, 8.0f)));
        ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ButtonActivated);
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(title);

        if (trailing && *trailing) {
            const float width = ImGui::CalcTextSize(trailing).x;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
            ImGui::TextUnformatted(trailing);
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
    }

    void endCard() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // One metric line: dim label on the left, value right-aligned in its state
    // color, and - when fraction >= 0 - a slim usage bar underneath.
    void metricRow(const char* label, const char* value, double fraction, const ImVec4& valueColor, const char* tooltip) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        if (tooltip && *tooltip && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }

        const float valueWidth = ImGui::CalcTextSize(value).x;
        const float target = ImGui::GetWindowContentRegionMax().x - valueWidth;
        ImGui::SameLine(target > ImGui::GetCursorPosX() ? target : ImGui::GetCursorPosX());
        ImGui::PushStyleColor(ImGuiCol_Text, valueColor);
        ImGui::TextUnformatted(value);
        ImGui::PopStyleColor();

        if (fraction >= 0.0) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, valueColor);
            ImGui::ProgressBar(static_cast<float>(clamp01(fraction)), ImVec2(-FLT_MIN, Theme::dpi(6.0f)), "");
            ImGui::PopStyleColor();
        }
    }

    // Secondary facts (temperatures, totals, uptime) in the dim text color.
    void detailLine(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    void historyPlot(const char* id, const std::vector<float>& history, int offset,
                     const ImVec4& color, const char* overlay, float scaleMax,
                     float height, float width = -FLT_MIN) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
        ImGui::PlotLines(id, history.data(), static_cast<int>(history.size()), offset, overlay,
                         0.0f, scaleMax, ImVec2(width, Theme::dpi(height)));
        ImGui::PopStyleColor();
    }

    bool labelMentionsServer(const std::string& label) {
        std::string lowered = label;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered.find("server") != std::string::npos;
    }
}

ResourceMonitorWindow::ResourceMonitorWindow() {
    editorCpuHistory.assign(HISTORY_SAMPLES, 0.0f);
    editorRamHistory.assign(HISTORY_SAMPLES, 0.0f);
    systemCpuHistory.assign(HISTORY_SAMPLES, 0.0f);
    systemRamHistory.assign(HISTORY_SAMPLES, 0.0f);
    gpuHistory.assign(HISTORY_SAMPLES, 0.0f);
    vramHistory.assign(HISTORY_SAMPLES, 0.0f);
    downHistory.assign(HISTORY_SAMPLES, 0.0f);
    upHistory.assign(HISTORY_SAMPLES, 0.0f);
    historyHead = 0;
    editorCpuPercent = 0.0;
    editorRssBytes = 0;
    windowOpen = false;
    focusRequested = false;
    paused = false;
    refreshRateIndex = 1;  // 0.5 s, matching SystemStats' default interval
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
    std::fill(editorCpuHistory.begin(), editorCpuHistory.end(), 0.0f);
    std::fill(editorRamHistory.begin(), editorRamHistory.end(), 0.0f);
    std::fill(systemCpuHistory.begin(), systemCpuHistory.end(), 0.0f);
    std::fill(systemRamHistory.begin(), systemRamHistory.end(), 0.0f);
    std::fill(gpuHistory.begin(), gpuHistory.end(), 0.0f);
    std::fill(vramHistory.begin(), vramHistory.end(), 0.0f);
    std::fill(downHistory.begin(), downHistory.end(), 0.0f);
    std::fill(upHistory.begin(), upHistory.end(), 0.0f);
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
    // Ring slots stay in MB (or KB/s) so the graphs read at a glance and the
    // float range is sane; percentages are stored as-is.
    editorCpuHistory[historyHead] = static_cast<float>(editorCpuPercent);
    editorRamHistory[historyHead] = static_cast<float>(static_cast<double>(editorRssBytes) / (1024.0 * 1024.0));
    systemCpuHistory[historyHead] = static_cast<float>(system.cpu.usagePercent);
    systemRamHistory[historyHead] = static_cast<float>(static_cast<double>(system.memory.usedBytes) / (1024.0 * 1024.0));
    gpuHistory[historyHead] = static_cast<float>(system.gpu.usagePercent < 0.0 ? 0.0 : system.gpu.usagePercent);
    vramHistory[historyHead] = static_cast<float>(static_cast<double>(system.gpu.vramUsedBytes) / (1024.0 * 1024.0));
    downHistory[historyHead] = static_cast<float>(system.network.downBytesPerSec / 1024.0);
    upHistory[historyHead] = static_cast<float>(system.network.upBytesPerSec / 1024.0);
    historyHead = (historyHead + 1) % HISTORY_SAMPLES;
}

void ResourceMonitorWindow::drawToolbar() {
    const ImVec4 stateColor = paused ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : Theme::Colors::SuccessText;
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::TextUnformatted(paused ? ICON_FA_PAUSE " Paused" : ICON_FA_CIRCLE " Live");
    ImGui::PopStyleColor();

    const ImGuiIO& io = ImGui::GetIO();
    char frameText[64];
    std::snprintf(frameText, sizeof(frameText), "%.1f FPS  %.2f ms", io.Framerate,
                  1000.0f / (io.Framerate > 0.0f ? io.Framerate : 1.0f));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
    ImGui::Text("|  %s", frameText);
    ImGui::PopStyleColor();

    // Right-aligned sampler controls: cadence, pause/resume, clear graphs.
    const char* pauseLabel = paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause";
    const char* resetLabel = ICON_FA_ARROW_ROTATE_LEFT " Reset";
    const float buttonPad = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float comboWidth = Theme::dpi(96.0f);
    const float controlsWidth = comboWidth
        + ImGui::CalcTextSize(pauseLabel).x + buttonPad
        + ImGui::CalcTextSize(resetLabel).x + buttonPad
        + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float controlsX = ImGui::GetWindowContentRegionMax().x - controlsWidth;
    ImGui::SameLine(controlsX > ImGui::GetCursorPosX() ? controlsX : ImGui::GetCursorPosX());

    static const char* rateItems = "0.25 s\0" "0.5 s\0" "1 s\0" "2 s\0";
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("##sampling_rate", &refreshRateIndex, rateItems)) {
        refreshRateIndex = refreshRateIndex < 0 ? 0 : (refreshRateIndex >= kRefreshRateCount ? kRefreshRateCount - 1 : refreshRateIndex);
        SystemStats::setRefreshInterval(kRefreshRates[refreshRateIndex]);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Sampling interval");
    }

    ImGui::SameLine();
    if (ImGui::Button(pauseLabel)) {
        paused = !paused;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s sampling", paused ? "Resume" : "Pause");
    }

    ImGui::SameLine();
    if (ImGui::Button(resetLabel)) {
        resetHistory();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear the history graphs");
    }
}

void ResourceMonitorWindow::drawSystemCard() {
    const CpuStats& cpu = system.cpu;
    const MemoryStats& memory = system.memory;

    char subtitle[160];
    std::snprintf(subtitle, sizeof(subtitle), "%s", cpu.name.empty() ? "" : cpu.name.c_str());
    beginCard("##card_system", ICON_FA_MICROCHIP, "System", subtitle);

    char value[64];
    const double cpuFraction = cpu.usagePercent / 100.0;
    std::snprintf(value, sizeof(value), "%.1f%%", cpu.usagePercent);
    metricRow("CPU usage", value, cpuFraction, pressureColor(cpuFraction),
              "Processor time across every core since the previous sample.");
    historyPlot("##system_cpu", systemCpuHistory, historyHead,
                ImGui::GetStyleColorVec4(ImGuiCol_PlotLines), "CPU %", 100.0f, 54.0f);

    ImGui::Separator();

    const double ramFraction = memory.totalBytes > 0
        ? static_cast<double>(memory.usedBytes) / static_cast<double>(memory.totalBytes)
        : 0.0;
    char usedText[32];
    char totalText[32];
    char memoryValue[80];
    std::snprintf(memoryValue, sizeof(memoryValue), "%s / %s",
                  fmtBytes(usedText, sizeof(usedText), memory.usedBytes),
                  fmtBytes(totalText, sizeof(totalText), memory.totalBytes));
    metricRow("Memory", memoryValue, ramFraction, pressureColor(ramFraction),
              "Physical RAM in use; the bar is against installed memory.");
    historyPlot("##system_ram", systemRamHistory, historyHead, Theme::Colors::SuccessText,
                "RAM MB", FLT_MAX, 54.0f);

    char tempText[32];
    char uptimeText[32];
    char detail[192];
    std::snprintf(detail, sizeof(detail), "%s Temperature %s  |  %u threads  |  Uptime %s  |  Available %s",
                  ICON_FA_TEMPERATURE_HALF,
                  fmtTemperature(tempText, sizeof(tempText), cpu.temperatureC),
                  cpu.logicalCores,
                  fmtUptime(uptimeText, sizeof(uptimeText), system.uptimeSeconds),
                  fmtBytes(usedText, sizeof(usedText), memory.availableBytes));
    detailLine(detail);

    if (memory.commitLimitBytes > 0) {
        char commitUsed[32];
        char commitLimit[32];
        char commitDetail[128];
        std::snprintf(commitDetail, sizeof(commitDetail), "Commit charge %s / %s (RAM + page file)",
                      fmtBytes(commitUsed, sizeof(commitUsed), memory.commitUsedBytes),
                      fmtBytes(commitLimit, sizeof(commitLimit), memory.commitLimitBytes));
        detailLine(commitDetail);
    }

    endCard();
}

void ResourceMonitorWindow::drawGpuCard() {
    const GpuStats& gpu = system.gpu;
    char subtitle[160];
    std::snprintf(subtitle, sizeof(subtitle), "%s", gpu.name.empty() ? "" : gpu.name.c_str());
    beginCard("##card_gpu", ICON_FA_VIDEO, "GPU", subtitle);

    if (gpu.usagePercent < 0.0 && gpu.vramTotalBytes == 0) {
        detailLine("No GPU counters on this machine. NVIDIA GPUs also report");
        detailLine("temperature through the driver; AMD/Intel show usage only.");
        endCard();
        return;
    }

    char value[64];
    if (gpu.usagePercent >= 0.0) {
        const double fraction = gpu.usagePercent / 100.0;
        std::snprintf(value, sizeof(value), "%.1f%%", gpu.usagePercent);
        metricRow("Usage", value, fraction, pressureColor(fraction),
                  "Busiest GPU engine, all processes on the adapter combined.");
        historyPlot("##gpu_usage", gpuHistory, historyHead, Theme::Colors::ChildSceneText,
                    "GPU %", 100.0f, 48.0f);
    } else {
        detailLine("Usage counter unavailable on this machine");
    }

    if (gpu.vramTotalBytes > 0) {
        const double fraction = static_cast<double>(gpu.vramUsedBytes) / static_cast<double>(gpu.vramTotalBytes);
        char usedText[32];
        char totalText[32];
        char vramValue[80];
        std::snprintf(vramValue, sizeof(vramValue), "%s / %s",
                      fmtBytes(usedText, sizeof(usedText), gpu.vramUsedBytes),
                      fmtBytes(totalText, sizeof(totalText), gpu.vramTotalBytes));
        metricRow("VRAM", vramValue, fraction, pressureColor(fraction),
                  "Video memory in use on the busiest adapter.");
        historyPlot("##gpu_vram", vramHistory, historyHead, Theme::Colors::SubtleText,
                    "VRAM MB", FLT_MAX, 48.0f);
    } else {
        detailLine("VRAM counters unavailable on this machine");
    }

    char tempText[32];
    char detail[96];
    std::snprintf(detail, sizeof(detail), "%s Temperature %s", ICON_FA_TEMPERATURE_HALF,
                  fmtTemperature(tempText, sizeof(tempText), gpu.temperatureC));
    ImGui::PushStyleColor(ImGuiCol_Text, temperatureColor(gpu.temperatureC));
    ImGui::TextUnformatted(detail);
    ImGui::PopStyleColor();

    endCard();
}

void ResourceMonitorWindow::drawNetworkCard() {
    const NetworkStats& network = system.network;

    char downText[32];
    char upText[32];
    char subtitle[96];
    std::snprintf(subtitle, sizeof(subtitle), "%s %s   %s %s",
                  ICON_FA_ARROW_DOWN, fmtRate(downText, sizeof(downText), network.downBytesPerSec),
                  ICON_FA_ARROW_UP, fmtRate(upText, sizeof(upText), network.upBytesPerSec));
    beginCard("##card_network", ICON_FA_NETWORK_WIRED, "Network", subtitle);

    // Down/up get their own plots: a shared scale would flatten whichever side
    // is quieter, and a server usually has very asymmetric traffic.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    historyPlot("##net_down", downHistory, historyHead, Theme::Colors::NetworkDown,
                "Down KB/s", FLT_MAX, 48.0f, half);
    ImGui::SameLine();
    historyPlot("##net_up", upHistory, historyHead, Theme::Colors::NetworkUp,
                "Up KB/s", FLT_MAX, 48.0f, half);

    char totalDown[32];
    char totalUp[32];
    char detail[160];
    std::snprintf(detail, sizeof(detail), "Session  %s %s   %s %s",
                  ICON_FA_ARROW_DOWN, fmtBytes(totalDown, sizeof(totalDown), network.downBytesTotal),
                  ICON_FA_ARROW_UP, fmtBytes(totalUp, sizeof(totalUp), network.upBytesTotal));
    detailLine(detail);

    // Interfaces, busiest first; idle ones would drown the table on a machine
    // with VPN or Hyper-V adapters.
    std::vector<const NetworkInterfaceStats*> active;
    size_t idleCount = 0;
    for (const NetworkInterfaceStats& entry : network.interfaces) {
        if (entry.downBytesPerSec > 0.0 || entry.upBytesPerSec > 0.0) {
            active.push_back(&entry);
        } else {
            ++idleCount;
        }
    }
    std::sort(active.begin(), active.end(), [](const NetworkInterfaceStats* a, const NetworkInterfaceStats* b) {
        return (a->downBytesPerSec + a->upBytesPerSec) > (b->downBytesPerSec + b->upBytesPerSec);
    });

    constexpr size_t kMaxRows = 6;
    if (!active.empty()) {
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##resource_interfaces", 4, flags)) {
            ImGui::TableSetupColumn("Interface");
            ImGui::TableSetupColumn("Down", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(86.0f));
            ImGui::TableSetupColumn("Up", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(86.0f));
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(86.0f));
            ImGui::TableHeadersRow();

            const size_t rows = std::min(active.size(), kMaxRows);
            for (size_t i = 0; i < rows; ++i) {
                const NetworkInterfaceStats& entry = *active[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.name.c_str());

                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::NetworkDown);
                ImGui::TextUnformatted(fmtRate(downText, sizeof(downText), entry.downBytesPerSec));
                ImGui::PopStyleColor();

                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::NetworkUp);
                ImGui::TextUnformatted(fmtRate(upText, sizeof(upText), entry.upBytesPerSec));
                ImGui::PopStyleColor();

                ImGui::TableNextColumn();
                char totalText[32];
                std::snprintf(totalText, sizeof(totalText), "%s",
                              fmtBytes(totalDown, sizeof(totalDown), entry.downBytesTotal + entry.upBytesTotal));
                ImGui::TextUnformatted(totalText);
            }
            ImGui::EndTable();
        }
        if (active.size() > kMaxRows || idleCount > 0) {
            char more[96];
            std::snprintf(more, sizeof(more), "%zu more interface%s with no traffic",
                          active.size() - std::min(active.size(), kMaxRows) + idleCount,
                          (active.size() - std::min(active.size(), kMaxRows) + idleCount) == 1 ? "" : "s");
            detailLine(more);
        }
    } else {
        detailLine("No interface traffic since the window opened");
    }

    // Multiplayer/server shortcut: any tracked child tagged as a server gets its
    // own line, so the port owner is visible next to its throughput.
    for (const ChildProcess& child : children) {
        if (!labelMentionsServer(child.label)) {
            continue;
        }
        char uptimeText[32];
        char line[192];
        std::snprintf(line, sizeof(line), "%s %s  pid %llu  up %s%s",
                      ICON_FA_SERVER,
                      child.label.empty() ? "server" : child.label.c_str(),
                      child.pid,
                      fmtDuration(uptimeText, sizeof(uptimeText), ImGui::GetTime() - child.registeredTime),
                      child.sample.alive ? "" : "  (exited)");
        detailLine(line);
    }

    endCard();
}

void ResourceMonitorWindow::drawEditorSection() {
    beginCard("##card_editor", ICON_FA_DESKTOP, "Editor process");

    char value[64];
    const double cpuFraction = editorCpuPercent / 100.0;
    std::snprintf(value, sizeof(value), "%.1f%%", editorCpuPercent);
    metricRow("CPU", value, cpuFraction, pressureColor(cpuFraction),
              "This editor process alone; 100% is one fully busy core.");
    historyPlot("##editor_cpu", editorCpuHistory, historyHead,
                ImGui::GetStyleColorVec4(ImGuiCol_PlotLines), "CPU %", 100.0f, 48.0f);

    char ramText[32];
    const double ramFraction = system.memory.totalBytes > 0
        ? static_cast<double>(editorRssBytes) / static_cast<double>(system.memory.totalBytes)
        : 0.0;
    metricRow("Memory", fmtBytes(ramText, sizeof(ramText), editorRssBytes), ramFraction,
              pressureColor(ramFraction), "Resident set of the editor process.");
    historyPlot("##editor_ram", editorRamHistory, historyHead, Theme::Colors::SuccessText,
                "RAM MB", FLT_MAX, 48.0f);

    endCard();
}

void ResourceMonitorWindow::drawChildSection() {
    char subtitle[64];
    std::snprintf(subtitle, sizeof(subtitle), "%zu tracked", children.size());
    beginCard("##card_children", ICON_FA_DIAGRAM_PROJECT, "Child processes", subtitle);

    if (children.empty()) {
        detailLine("None running. The resource server and build tools appear here.");
        endCard();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##resource_children", 6, flags)) {
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

            const ImVec4 dotColor = child.sample.alive ? Theme::Colors::SuccessText
                                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::PushStyleColor(ImGuiCol_Text, dotColor);
            ImGui::TextUnformatted(ICON_FA_CIRCLE);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (child.sample.alive) {
                ImGui::TextUnformatted(nameText);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(nameText);
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(child.sample.alive ? "Running" : "Exited or not queryable");
            }

            ImGui::TableNextColumn();
            ImGui::Text("%llu", child.pid);

            ImGui::TableNextColumn();
            std::snprintf(cpuText, sizeof(cpuText), "%.1f%%", child.sample.cpuPercent);
            if (child.sample.cpuPercent >= 50.0) {
                ImGui::PushStyleColor(ImGuiCol_Text, pressureColor(child.sample.cpuPercent / 100.0));
                ImGui::TextUnformatted(cpuText);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextUnformatted(cpuText);
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtBytes(ramText, sizeof(ramText), child.sample.workingSetBytes));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtBytes(privText, sizeof(privText), child.sample.privateBytes));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtDuration(upText, sizeof(upText), now - child.registeredTime));
        }

        ImGui::EndTable();
    }

    endCard();
}

void ResourceMonitorWindow::drawCounterSection() {
    beginCard("##card_counters", ICON_FA_LAYER_GROUP, "Project counters");

    if (counterGroups.empty()) {
        detailLine("No counters reported yet");
        endCard();
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
                    const double fraction = clamp01(entry.value / entry.max);
                    std::snprintf(overlay, sizeof(overlay), "%s: %.0f / %.0f",
                                  entry.label.c_str(), entry.value, entry.max);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pressureColor(fraction));
                    ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-1.0f, 0.0f), overlay);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Text("%s: %.0f", entry.label.c_str(), entry.value);
                }
            }
        }
    }

    endCard();
}

void ResourceMonitorWindow::drawSamplerFooter() {
    const double interval = SystemStats::getRefreshInterval();
    char windowText[32];
    char detail[128];
    std::snprintf(detail, sizeof(detail), "Every %.2fs  |  %d points  |  %s of history",
                  interval, HISTORY_SAMPLES,
                  fmtDuration(windowText, sizeof(windowText), interval * HISTORY_SAMPLES));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
    ImGui::TextUnformatted(detail);
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

    // The machine-wide snapshot is shared with the footer HUD and self-throttles,
    // so this window just watches the revision counter for fresh data. Pausing
    // freezes the copy on screen without stopping the shared sampler.
    if (!paused) {
        const SystemSample& fresh = SystemStats::current();
        if (SystemStats::revision() != lastRevision) {
            lastRevision = SystemStats::revision();
            system = fresh;
            sampleOnce();
        }
    }

    drawToolbar();
    ImGui::Separator();

    drawSystemCard();
    drawGpuCard();
    drawNetworkCard();
    drawEditorSection();
    drawChildSection();
    drawCounterSection();
    drawSamplerFooter();

    ImGui::End();

    if (wasOpen && !windowOpen) {
        setOpen(false);
    }
}
