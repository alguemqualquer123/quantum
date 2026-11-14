// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "SystemStats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <dxgi1_4.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <ctime>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/machine.h>  // CPU_STATE_IDLE / CPU_STATE_MAX
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_var.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include <unistd.h>
#endif

namespace quantum::editor {

namespace {
    // Monotonic wall clock in seconds; the denominator of every rate below.
    double steadySeconds() {
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point origin = Clock::now();
        return std::chrono::duration<double>(Clock::now() - origin).count();
    }

    double clampDouble(double value, double low, double high) {
        return value < low ? low : (value > high ? high : value);
    }

    // Converts a cumulative interface counter pair into per-second rates and
    // session totals. Shared by every platform so the rate math only exists once.
    struct InterfaceTicker {
        struct Baseline {
            uint64_t rx = 0;
            uint64_t tx = 0;
            uint64_t downTotal = 0;
            uint64_t upTotal = 0;
            bool valid = false;
        };

        std::unordered_map<std::string, Baseline> baselines;
        std::unordered_set<std::string> seen;
        std::vector<NetworkInterfaceStats>* out = nullptr;
        double seconds = 0.5;

        void begin(std::vector<NetworkInterfaceStats>* interfaces, double interval) {
            out = interfaces;
            seconds = interval > 0.001 ? interval : 0.001;
            seen.clear();
        }

        void add(const std::string& name, uint64_t rx, uint64_t tx) {
            seen.insert(name);
            Baseline& baseline = baselines[name];

            NetworkInterfaceStats entry;
            entry.name = name;
            if (baseline.valid && rx >= baseline.rx && tx >= baseline.tx) {
                const uint64_t rxDelta = rx - baseline.rx;
                const uint64_t txDelta = tx - baseline.tx;
                entry.downBytesPerSec = static_cast<double>(rxDelta) / seconds;
                entry.upBytesPerSec = static_cast<double>(txDelta) / seconds;
                baseline.downTotal += rxDelta;
                baseline.upTotal += txDelta;
            }
            // Counters that reset (interface re-created) just restart the baseline.
            baseline.rx = rx;
            baseline.tx = tx;
            baseline.valid = true;

            entry.downBytesTotal = baseline.downTotal;
            entry.upBytesTotal = baseline.upTotal;
            out->push_back(std::move(entry));
        }

        // Sum the per-interface entries and drop baselines for interfaces that
        // disappeared, so a VPN toggling off does not leak state forever.
        void finish(NetworkStats& network) {
            network.downBytesPerSec = 0.0;
            network.upBytesPerSec = 0.0;
            network.downBytesTotal = 0;
            network.upBytesTotal = 0;
            for (const NetworkInterfaceStats& entry : *out) {
                network.downBytesPerSec += entry.downBytesPerSec;
                network.upBytesPerSec += entry.upBytesPerSec;
                network.downBytesTotal += entry.downBytesTotal;
                network.upBytesTotal += entry.upBytesTotal;
            }
            for (auto it = baselines.begin(); it != baselines.end();) {
                it = seen.count(it->first) ? std::next(it) : baselines.erase(it);
            }
        }

        void clear() {
            baselines.clear();
            seen.clear();
        }
    };

    // ---- NVML ------------------------------------------------------------------
    // The NVIDIA driver ships NVML, which reports exact GPU utilization, VRAM and
    // temperature with no process spawn. It is loaded dynamically so machines
    // without the driver (or with another vendor) pay nothing and stay on the
    // generic per-platform path. ABI below is documented and stable since 2013.
    typedef void* NvmlDevice;
    struct NvmlUtilization {
        unsigned int gpu;
        unsigned int memory;
    };
    struct NvmlMemory {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    };

#if defined(_WIN32)
    void* openNvmlLibrary() {
        // Current drivers install the DLL into System32; older ones kept it next
        // to nvidia-smi, so try both before giving up.
        void* handle = reinterpret_cast<void*>(LoadLibraryA("nvml.dll"));
        if (!handle) {
            const char* programFiles = std::getenv("ProgramFiles");
            if (programFiles) {
                const std::string path = std::string(programFiles) + "\\NVIDIA Corporation\\NVSMI\\nvml.dll";
                handle = reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
            }
        }
        return handle;
    }

    void* nvmlSymbol(void* library, const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
    }
#else
    void* openNvmlLibrary() {
        return dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    }

    void* nvmlSymbol(void* library, const char* name) {
        return dlsym(library, name);
    }
#endif

    struct NvmlApi {
        bool tried = false;
        bool available = false;
        void* library = nullptr;
        NvmlDevice device = nullptr;
        std::string name;

