// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ProcessStats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace doriax::editor {
    namespace {
        // Wall clock in seconds since an arbitrary epoch; used as the denominator
        // for the CPU-time deltas that GetProcessTimes / /proc report.
        double wallSeconds() {
            using Clock = std::chrono::steady_clock;
            static const Clock::time_point origin = Clock::now();
            return std::chrono::duration<double>(Clock::now() - origin).count();
        }

        double cores() {
#if defined(_WIN32)
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            double n = static_cast<double>(info.dwNumberOfProcessors);
#else
            long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
            return n > 0.0 ? n : 1.0;
        }

        // Percent of one process' CPU busy time over the interval since its previous
        // read, Task-Manager style: 100 means one full core, numCores means everything.
        // A process seen for the first time has no baseline, so it reports 0.
        struct CpuBaseline {
            unsigned long long busy = 0;  // cpu time in 100ns ticks
            double wall = 0.0;            // wallSeconds() at the previous read
            bool valid = false;
        };

        double cpuPercentFrom(unsigned long long busyTicks, CpuBaseline& baseline) {
            const double now = wallSeconds();
            if (!baseline.valid) {
                baseline.busy = busyTicks;
                baseline.wall = now;
                baseline.valid = true;
                return 0.0;
            }

            const double busyDeltaSeconds = static_cast<double>(busyTicks - baseline.busy) * 1e-7;
            const double wallDelta = now - baseline.wall;
            baseline.busy = busyTicks;
            baseline.wall = now;
            if (wallDelta <= 0.0) {
                return 0.0;
            }

            const double pct = 100.0 * busyDeltaSeconds / (wallDelta * cores());
            return pct > 0.0 ? pct : 0.0;
        }

#if defined(_WIN32)
        unsigned long long filetimeToTicks(const FILETIME& ft) {
            ULARGE_INTEGER ul{};
            ul.LowPart = ft.dwLowDateTime;
            ul.HighPart = ft.dwHighDateTime;
            return ul.QuadPart;
        }

        unsigned long long processBusyTicks(HANDLE process) {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
                return 0;
            }
            return filetimeToTicks(kernel) + filetimeToTicks(user);
        }

        std::string trimToName(const std::string& path) {
            const size_t sep = path.find_last_of("\\/");
            return sep == std::string::npos ? path : path.substr(sep + 1);
        }

        std::string processName(HANDLE process) {
            char buffer[MAX_PATH];
            DWORD size = static_cast<DWORD>(sizeof(buffer));
            if (QueryFullProcessImageNameA(process, 0, buffer, &size) && size > 0) {
                return trimToName(std::string(buffer, size));
            }
            return {};
        }
