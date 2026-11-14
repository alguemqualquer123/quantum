// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quantum::editor {
    // One process as seen by a Resource Monitor snapshot. CPU and the per-process
    // memory deltas are relative to the previous sample of the same pid, so the
    // first read of a process reports cpuPercent == 0.
    struct ProcessSample {
        std::string name;
        unsigned long long pid = 0;
        double cpuPercent = 0.0;
        uint64_t workingSetBytes = 0;
        uint64_t privateBytes = 0;
        // False when the pid could not be opened/read: the child exited or is
        // protected. Every other field is zeroed in that case.
        bool alive = false;
    };

    // Dependency-free process sampler for the Resource Monitor window. Reads the
    // current process and a caller-supplied pid set; it never allocates a thread
    // and never throws, so any failure just yields zeroed samples. The header pulls
    // in nothing beyond the standard library, including on the engine side.
    class ProcessStats {
    public:
        // Current-process CPU percent (all cores folded into one 0..100*scale number)
        // and resident memory in bytes. cpuPercent is 0 on the first call because no
        // baseline exists yet.
        static void selfUsage(double& cpuPercent, uint64_t& rssBytes);

        // Per-pid snapshot for the given child processes. Returns exactly one entry
        // per requested pid, in the same order; pids that cannot be queried come back
        // zeroed. Keep the pid list stable between calls so the CPU deltas line up.
        static std::vector<ProcessSample> snapshot(const std::vector<unsigned long long>& pids);
    };
}