        int (*init)() = nullptr;
        int (*getHandle)(unsigned int, NvmlDevice*) = nullptr;
        int (*getName)(NvmlDevice, char*, unsigned int) = nullptr;
        int (*getTemperature)(NvmlDevice, int, unsigned int*) = nullptr;
        int (*getUtilization)(NvmlDevice, NvmlUtilization*) = nullptr;
        int (*getMemory)(NvmlDevice, NvmlMemory*) = nullptr;

        void open() {
            if (tried) {
                return;
            }
            tried = true;
            library = openNvmlLibrary();
            if (!library) {
                return;
            }

            init = reinterpret_cast<int (*)()>(nvmlSymbol(library, "nvmlInit_v2"));
            if (!init) {
                init = reinterpret_cast<int (*)()>(nvmlSymbol(library, "nvmlInit"));
            }
            getHandle = reinterpret_cast<int (*)(unsigned int, NvmlDevice*)>(nvmlSymbol(library, "nvmlDeviceGetHandleByIndex_v2"));
            if (!getHandle) {
                getHandle = reinterpret_cast<int (*)(unsigned int, NvmlDevice*)>(nvmlSymbol(library, "nvmlDeviceGetHandleByIndex"));
            }
            getName = reinterpret_cast<int (*)(NvmlDevice, char*, unsigned int)>(nvmlSymbol(library, "nvmlDeviceGetName"));
            getTemperature = reinterpret_cast<int (*)(NvmlDevice, int, unsigned int*)>(nvmlSymbol(library, "nvmlDeviceGetTemperature"));
            getUtilization = reinterpret_cast<int (*)(NvmlDevice, NvmlUtilization*)>(nvmlSymbol(library, "nvmlDeviceGetUtilizationRates"));
            getMemory = reinterpret_cast<int (*)(NvmlDevice, NvmlMemory*)>(nvmlSymbol(library, "nvmlDeviceGetMemoryInfo"));

            if (!init || !getHandle || init() != 0) {
                return;
            }
            if (getHandle(0, &device) != 0 || !device) {
                return;
            }
            available = true;

            if (getName) {
                char buffer[128] = {};
                if (getName(device, buffer, sizeof(buffer)) == 0) {
                    name = buffer;
                }
            }
        }

