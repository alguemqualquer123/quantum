// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "util/ProcessStats.h"
#include "util/SystemStats.h"

namespace quantum::editor {
    // System-monitor style panel: machine-wide CPU, memory, GPU/VRAM, temperature
    // and network throughput from SystemStats, plus live CPU & RAM for the editor
    // and any child process it launches, plus caller-fed asset/GPU counters. All
    // sampling is time-gated on the UI thread inside show(); there is no worker
    // thread, and the machine-wide side is shared with the footer HUD.
    class ResourceMonitorWindow {
    private:
        struct ChildProcess {
            unsigned long long pid = 0;
            std::string label;              // caller-supplied tag; shown alongside the probed name
            double registeredTime = 0.0;    // ImGui::GetTime() at registration, for the uptime column
            ProcessSample sample;           // last sampled values, zeroed until the first refresh
        };

        struct CounterEntry {
            std::string label;
            double value = 0.0;
            double max = 0.0;               // <= 0 hides the bar and shows a plain number instead
        };

        static constexpr int HISTORY_SAMPLES = 240;  // one point per refresh; window length follows the rate

        // Fixed-size rings plot straight through ImGui's values_offset wrap, so no
        // realloc and no per-frame heap churn once the window exists.
        std::vector<float> editorCpuHistory;
        std::vector<float> editorRamHistory;   // megabytes
        std::vector<float> systemCpuHistory;   // percent
        std::vector<float> systemRamHistory;   // megabytes
        std::vector<float> gpuHistory;         // percent
        std::vector<float> vramHistory;        // megabytes
        std::vector<float> downHistory;        // kilobytes per second
        std::vector<float> upHistory;          // kilobytes per second
        int historyHead = 0;                   // next write slot; doubles as the PlotLines offset

        SystemSample system;                   // last machine-wide snapshot (frozen while paused)
        unsigned long long lastRevision = 0;   // SystemStats revision `system` was captured at

        double editorCpuPercent = 0.0;
        uint64_t editorRssBytes = 0;

        std::vector<ChildProcess> children;  // registration order is preserved for a stable table

        std::vector<std::string> counterGroups;                              // group names, in insertion order
        std::unordered_map<std::string, std::vector<CounterEntry>> counters;  // group -> entries

        bool windowOpen;
        bool focusRequested;
        bool paused;
        int refreshRateIndex = 1;             // index into the sampling-rate combo

        void sampleOnce();
        void pushHistory();
        void drawToolbar();
        void drawSystemCard();
        void drawGpuCard();
        void drawNetworkCard();
        void drawEditorSection();
        void drawChildSection();
        void drawCounterSection();
        void drawSamplerFooter();

        size_t findChild(unsigned long long pid) const;

    public:
        static constexpr const char* WINDOW_NAME = "Resource Monitor";

        // No engine dependencies: children and counters arrive through the setters below,
        // so App wires this up without exposing engine internals.
        ResourceMonitorWindow();

        // Call every frame; sampling is self-throttled, drawing is cheap. Main thread only.
        void show();
        void setOpen(bool open);
        bool isOpen() const;

        // Register a child process to track (engine run, resource compiler, build generator).
        // Re-adding an existing pid only refreshes its label. Call from the main thread.
        void addChildProcess(unsigned long long pid, const std::string& label);
        void removeChildProcess(unsigned long long pid);
        void clearChildProcesses();

        // Feed one labeled bar: group buckets the panel (e.g. "GPU", "Scene"), label names the
        // metric. max > 0 renders a filled bar of value/max; max <= 0 shows the raw number.
        // Calling again with the same group + label updates it. Call from the main thread.
        void setCounter(const std::string& group, const std::string& label, double value, double max);

        // Sampler controls, exposed so App can drive them from a menu if it wants to.
        void setPaused(bool paused);
        bool isPaused() const;
        void resetHistory();
    };
}
