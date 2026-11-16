#include "wp/performance_hints.h"

#ifdef __ANDROID__

#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

#include "wp/log.h"

namespace wp::performance_hints {

namespace {

struct Session;

constexpr int64_t kWorkTargetNs = 14000000;

using GetManager = void* (*)();
using CreateSession = Session* (*)(void*, const int32_t*, size_t, int64_t);
using ReportWork = int (*)(Session*, int64_t);

Session* g_session = nullptr;
ReportWork g_report = nullptr;
cpu_set_t g_fast_cores;
bool g_have_fast_cores = false;

void keep_to_fast_cores() {
    long speeds[CPU_SETSIZE] = {};
    long top = 0;
    int count = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    count = count > CPU_SETSIZE ? CPU_SETSIZE : count;
    for (int i = 0; i < count; i++) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (FILE* file = std::fopen(path, "r")) {
            if (std::fscanf(file, "%ld", &speeds[i]) != 1) {
                speeds[i] = 0;
            }
            std::fclose(file);
        }
        top = speeds[i] > top ? speeds[i] : top;
    }
    if (top == 0) {
        return;
    }
    CPU_ZERO(&g_fast_cores);
    int fast = 0;
    for (int i = 0; i < count; i++) {
        if (speeds[i] == top) {
            CPU_SET(i, &g_fast_cores);
            fast++;
        }
    }
    g_have_fast_cores = true;
    if (sched_setaffinity(0, sizeof g_fast_cores, &g_fast_cores) == 0) {
        log::write("threads", "game thread kept on the %d fastest cores (%ld MHz)", fast, top / 1000);
    }
}

void start_session() {
    void* library = dlopen("libandroid.so", RTLD_NOW);
    if (!library) {
        return;
    }
    auto get_manager = reinterpret_cast<GetManager>(dlsym(library, "APerformanceHint_getManager"));
    auto create_session = reinterpret_cast<CreateSession>(dlsym(library, "APerformanceHint_createSession"));
    g_report = reinterpret_cast<ReportWork>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
    void* manager = get_manager ? get_manager() : nullptr;
    if (!manager || !create_session || !g_report) {
        log::write("threads", "performance hints unavailable on this Android");
        return;
    }
    int32_t thread = static_cast<int32_t>(gettid());
    g_session = create_session(manager, &thread, 1, kWorkTargetNs);
    log::write("threads", g_session ? "performance hints on" : "performance hints refused");
}

}

void retrace_done(int64_t idle_nanoseconds) {
    using Clock = std::chrono::steady_clock;
    static bool started = false;
    static Clock::time_point last;
    static int64_t last_idle = 0;
    static unsigned retraces = 0;
    Clock::time_point now = Clock::now();
    if (!started) {
        started = true;
        keep_to_fast_cores();
        start_session();
    } else {
        if (g_have_fast_cores && ++retraces % 30 == 0) {
            cpu_set_t current;
            if (sched_getaffinity(0, sizeof current, &current) == 0 && !CPU_EQUAL(&current, &g_fast_cores)) {
                sched_setaffinity(0, sizeof g_fast_cores, &g_fast_cores);
            }
        }
        if (g_session) {
            int64_t work = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count() - (idle_nanoseconds - last_idle);
            if (work > 0) {
                g_report(g_session, work);
            }
        }
    }
    last = now;
    last_idle = idle_nanoseconds;
}

}

#else

namespace wp::performance_hints {

void retrace_done(int64_t) {}

}

#endif