        void fillGpu(GpuStats& gpu) {
            if (!available) {
                return;
            }
            if (!name.empty()) {
                gpu.name = name;
            }
            NvmlUtilization utilization{};
            if (getUtilization && getUtilization(device, &utilization) == 0) {
                gpu.usagePercent = clampDouble(static_cast<double>(utilization.gpu), 0.0, 100.0);
            }
            NvmlMemory memory{};
            if (getMemory && getMemory(device, &memory) == 0 && memory.total > 0) {
                gpu.vramTotalBytes = memory.total;
                gpu.vramUsedBytes = memory.used;
            }
            unsigned int temperature = 0;
            if (getTemperature && getTemperature(device, 0, &temperature) == 0) {
                gpu.temperatureC = static_cast<double>(temperature);
            }
        }
    };

#if defined(_WIN32)
    // ---- Windows ---------------------------------------------------------------
    std::string utf8FromWide(const wchar_t* wide) {
        if (!wide || !*wide) {
            return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return {};
        }
        std::string out(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    // Intel leaves trailing spaces in the registry string; trim for display.
    void trimTrailingSpaces(std::string& text) {
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
            text.pop_back();
        }
    }

    std::string registryCpuName() {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
            return {};
        }
        wchar_t buffer[256] = {};
        DWORD size = static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t));
        std::string name;
        if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            name = utf8FromWide(buffer);
            trimTrailingSpaces(name);
        }
        RegCloseKey(key);
        return name;
    }

    struct CpuTicks {
        uint64_t idle = 0;
        uint64_t kernel = 0;
        uint64_t user = 0;
        bool valid = false;
    };

    uint64_t fileTimeTicks(const FILETIME& time) {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return value.QuadPart;
    }

    // GetSystemTimes returns cumulative kernel/user/idle time; the kernel bucket
    // already contains idle, so busy = 1 - idle/(kernel + user).
    double systemCpuPercent(CpuTicks& baseline) {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) {
            return 0.0;
        }
        const uint64_t idleNow = fileTimeTicks(idle);
        const uint64_t kernelNow = fileTimeTicks(kernel);
        const uint64_t userNow = fileTimeTicks(user);

        if (!baseline.valid) {
            baseline = CpuTicks{idleNow, kernelNow, userNow, true};
            return 0.0;
        }

        const uint64_t idleDelta = idleNow - baseline.idle;
        const uint64_t totalDelta = (kernelNow - baseline.kernel) + (userNow - baseline.user);
        baseline = CpuTicks{idleNow, kernelNow, userNow, true};
        if (totalDelta == 0) {
            return 0.0;
        }
        const double busy = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
        return clampDouble(busy, 0.0, 1.0) * 100.0;
    }

    // --------- GPU ---------
    // Three sources, cheapest first:
    //   * NVML (driver DLL, loaded dynamically) - NVIDIA only, exact usage, VRAM
    //     and temperature with no process spawn.
    //   * PDH "GPU Engine" (English counter names, locale-proof) - per-process,
    //     per-engine utilization; summed per adapter LUID into one number.
    //   * DXGI - adapter name plus dedicated VRAM total/current usage.
    // Instance format: pid_1234_luid_0x00000000_0x0000C5A1_phys_0_eng_0_engtype_3D
    std::string parseGpuEngineType(const wchar_t* instance) {
        if (!instance) {
            return {};
        }
        const wchar_t* mark = wcsstr(instance, L"engtype_");
        if (!mark) {
            return {};
        }
        mark += 8;
        std::string out;
        while (*mark && *mark != L'_') {
            out.push_back(static_cast<char>(*mark));
            ++mark;
        }
        return out;
    }

    bool parseGpuEngineInstance(const wchar_t* instance, LUID& luid, bool& hasLuid) {
        hasLuid = false;
        if (!instance) {
            return false;
        }
        const wchar_t* luidMark = wcsstr(instance, L"luid_");
        if (!luidMark) {
            return false;
        }
        luidMark += 5;
        wchar_t* end = nullptr;
        const unsigned long long low = wcstoull(luidMark, &end, 0);
        if (!end || *end != L'_') {
            return false;
        }
        const unsigned long long high = wcstoull(end + 1, &end, 0);
        if (!end) {
            return false;
        }
        luid.LowPart = static_cast<DWORD>(low);
        luid.HighPart = static_cast<LONG>(high);
        hasLuid = true;
        return true;
    }

    struct WindowsGpu {
        bool pdhTried = false;
        PDH_HQUERY query = nullptr;
        PDH_HCOUNTER engineCounter = nullptr;
        PDH_HCOUNTER thermalCounter = nullptr;
        bool collected = false;
        std::vector<BYTE> scratch;

        bool dxgiTried = false;
        IDXGIFactory1* factory = nullptr;
        IDXGIFactory4* factory4 = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        std::string adapterName;
        uint64_t vramTotal = 0;

        NvmlApi nvml;

        bool openPdh() {
            if (pdhTried) {
                return query != nullptr;
            }
            pdhTried = true;
            if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
                query = nullptr;
                return false;
            }
            // English counter names keep this working on localized Windows.
            if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engineCounter) != ERROR_SUCCESS) {
                engineCounter = nullptr;
            }
            if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\Temperature", 0, &thermalCounter) != ERROR_SUCCESS) {
                thermalCounter = nullptr;
            }
            if (!engineCounter && !thermalCounter) {
                PdhCloseQuery(query);
                query = nullptr;
            }
            return query != nullptr;
        }

        bool collectArray(PDH_HCOUNTER counter, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W** items) {
            DWORD size = 0;
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                             &size, count, nullptr) != PDH_MORE_DATA) {
                return false;
            }
            if (scratch.size() < size) {
                scratch.resize(size);
            }
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                             &size, count,
                                             reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(scratch.data())) != ERROR_SUCCESS) {
                return false;
            }
            *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(scratch.data());
            return true;
        }

        bool collect() {
            if (!openPdh()) {
                return false;
            }
            collected = PdhCollectQueryData(query) == ERROR_SUCCESS;
            return collected;
        }

        // Busiest adapter by summed engine utilization; also yields the LUID so the
        // DXGI side can report the matching adapter's name and VRAM.
        double sampleEngineUsage(LUID& busiestLuid, bool& hasBusiest) {
            hasBusiest = false;
            if (!collected || !engineCounter) {
                return -1.0;
            }

            DWORD count = 0;
            PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
            if (!collectArray(engineCounter, &count, &items)) {
                return -1.0;
            }

            struct EngineGroup {
                LUID luid{};
                bool hasLuid = false;
                std::string engtype;
                double sum = 0.0;
            };
            std::vector<EngineGroup> groups;
            for (DWORD i = 0; i < count; ++i) {
                const PDH_FMT_COUNTERVALUE_ITEM_W& item = items[i];
                if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA
                    && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
                    continue;
                }
                if (item.szName && wcsstr(item.szName, L"engtype_None")) {
                    continue;
                }
                const double value = item.FmtValue.doubleValue;
                if (!(value >= 0.0)) {
                    continue;
                }

                LUID luid{};
                bool hasLuid = false;
                parseGpuEngineInstance(item.szName, luid, hasLuid);
                const std::string engtype = parseGpuEngineType(item.szName);

                // One group per (adapter, engine type): the percentages of the
                // processes sharing an engine add up, different engines do not.
                size_t slot = groups.size();
                for (size_t g = 0; g < groups.size(); ++g) {
                    const bool sameAdapter = groups[g].hasLuid && hasLuid
                        && groups[g].luid.LowPart == luid.LowPart
                        && groups[g].luid.HighPart == luid.HighPart;
                    if (sameAdapter && groups[g].engtype == engtype) {
                        slot = g;
                        break;
                    }
                }
                if (slot == groups.size()) {
                    groups.push_back(EngineGroup{luid, hasLuid, engtype, 0.0});
                }
                groups[slot].sum += value;
            }

            if (groups.empty()) {
                return -1.0;
            }
            // Report the busiest engine, the way Task Manager's single GPU number
            // does; summing across engines would exceed what any engine is doing.
            size_t busiest = 0;
            for (size_t g = 1; g < groups.size(); ++g) {
                if (groups[g].sum > groups[busiest].sum) {
                    busiest = g;
                }
            }
            busiestLuid = groups[busiest].luid;
            hasBusiest = groups[busiest].hasLuid;
            return clampDouble(groups[busiest].sum, 0.0, 100.0);
        }

        // ACPI thermal zone in Kelvin; desktops without the counter report nothing.
        double sampleThermalZone() {
            if (!collected || !thermalCounter) {
                return -1.0;
            }
            DWORD count = 0;
            PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
            if (!collectArray(thermalCounter, &count, &items)) {
                return -1.0;
            }
            double hottest = -1.0;
            for (DWORD i = 0; i < count; ++i) {
                const PDH_FMT_COUNTERVALUE_ITEM_W& item = items[i];
                if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA
                    && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
                    continue;
                }
                const double kelvin = item.FmtValue.doubleValue;
                if (!(kelvin > 200.0 && kelvin < 450.0)) {
                    continue;
                }
                const double celsius = kelvin - 273.15;
                if (celsius > hottest) {
                    hottest = celsius;
                }
            }
            return hottest;
        }

        void openDxgi() {
            if (dxgiTried) {
                return;
            }
            dxgiTried = true;
            if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
                factory = nullptr;
                return;
            }
            factory->QueryInterface(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory4));
        }

        // Prefer the adapter the engine usage belonged to when the LUID is known;
        // otherwise the one with the most dedicated VRAM (the discrete GPU).
        void selectAdapter(bool haveLuid, LUID luid) {
            if (!factory || adapter) {
                return;
            }
            IDXGIAdapter1* chosen = nullptr;
            if (haveLuid && factory4) {
                factory4->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&chosen));
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (chosen) {
                chosen->GetDesc1(&desc);
            } else {
                DXGI_ADAPTER_DESC1 bestDesc{};
                for (UINT index = 0;; ++index) {
                    IDXGIAdapter1* candidate = nullptr;
                    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND || !candidate) {
                        break;
                    }
                    DXGI_ADAPTER_DESC1 candidateDesc{};
                    const bool usable = SUCCEEDED(candidate->GetDesc1(&candidateDesc))
                        && !(candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
                    if (usable && (!chosen || candidateDesc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory)) {
                        if (chosen) {
                            chosen->Release();
                        }
                        chosen = candidate;
                        bestDesc = candidateDesc;
                        continue;
                    }
                    candidate->Release();
                }
                desc = bestDesc;
            }

            adapter = chosen;
            if (adapter) {
                adapterName = utf8FromWide(desc.Description);
                vramTotal = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            }
        }

        uint64_t sampleVramUsed() {
            if (!adapter) {
                return 0;
            }
            IDXGIAdapter3* adapter3 = nullptr;
            if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) || !adapter3) {
                return 0;
            }
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            uint64_t used = 0;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                used = static_cast<uint64_t>(info.CurrentUsage);
            }
            // APUs keep everything in system memory, where the local segment reads
            // zero; the non-local group is the shared memory they actually use.
            if (used == 0
                && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
                used = static_cast<uint64_t>(info.CurrentUsage);
            }
            adapter3->Release();
            return used;
        }

        void sample(GpuStats& gpu, double& cpuTemperatureC) {
            collect();
            LUID busiestLuid{};
            bool hasBusiest = false;
            const double engineUsage = sampleEngineUsage(busiestLuid, hasBusiest);
            cpuTemperatureC = sampleThermalZone();

            openDxgi();
            selectAdapter(hasBusiest, busiestLuid);

            if (engineUsage >= 0.0) {
                gpu.usagePercent = engineUsage;
            }
            if (!adapterName.empty()) {
                gpu.name = adapterName;
            }
            if (vramTotal > 0) {
                gpu.vramTotalBytes = vramTotal;
                gpu.vramUsedBytes = sampleVramUsed();
            }

            // NVML beats the generic path on NVIDIA (adds temperature too).
            nvml.open();
            nvml.fillGpu(gpu);
        }

        void reset() {
            scratch.clear();
        }
    };