#else
        // Read a whole /proc pseudo-file. These report size 0 through stat(), so the
        // content is pulled in fixed chunks until fread comes up short. Never throws:
        // a missing or unreadable file (as on macOS, which has no /proc) yields "".
        std::string readProcFile(const std::string& path) {
            std::string out;
            std::FILE* file = std::fopen(path.c_str(), "rb");
            if (!file) {
                return out;
            }
            char chunk[4096];
            size_t read = 0;
            while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
                out.append(chunk, read);
            }
            std::fclose(file);
            return out;
        }

        // utime + stime from /proc/<pid>/stat, converted to 100ns ticks so the caller
        // can share one cpuPercentFrom path with the Windows branch. The comm field can
        // hold spaces and parentheses, so parsing resumes after the last ')'.
        unsigned long long processBusyTicks(long pid) {
            const std::string stat = readProcFile("/proc/" + std::to_string(pid) + "/stat");
            const size_t close = stat.rfind(')');
            if (close == std::string::npos || close + 2 >= stat.size()) {
                return 0;
            }

            // Fields after ')': [0]=state ... utime is field 14 (index 11), stime 15 (12).
            std::vector<std::string> fields;
            const char* p = stat.data() + close + 2;
            const char* end = stat.data() + stat.size();
            while (p < end) {
                while (p < end && *p == ' ') p++;
                const char* token = p;
                while (p < end && *p != ' ') p++;
                fields.emplace_back(token, static_cast<size_t>(p - token));
            }
            if (fields.size() < 13) {
                return 0;
            }

            const unsigned long long utime = std::strtoull(fields[11].c_str(), nullptr, 10);
            const unsigned long long stime = std::strtoull(fields[12].c_str(), nullptr, 10);

            long ticksPerSecond = sysconf(_SC_CLK_TCK);
            if (ticksPerSecond <= 0) {
                ticksPerSecond = 100;  // the near-universal Linux default
            }
            const unsigned long long hundredNsPerTick = 10000000ULL / static_cast<unsigned long long>(ticksPerSecond);
            return (utime + stime) * hundredNsPerTick;
        }

        // VmRSS from /proc/<pid>/status, in kB then scaled to bytes.
        uint64_t readRssBytes(const std::string& path) {
            const std::string status = readProcFile(path);
            const size_t mark = status.find("VmRSS:");
            if (mark == std::string::npos) {
                return 0;
            }
            const char* p = status.data() + mark + 6;
            const char* end = status.data() + status.size();
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            const unsigned long long value = std::strtoull(p, nullptr, 10);
            return value * 1024ULL;
        }

        // /proc/<pid>/comm holds just the executable name plus a trailing newline.
        std::string readProcessName(long pid) {
            std::string name = readProcFile("/proc/" + std::to_string(pid) + "/comm");
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
                name.pop_back();
            }
            return name;
        }
#endif
    }  // namespace

    void ProcessStats::selfUsage(double& cpuPercent, uint64_t& rssBytes) {
        cpuPercent = 0.0;
        rssBytes = 0;

        static std::unordered_map<unsigned long long, CpuBaseline> baselines;
        CpuBaseline& baseline = baselines[0];  // pid 0 stands for "this process"

#if defined(_WIN32)
        HANDLE process = GetCurrentProcess();  // pseudo-handle, always valid here
        cpuPercentFrom(processBusyTicks(process), baseline);

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            rssBytes = static_cast<uint64_t>(pmc.WorkingSetSize);
        }
#else
        const long pid = static_cast<long>(getpid());
        cpuPercentFrom(processBusyTicks(pid), baseline);
        rssBytes = readRssBytes("/proc/self/status");
#endif
    }

    std::vector<ProcessSample> ProcessStats::snapshot(const std::vector<unsigned long long>& pids) {
        std::vector<ProcessSample> samples;
        samples.reserve(pids.size());

        static std::unordered_map<unsigned long long, CpuBaseline> baselines;

        for (unsigned long long pid : pids) {
            ProcessSample sample;
            sample.pid = pid;

#if defined(_WIN32)
            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
            if (process) {
                sample.name = processName(process);

                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                    sample.workingSetBytes = static_cast<uint64_t>(pmc.WorkingSetSize);
                    sample.privateBytes = static_cast<uint64_t>(pmc.PrivateUsage);
                }

                CpuBaseline& baseline = baselines[pid];
                sample.cpuPercent = cpuPercentFrom(processBusyTicks(process), baseline);
                CloseHandle(process);
            }
#else
            // macOS has no /proc, so every read fails and the sample stays zeroed.
            const long nativePid = static_cast<long>(pid);
            sample.name = readProcessName(nativePid);
            // RSS doubles as the working set; Linux exposes no cheap per-process private-byte count.
            sample.workingSetBytes = readRssBytes("/proc/" + std::to_string(pid) + "/status");

            CpuBaseline& baseline = baselines[pid];
            sample.cpuPercent = cpuPercentFrom(processBusyTicks(nativePid), baseline);
#endif

            samples.push_back(std::move(sample));
        }

        return samples;
    }
}
