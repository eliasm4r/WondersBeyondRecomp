#include "wp/host_stacks.h"

#ifndef _WIN32
#ifdef __ANDROID__
#include <unwind.h>

#include <cinttypes>
#else
#include <execinfo.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#endif

namespace wp::host_stacks {

#ifdef _WIN32

void watch_current_thread(const char*) {}

void print() {}

#else

namespace {

struct Watched {
    pthread_t thread;
    const char* name;
};

std::mutex g_lock;
std::vector<Watched> g_threads;
std::atomic<bool> g_written{false};

#ifdef __ANDROID__
struct Frames {
    void** frames;
    int count;
    int limit;
};

_Unwind_Reason_Code add_frame(_Unwind_Context* context, void* data) {
    Frames& found = *static_cast<Frames*>(data);
    uintptr_t address = _Unwind_GetIP(context);
    if (address && found.count < found.limit) {
        found.frames[found.count++] = reinterpret_cast<void*>(address);
    }
    return found.count < found.limit ? _URC_NO_REASON : _URC_END_OF_STACK;
}
#endif

void write_stack(int) {
    void* frames[64];
#ifdef __ANDROID__
    Frames found{frames, 0, 64};
    _Unwind_Backtrace(add_frame, &found);
    for (int i = 0; i < found.count; i++) {
        std::fprintf(stderr, "  %p\n", frames[i]);
    }
#else
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
    g_written.store(true);
}

}

void watch_current_thread(const char* name) {
    static std::once_flag installed;
    std::call_once(installed, [] {
        struct sigaction action {};
        action.sa_handler = write_stack;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGUSR2, &action, nullptr);
    });
    std::lock_guard<std::mutex> hold(g_lock);
    g_threads.push_back({pthread_self(), name});
}

void print() {
    std::vector<Watched> threads;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        threads = g_threads;
    }
    for (const Watched& watched : threads) {
        std::fprintf(stderr, "host thread %s:\n", watched.name);
        g_written.store(false);
        if (pthread_kill(watched.thread, SIGUSR2) != 0) {
            continue;
        }
        for (int i = 0; i < 200 && !g_written.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

#endif

}