#endif  // _WIN32

#if !defined(_WIN32) && !defined(__APPLE__)
    // ---- Linux -----------------------------------------------------------------
    std::string readTextFile(const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return {};
        }
        std::string out;
        char chunk[512];
        size_t read = 0;
        while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            out.append(chunk, read);
        }
        std::fclose(file);
        return out;
    }

    // Value after "key" in a /proc file ("MemTotal: 16384 kB"), up to the next
    // space. Keys may be passed with or without the trailing colon.
    bool readProcKey(const std::string& text, const char* key, unsigned long long& value) {
        const size_t mark = text.find(key);
        if (mark == std::string::npos) {
            return false;
        }
        const char* p = text.c_str() + mark + std::strlen(key);
        while (*p == ' ' || *p == '\t' || *p == ':') {
            ++p;
        }
        char* end = nullptr;
        value = std::strtoull(p, &end, 10);
        return end != p;
    }

    bool readSysfsNumber(const std::string& path, long long& value) {
        const std::string text = readTextFile(path);
        if (text.empty()) {
            return false;
        }
        // Vendor ids come as "0x1002"; temperatures and percents as plain decimal.
        // Base 0 would misread a leading zero as octal, so pick the base explicitly.
        const int base = (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 16 : 10;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, base);
        if (end == text.c_str()) {
            return false;
        }
        value = parsed;
        return true;
    }

    struct LinuxCpu {
        uint64_t total = 0;
        uint64_t idle = 0;
        bool valid = false;
    };

    // /proc/stat first line: cpu user nice system idle iowait irq softirq steal ...
    // iowait counts as idle time, matching top/htop.
    double linuxCpuPercent(LinuxCpu& baseline) {
        const std::string stat = readTextFile("/proc/stat");
        if (stat.rfind("cpu ", 0) != 0) {
            return 0.0;
        }
        unsigned long long values[10] = {};
        const char* p = stat.c_str() + 4;
        int fields = 0;
        while (fields < 10) {
            while (*p == ' ') ++p;
            if (*p < '0' || *p > '9') break;
            char* end = nullptr;
            values[fields++] = std::strtoull(p, &end, 10);
            p = end;
        }

        uint64_t total = 0;
        for (int i = 0; i < fields; ++i) total += values[i];
        const uint64_t idle = values[3] + values[4];

        if (!baseline.valid) {
            baseline = LinuxCpu{total, idle, true};
            return 0.0;
        }
        const uint64_t totalDelta = total - baseline.total;
        const uint64_t idleDelta = idle - baseline.idle;
        baseline = LinuxCpu{total, idle, true};
        if (totalDelta == 0) return 0.0;
        const double busy = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
        return clampDouble(busy, 0.0, 1.0) * 100.0;
    }

    std::string linuxCpuName() {
        std::FILE* file = std::fopen("/proc/cpuinfo", "rb");
        if (!file) {
            return {};
        }
        std::string name;
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), file)) {
            if (std::strncmp(buffer, "model name", 10) == 0) {
                const char* colon = std::strchr(buffer, ':');
                if (colon && colon[1] == ' ') {
                    name = colon + 2;
                    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
                        name.pop_back();
                    }
                }
                break;
            }
        }
        std::fclose(file);
        return name;
    }

    unsigned int linuxCpuCount() {
        const long count = sysconf(_SC_NPROCESSORS_ONLN);
        return count > 0 ? static_cast<unsigned int>(count) : 0;
    }

    // Prefer package/core sensors; "acpitz" is a chassis zone and reads cold.
    double linuxCpuTemperature() {
        double fallback = -1.0;
        for (int zone = 0; zone < 16; ++zone) {
            const std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(zone);
            const std::string type = readTextFile(base + "/type");
            if (type.empty()) {
                break;
            }
            long long milli = 0;
            if (!readSysfsNumber(base + "/temp", milli)) {
                continue;
            }
            const double celsius = static_cast<double>(milli) / 1000.0;
            if (!(celsius > 0.0 && celsius < 150.0)) {
                continue;
            }
            if (type.find("x86_pkg") != std::string::npos || type.find("cpu") != std::string::npos
                || type.find("k10") != std::string::npos || type.find("zen") != std::string::npos
                || type.find("soc") != std::string::npos) {
                return celsius;
            }
            if (fallback < 0.0 && type.find("acpi") != std::string::npos) {
                fallback = celsius;
            }
        }

        static const char* kDriverNames[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal"};
        for (int hwmon = 0; hwmon < 16; ++hwmon) {
            const std::string base = "/sys/class/hwmon/hwmon" + std::to_string(hwmon);
            const std::string driver = readTextFile(base + "/name");
            if (driver.empty()) {
                break;
            }
            for (const char* wanted : kDriverNames) {
                if (driver.rfind(wanted, 0) == 0) {
                    long long milli = 0;
                    if (readSysfsNumber(base + "/temp1_input", milli)) {
                        const double celsius = static_cast<double>(milli) / 1000.0;
                        if (celsius > 0.0 && celsius < 150.0) {
                            return celsius;
                        }
                    }
                }
            }
        }
        return fallback;
    }

    // amdgpu exposes everything through sysfs; other drivers need NVML or stay N/A.
    struct LinuxGpu {
        bool discovered = false;
        bool found = false;
        std::string cardPath;
        NvmlApi nvml;

        void discover() {
            if (discovered) {
                return;
            }
            discovered = true;
            for (int card = 0; card < 8 && !found; ++card) {
                const std::string base = "/sys/class/drm/card" + std::to_string(card);
                long long vendor = 0;
                if (!readSysfsNumber(base + "/device/vendor", vendor)) {
                    continue;
                }
                if ((vendor & 0xFFFF) != 0x1002) {  // AMD
                    continue;
                }
                if (readTextFile(base + "/device/gpu_busy_percent").empty()) {
                    continue;
                }
                cardPath = base + "/device";
                found = true;
            }
        }

        void sample(GpuStats& gpu) {
            nvml.open();
            if (nvml.available) {
                nvml.fillGpu(gpu);
                return;
            }

            discover();
            if (!found) {
                return;
            }
            long long busy = 0;
            if (readSysfsNumber(cardPath + "/gpu_busy_percent", busy)) {
                gpu.usagePercent = clampDouble(static_cast<double>(busy), 0.0, 100.0);
            }
            long long used = 0;
            long long total = 0;
            if (readSysfsNumber(cardPath + "/mem_info_vram_used", used)
                && readSysfsNumber(cardPath + "/mem_info_vram_total", total) && total > 0) {
                gpu.vramUsedBytes = static_cast<uint64_t>(used);
                gpu.vramTotalBytes = static_cast<uint64_t>(total);
            }
            if (gpu.name.empty()) {
                gpu.name = "AMD GPU (amdgpu)";
            }

            long long milli = 0;
            for (int hwmon = 0; hwmon < 8; ++hwmon) {
                const std::string base = cardPath + "/hwmon/hwmon" + std::to_string(hwmon);
                if (readSysfsNumber(base + "/temp1_input", milli) && milli > 0 && milli < 150000) {
                    gpu.temperatureC = static_cast<double>(milli) / 1000.0;
                    break;
                }
            }
        }
    };
#endif  // Linux

#if defined(__APPLE__)
    // ---- macOS -----------------------------------------------------------------
    struct DarwinCpu {
        uint64_t total = 0;
        uint64_t idle = 0;
        bool valid = false;
    };

    double darwinCpuPercent(DarwinCpu& baseline) {
        host_cpu_load_info_data_t load{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                            reinterpret_cast<host_info_t>(&load), &count) != KERN_SUCCESS) {
            return 0.0;
        }
        uint64_t total = 0;
        for (int i = 0; i < CPU_STATE_MAX; ++i) {
            total += load.cpu_ticks[i];
        }
        const uint64_t idle = load.cpu_ticks[CPU_STATE_IDLE];

        if (!baseline.valid) {
            baseline = DarwinCpu{total, idle, true};
            return 0.0;
        }
        const uint64_t totalDelta = total - baseline.total;
        const uint64_t idleDelta = idle - baseline.idle;
        baseline = DarwinCpu{total, idle, true};
        if (totalDelta == 0) return 0.0;
        const double busy = 1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta);
        return clampDouble(busy, 0.0, 1.0) * 100.0;
    }

    template <typename T>
    bool sysctlValue(const char* name, T& value) {
        size_t size = sizeof(value);
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0;
    }

    NvmlApi g_darwinNvml;
