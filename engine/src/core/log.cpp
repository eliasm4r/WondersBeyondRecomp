#include "wp/fast_clock.h"
#include "wp/log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace wp::log {

namespace {

using Clock = wp::FastClock;

constexpr size_t kMaxPending = 8u << 20;

struct LogFile {
    std::FILE* file = nullptr;
    Clock::time_point origin = Clock::now();
    std::mutex mutex;
    std::mutex output;
    std::condition_variable wake;
    std::string pending;
    unsigned dropped = 0;

    LogFile() {
        if (const char* path = std::getenv("WP_LOG_FILE")) {
            if (std::FILE* emptied = std::fopen(path, "w")) {
                std::fclose(emptied);
            }
            file = std::fopen(path, "a");
        }
        if (file) {
            std::thread([this] { run(); }).detach();
        }
    }

    void take(std::string& text) {
        text.swap(pending);
        if (dropped) {
            char note[96];
            std::snprintf(note, sizeof note, "[%10s] %-7s %u lines dropped while the disk was busy\n", "", "log", dropped);
            text += note;
            dropped = 0;
        }
    }

    void put(const std::string& text) {
        if (!text.empty()) {
            std::lock_guard<std::mutex> lock(output);
            std::fwrite(text.data(), 1, text.size(), file);
            std::fflush(file);
        }
    }

    void run() {
        std::string text;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return !pending.empty(); });
                take(text);
            }
            put(text);
            text.clear();
        }
    }
};

LogFile& instance() {
    static LogFile* log_file = [] {
        LogFile* created = new LogFile;
        std::atexit(flush);
        return created;
    }();
    return *log_file;
}

}

bool enabled() {
    return instance().file != nullptr;
}

void write(const char* category, const char* format, ...) {
    LogFile& log_file = instance();
    if (!log_file.file) {
        return;
    }
    double seconds = std::chrono::duration<double>(Clock::now() - log_file.origin).count();
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    char line[600];
    int length = std::snprintf(line, sizeof line, "[%10.3f] %-7s %s\n", seconds, category, message);
    if (length < 0) {
        return;
    }
    size_t size = std::min(static_cast<size_t>(length), sizeof line - 1);
    {
        std::lock_guard<std::mutex> lock(log_file.mutex);
        if (log_file.pending.size() + size > kMaxPending) {
            log_file.dropped++;
            return;
        }
        log_file.pending.append(line, size);
    }
    log_file.wake.notify_one();
}

void flush() {
    LogFile& log_file = instance();
    if (!log_file.file) {
        return;
    }
    std::string text;
    std::unique_lock<std::mutex> lock(log_file.mutex, std::defer_lock);
    for (int attempt = 0; attempt < 200 && !lock.try_lock(); attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!lock.owns_lock()) {
        return;
    }
    log_file.take(text);
    lock.unlock();
    std::unique_lock<std::mutex> output(log_file.output, std::defer_lock);
    for (int attempt = 0; attempt < 200 && !output.try_lock(); attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (output.owns_lock() && !text.empty()) {
        std::fwrite(text.data(), 1, text.size(), log_file.file);
        std::fflush(log_file.file);
    }
}

}
