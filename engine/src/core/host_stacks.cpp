#include "wp/host_stacks.h"

#ifndef _WIN32
#include <execinfo.h>
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

void write_stack(int) {
    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
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