#endif  // __APPLE__

    // ---- Shared sampler state --------------------------------------------------
    struct SamplerState {
        bool valid = false;
        double interval = 0.5;
        double lastQuery = -1.0;
        unsigned long long revision = 0;
        SystemSample sample;
        InterfaceTicker ticker;

#if defined(_WIN32)
        CpuTicks cpuTicks;
        WindowsGpu gpu;
#elif defined(__APPLE__)
        DarwinCpu cpuTicks;
#else
        LinuxCpu cpuTicks;
        LinuxGpu gpu;
        std::string cpuName;
#endif
    };

    SamplerState& state() {
        static SamplerState instance;
        return instance;
    }

    // Reads every platform counter into out; returns true when at least the CPU or
    // memory side produced data, so a fully failed read can keep valid == false.
    bool platformSample(SamplerState& s, SystemSample& out, double dt) {
        bool any = false;

#if defined(_WIN32)
        out.cpu.usagePercent = systemCpuPercent(s.cpuTicks);
        any = true;

        static const std::string cpuName = registryCpuName();
        if (out.cpu.name.empty()) {
            out.cpu.name = cpuName;
        }
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        out.cpu.logicalCores = info.dwNumberOfProcessors;

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            out.memory.totalBytes = memory.ullTotalPhys;
            out.memory.availableBytes = memory.ullAvailPhys;
            out.memory.usedBytes = memory.ullTotalPhys - memory.ullAvailPhys;
            out.memory.commitLimitBytes = memory.ullTotalPageFile;
            out.memory.commitUsedBytes = memory.ullTotalPageFile - memory.ullAvailPageFile;
            any = true;
        }

        out.uptimeSeconds = static_cast<double>(GetTickCount64()) / 1000.0;

        out.network.interfaces.clear();
        s.ticker.begin(&out.network.interfaces, dt);
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) == NO_ERROR && table) {
            for (ULONG i = 0; i < table->NumEntries; ++i) {
                const MIB_IF_ROW2& row = table->Table[i];
                if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
                    continue;
                }
                if (row.OperStatus != IfOperStatusUp) {
                    continue;
                }
                // WFP/QoS filter bindings appear as their own rows with the base
                // adapter's alias plus a filter suffix; they would double-count.
                if (row.InterfaceAndOperStatusFlags.FilterInterface) {
                    continue;
                }
                std::string name = utf8FromWide(row.Alias);
                if (name.empty()) {
                    name = utf8FromWide(row.Description);
                }
                if (name.empty()) {
                    name = "if" + std::to_string(row.InterfaceIndex);
                }
                s.ticker.add(name, row.InOctets, row.OutOctets);
            }
            FreeMibTable(table);
            any = true;
        }
        s.ticker.finish(out.network);

        s.gpu.sample(out.gpu, out.cpu.temperatureC);

