#include "wp/perf.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#endif

namespace wp::perf {

#ifdef _WIN32

namespace {

uint64_t ticks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct GpuQuery {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool tried = false;
    bool primed = false;
    double process = -1.0;
};

std::mutex g_gpu_mutex;
GpuQuery g_gpu;

std::wstring adapter_of(const wchar_t* name) {
    std::wstring text(name);
    size_t start = text.find(L"luid_");
    size_t end = text.find(L"_phys", start == std::wstring::npos ? 0 : start);
    return start == std::wstring::npos ? L"" : text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

}

double system_cpu_percent() {
    static std::mutex mutex;
    static uint64_t previous_idle = 0;
    static uint64_t previous_total = 0;
    std::lock_guard<std::mutex> lock(mutex);
    FILETIME idle_time;
    FILETIME kernel_time;
    FILETIME user_time;
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return -1.0;
    }
    uint64_t idle = ticks(idle_time);
    uint64_t total = ticks(kernel_time) + ticks(user_time);
    bool first = previous_total == 0;
    uint64_t idle_delta = idle - previous_idle;
    uint64_t total_delta = total - previous_total;
    previous_idle = idle;
    previous_total = total;
    if (first || total_delta == 0) {
        return -1.0;
    }
    return std::clamp(100.0 * static_cast<double>(total_delta - std::min(idle_delta, total_delta)) / static_cast<double>(total_delta), 0.0, 100.0);
}

double gpu_engine_percent() {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);
    if (!g_gpu.tried) {
        g_gpu.tried = true;
        if (PdhOpenQueryW(nullptr, 0, &g_gpu.query) != ERROR_SUCCESS) {
            g_gpu.query = nullptr;
        } else if (PdhAddEnglishCounterW(g_gpu.query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_gpu.counter) != ERROR_SUCCESS) {
            PdhCloseQuery(g_gpu.query);
            g_gpu.query = nullptr;
        }
    }
    g_gpu.process = -1.0;
    if (!g_gpu.query || PdhCollectQueryData(g_gpu.query) != ERROR_SUCCESS) {
        return -1.0;
    }
    if (!g_gpu.primed) {
        g_gpu.primed = true;
        return -1.0;
    }
    DWORD size = 0;
    DWORD count = 0;
    if (PdhGetFormattedCounterArrayW(g_gpu.counter, PDH_FMT_DOUBLE, &size, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA) || size == 0) {
        return -1.0;
    }
    std::vector<unsigned char> buffer(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(g_gpu.counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) {
        return -1.0;
    }
    std::wstring own = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::map<std::wstring, double> adapters;
    double process = 0.0;
    for (DWORD i = 0; i < count; i++) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
            continue;
        }
        double value = items[i].FmtValue.doubleValue;
        adapters[adapter_of(items[i].szName)] += value;
        if (std::wstring(items[i].szName).rfind(own, 0) == 0) {
            process += value;
        }
    }
    double busiest = 0.0;
    for (const auto& adapter : adapters) {
        busiest = std::max(busiest, adapter.second);
    }
    g_gpu.process = std::clamp(process, 0.0, 100.0);
    return std::clamp(busiest, 0.0, 100.0);
}

double gpu_process_percent() {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);
    return g_gpu.process;
}

void memory(double& used_gb, double& total_gb) {
    constexpr double kGigabyte = 1024.0 * 1024.0 * 1024.0;
    used_gb = -1.0;
    total_gb = -1.0;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        used_gb = static_cast<double>(counters.WorkingSetSize) / kGigabyte;
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        total_gb = static_cast<double>(status.ullTotalPhys) / kGigabyte;
    }
}

std::string cpu_name() {
    static const std::string name = [] {
        char buffer[256] = {};
        DWORD size = sizeof buffer;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, buffer, &size) !=
            ERROR_SUCCESS) {
            return std::string();
        }
        std::string text(buffer);
        size_t first = text.find_first_not_of(' ');
        size_t last = text.find_last_not_of(' ');
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    }();
    return name;
}

#else

double system_cpu_percent() {
    return -1.0;
}

double gpu_engine_percent() {
    return -1.0;
}

double gpu_process_percent() {
    return -1.0;
}

void memory(double& used_gb, double& total_gb) {
    used_gb = -1.0;
    total_gb = -1.0;
}

std::string cpu_name() {
    return "";
}

#endif

}
