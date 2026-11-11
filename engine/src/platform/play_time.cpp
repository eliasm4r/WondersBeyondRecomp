#include "wp/play_time.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace wp::play_time {

namespace {

constexpr const char* kKey = "play_time_seconds=";

std::mutex g_mutex;
std::string g_path;
uint64_t g_stored = 0;
std::chrono::steady_clock::time_point g_started;
std::atomic<bool> g_running{false};

}

uint64_t read_file(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind(kKey, 0) != 0) {
            continue;
        }
        std::string value = line.substr(std::char_traits<char>::length(kKey));
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            return 0;
        }
        errno = 0;
        unsigned long long parsed = std::strtoull(value.c_str(), nullptr, 10);
        return errno == 0 ? parsed : 0;
    }
    return 0;
}

bool write_file(const std::string& path, uint64_t seconds) {
    std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) {
            return false;
        }
        file << kKey << seconds << "\n";
        if (!file.flush()) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::string stats_path(const std::string& settings_file) {
    std::filesystem::path directory = std::filesystem::path(settings_file).parent_path();
    return (directory / "stats.ini").string();
}

void start(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = path;
        g_stored = read_file(path);
        g_started = std::chrono::steady_clock::now();
    }
    if (!g_running.exchange(true)) {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                save();
            }
        }).detach();
    }
}

uint64_t seconds() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) {
        return 0;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_started).count();
    return g_stored + static_cast<uint64_t>(elapsed);
}

void save() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        path = g_path;
    }
    if (!path.empty()) {
        write_file(path, seconds());
    }
}

std::string format(uint64_t seconds, ui::Language language) {
    unsigned long long hours = seconds / 3600;
    unsigned long long minutes = seconds / 60 % 60;
    char buffer[48];
    if (language == ui::Language::English) {
        std::snprintf(buffer, sizeof buffer, "%lluh %02llum", hours, minutes);
    } else {
        std::snprintf(buffer, sizeof buffer, "%llu h %02llu min", hours, minutes);
    }
    return buffer;
}

}