#elif defined(__APPLE__)
        out.cpu.usagePercent = darwinCpuPercent(s.cpuTicks);
        any = true;

        if (out.cpu.name.empty()) {
            char brand[128] = {};
            if (sysctlValue("machdep.cpu.brand_string", brand)) {
                out.cpu.name = brand;
            }
        }
        int cores = 0;
        if (sysctlValue("hw.logicalcpu", cores) && cores > 0) {
            out.cpu.logicalCores = static_cast<unsigned int>(cores);
        }

        uint64_t physical = 0;
        if (sysctlValue("hw.memsize", physical)) {
            vm_statistics64_data_t vm{};
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                  reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
                vm_size_t pageSize = 0;
                host_page_size(mach_host_self(), &pageSize);
                const uint64_t available = (static_cast<uint64_t>(vm.free_count)
                                            + static_cast<uint64_t>(vm.inactive_count))
                    * static_cast<uint64_t>(pageSize);
                out.memory.totalBytes = physical;
                out.memory.availableBytes = available < physical ? available : physical;
                out.memory.usedBytes = physical - out.memory.availableBytes;
                any = true;
            }
        }

        timeval boot{};
        if (sysctlValue("kern.boottime", boot) && boot.tv_sec > 0) {
            out.uptimeSeconds = std::difftime(std::time(nullptr), static_cast<time_t>(boot.tv_sec));
        }

        out.network.interfaces.clear();
        s.ticker.begin(&out.network.interfaces, dt);
        struct ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0) {
            for (struct ifaddrs* item = list; item; item = item->ifa_next) {
                if (!item->ifa_addr || item->ifa_addr->sa_family != AF_LINK || !item->ifa_data) {
                    continue;
                }
                if (item->ifa_flags & IFF_LOOPBACK) {
                    continue;
                }
                const struct if_data* data = static_cast<const struct if_data*>(item->ifa_data);
                s.ticker.add(item->ifa_name, data->ifi_ibytes, data->ifi_obytes);
            }
            freeifaddrs(list);
            any = true;
        }
        s.ticker.finish(out.network);

        g_darwinNvml.open();
        g_darwinNvml.fillGpu(out.gpu);

