// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace doriax::editor {
    // Machine-wide resource snapshot for the Resource Monitor window and the editor
    // footer HUD. Everything here is best-effort: a platform (or a locked-down
    // machine) that cannot read a value leaves it at its "unavailable" marker -
    // temperatures and percentages stay negative, byte counters stay 0 - and the
    // UI renders N/A instead of hiding the row.
    //
    // The header pulls in nothing beyond the standard library, so the editor can
    // include it anywhere; all platform APIs live behind the pimpl-free samplers
    // in SystemStats.cpp.

    struct CpuStats {
        std::string name;              // e.g. "AMD Ryzen 7 5800X"; empty = unknown
        unsigned int logicalCores = 0;
        double usagePercent = 0.0;     // 0..100 across every core, Task-Manager style
        double temperatureC = -1.0;    // < 0 = unavailable
    };

    struct MemoryStats {
        uint64_t totalBytes = 0;
        uint64_t usedBytes = 0;
        uint64_t availableBytes = 0;
        // Commit charge, not RAM: includes page-file/swap backing. 0 = unknown.
        uint64_t commitUsedBytes = 0;
        uint64_t commitLimitBytes = 0;
    };

    struct GpuStats {
        std::string name;              // adapter description; empty = unknown
        double usagePercent = -1.0;    // busiest engine, 0..100; < 0 = unavailable
        uint64_t vramUsedBytes = 0;
        uint64_t vramTotalBytes = 0;   // 0 = unavailable
        double temperatureC = -1.0;    // < 0 = unavailable (needs NVIDIA/NVML)
    };

    struct NetworkInterfaceStats {
        std::string name;
        double downBytesPerSec = 0.0;
        double upBytesPerSec = 0.0;
        uint64_t downBytesTotal = 0;   // accumulated while the sampler lived
        uint64_t upBytesTotal = 0;
    };

    struct NetworkStats {
        double downBytesPerSec = 0.0;  // summed over every non-loopback interface
        double upBytesPerSec = 0.0;
        uint64_t downBytesTotal = 0;
        uint64_t upBytesTotal = 0;
        std::vector<NetworkInterfaceStats> interfaces;  // stable (name) order
    };

    struct SystemSample {
        CpuStats cpu;
        MemoryStats memory;
        GpuStats gpu;
        NetworkStats network;
        double uptimeSeconds = 0.0;
        double sampleSeconds = 0.0;    // wall time covered by this refresh
        bool valid = false;            // false until the first read completes
    };

    // Shared, self-throttled system sampler. Every UI caller gets the same cached
    // SystemSample and the OS is queried at most once per refresh interval, so the
    // footer HUD and the monitor window never double the platform calls. Main
    // thread only, matching the rest of the editor's non-threaded utilities.
    class SystemStats {
    public:
        // Freshest snapshot; only hits the OS once the interval elapsed. Never
        // throws: if a platform call fails the previous (or an empty) sample is
        // returned with valid == false until something succeeds.
        static const SystemSample& current();

        // Drop rate baselines (network rates read 0 again) and force the next
        // current() to re-query the OS. Call from the UI thread.
        static void invalidate();

        // Sampling cadence in seconds, clamped to [0.2, 5.0]. Default 0.5.
        static void setRefreshInterval(double seconds);
        static double getRefreshInterval();

        // Bumped every time a fresh OS read lands, so consumers can decide to push
        // a history point per sample instead of keeping their own timers.
        static unsigned long long revision();
    };
}
