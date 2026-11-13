#include "wp/fast_clock.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace wp {

#ifdef _WIN32
namespace {

constexpr int64_t kCalibrationNanoseconds = 1000000000;

struct Origin {
    int64_t frequency;
    int64_t counter;
    bool invariant_tsc;
};

const Origin& origin() {
    static const Origin value = [] {
        LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        unsigned a = 0, b = 0, c = 0, d = 0;
        bool invariant = __get_cpuid(0x80000007, &a, &b, &c, &d) && (d & (1u << 8));
        return Origin{frequency.QuadPart, counter.QuadPart, invariant};
    }();
    return value;
}

std::atomic<double> g_ns_per_tsc{0.0};
std::atomic<bool> g_switching{false};
int64_t g_switch_ns = 0;
uint64_t g_switch_tsc = 0;
int64_t g_start_ns = 0;
uint64_t g_start_tsc = 0;
std::atomic<bool> g_started{false};

int64_t counter_ns(const Origin& o) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t ticks = now.QuadPart - o.counter;
    return ticks / o.frequency * 1000000000 + ticks % o.frequency * 1000000000 / o.frequency;
}

}

FastClock::time_point FastClock::now() noexcept {
    double ns_per_tsc = g_ns_per_tsc.load(std::memory_order_acquire);
    if (ns_per_tsc != 0.0) {
        return time_point(duration(g_switch_ns + static_cast<int64_t>(static_cast<double>(__rdtsc() - g_switch_tsc) * ns_per_tsc)));
    }
    const Origin& o = origin();
    int64_t ns = counter_ns(o);
    if (o.invariant_tsc) {
        uint64_t tsc = __rdtsc();
        if (!g_started.load(std::memory_order_acquire)) {
            bool expected = false;
            if (g_switching.compare_exchange_strong(expected, true)) {
                g_start_ns = ns;
                g_start_tsc = tsc;
                g_started.store(true, std::memory_order_release);
                g_switching.store(false);
            }
        } else if (ns - g_start_ns >= kCalibrationNanoseconds && tsc > g_start_tsc) {
            bool expected = false;
            if (g_switching.compare_exchange_strong(expected, true)) {
                g_switch_ns = ns;
                g_switch_tsc = tsc;
                g_ns_per_tsc.store(static_cast<double>(ns - g_start_ns) / static_cast<double>(tsc - g_start_tsc), std::memory_order_release);
            }
        }
    }
    return time_point(duration(ns));
}
#else
FastClock::time_point FastClock::now() noexcept {
    return time_point(std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch()));
}
#endif

}