#else
        out.cpu.usagePercent = linuxCpuPercent(s.cpuTicks);
        any = true;

        if (s.cpuName.empty()) {
            s.cpuName = linuxCpuName();
        }
        if (out.cpu.name.empty()) {
            out.cpu.name = s.cpuName;
        }
        out.cpu.logicalCores = linuxCpuCount();

        const std::string meminfo = readTextFile("/proc/meminfo");
        unsigned long long totalKb = 0;
        unsigned long long availableKb = 0;
        unsigned long long freeKb = 0;
        unsigned long long swapTotalKb = 0;
        unsigned long long swapFreeKb = 0;
        if (readProcKey(meminfo, "MemTotal:", totalKb)) {
            readProcKey(meminfo, "MemAvailable:", availableKb);
            readProcKey(meminfo, "MemFree:", freeKb);
            readProcKey(meminfo, "SwapTotal:", swapTotalKb);
            readProcKey(meminfo, "SwapFree:", swapFreeKb);
            if (availableKb == 0) {
                availableKb = freeKb;  // pre-3.14 kernels have no MemAvailable
            }
            out.memory.totalBytes = totalKb * 1024ULL;
            out.memory.availableBytes = availableKb * 1024ULL;
            out.memory.usedBytes = out.memory.totalBytes - out.memory.availableBytes;
            out.memory.commitLimitBytes = (totalKb + swapTotalKb) * 1024ULL;
            out.memory.commitUsedBytes = out.memory.usedBytes + (swapTotalKb - swapFreeKb) * 1024ULL;
            any = true;
        }

        const std::string uptime = readTextFile("/proc/uptime");
        char* uptimeEnd = nullptr;
        const double uptimeSeconds = std::strtod(uptime.c_str(), &uptimeEnd);
        if (uptimeEnd != uptime.c_str()) {
            out.uptimeSeconds = uptimeSeconds;
        }

        out.network.interfaces.clear();
        s.ticker.begin(&out.network.interfaces, dt);
        const std::string netdev = readTextFile("/proc/net/dev");
        size_t lineStart = netdev.find('\n');  // skip the two header lines
        if (lineStart != std::string::npos) {
            lineStart = netdev.find('\n', lineStart + 1);
        }
        while (lineStart != std::string::npos && lineStart + 1 < netdev.size()) {
            const size_t lineEnd = netdev.find('\n', lineStart + 1);
            const std::string line = netdev.substr(
                lineStart + 1,
                lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart - 1);
            lineStart = lineEnd;

            const size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                name.erase(name.begin());
            }
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                name.pop_back();
            }
            if (name.empty() || name == "lo") {
                continue;
            }

            // Receive: bytes packets errs drop fifo frame compressed multicast;
            // transmit repeats the same eight fields after that.
            unsigned long long fields[16] = {};
            const char* p = line.c_str() + colon + 1;
            int fieldCount = 0;
            while (fieldCount < 16) {
                while (*p == ' ' || *p == '\t') ++p;
                if (*p < '0' || *p > '9') break;
                char* end = nullptr;
                fields[fieldCount++] = std::strtoull(p, &end, 10);
                p = end;
            }
            if (fieldCount >= 9) {
                s.ticker.add(name, fields[0], fields[8]);
            }
        }
        s.ticker.finish(out.network);
        any = true;

        s.gpu.sample(out.gpu);
        if (out.cpu.temperatureC < 0.0) {
            out.cpu.temperatureC = linuxCpuTemperature();
        }
#endif
        return any;
    }

    void platformReset(SamplerState& s) {
        s.ticker.clear();
#if defined(_WIN32)
        s.cpuTicks = CpuTicks{};
        s.gpu.reset();
#elif defined(__APPLE__)
        s.cpuTicks = DarwinCpu{};
#else
        s.cpuTicks = LinuxCpu{};
#endif
    }
}

const SystemSample& SystemStats::current() {
    SamplerState& s = state();
    const double now = steadySeconds();

    if (s.lastQuery >= 0.0 && (now - s.lastQuery) < s.interval) {
        return s.sample;
    }

    // The first read has no previous stamp; the configured interval stands in.
    const double elapsed = s.lastQuery < 0.0 ? s.interval : (now - s.lastQuery);
    s.lastQuery = now;

    // Sampled in place so the interface vector keeps its capacity across reads.
    const bool any = platformSample(s, s.sample, elapsed);
    if (any) {
        s.valid = true;
    }
    s.sample.valid = s.valid;
    s.sample.sampleSeconds = elapsed;
    ++s.revision;
    return s.sample;
}

void SystemStats::invalidate() {
    SamplerState& s = state();
    platformReset(s);
    s.lastQuery = -1.0;
    s.sample.network.interfaces.clear();
    s.sample.network.downBytesPerSec = 0.0;
    s.sample.network.upBytesPerSec = 0.0;
    s.sample.network.downBytesTotal = 0;
    s.sample.network.upBytesTotal = 0;
}

void SystemStats::setRefreshInterval(double seconds) {
    state().interval = clampDouble(seconds, 0.2, 5.0);
}

double SystemStats::getRefreshInterval() {
    return state().interval;
}

unsigned long long SystemStats::revision() {
    return state().revision;
}

}  // namespace quantum::editor
