#include "wp/screenshot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#include "wp/color.h"
#include "wp/game.h"
#include "wp/log.h"
#include "wp/video.h"

namespace wp::screenshot {

namespace {

std::string stamped(const char* prefix, const std::tm& time) {
    char stamp[96];
    std::snprintf(stamp, sizeof stamp, "_%04d-%02d-%02d_%02d-%02d-%02d", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min,
                  time.tm_sec);
    return std::string(prefix) + stamp;
}

}

uint32_t recent_level(uint32_t width) {
    uint32_t level = 0;
    while ((width >> level) > kRecentWidth) {
        level++;
    }
    return level;
}

size_t recent_count(uint32_t width, uint32_t height) {
    uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
    if (bytes == 0) {
        return 0;
    }
    return static_cast<size_t>(std::min<uint64_t>(kRecentImages, kRecentBytes / bytes));
}

std::string recent_folder(const char* prefix, const std::tm& time) {
    return stamped(prefix, time) + "_recent";
}

std::string recent_name(size_t index, bool between) {
    char name[32];
    std::snprintf(name, sizeof name, between ? "%03u_between.png" : "%03u.png", static_cast<unsigned>(index));
    return name;
}

std::string file_name(const char* prefix, const std::tm& time, int attempt) {
    std::string name = stamped(prefix, time);
    if (attempt > 1) {
        name += "_" + std::to_string(attempt);
    }
    return name + ".png";
}

namespace {

constexpr auto kPhotoLifetime = std::chrono::minutes(2);

std::mutex g_photo_mutex;
std::vector<uint32_t> g_photo;
uint32_t g_photo_width = 0;
uint32_t g_photo_height = 0;
std::chrono::steady_clock::time_point g_photo_time;

}

void photo_taken(std::vector<uint32_t> pixels, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_photo_mutex);
    g_photo = std::move(pixels);
    g_photo_width = width;
    g_photo_height = height;
    g_photo_time = std::chrono::steady_clock::now();
}

bool photo_pending() {
    std::lock_guard<std::mutex> lock(g_photo_mutex);
    return !g_photo.empty() && std::chrono::steady_clock::now() - g_photo_time < kPhotoLifetime;
}

std::string photo_stored() {
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    {
        std::lock_guard<std::mutex> lock(g_photo_mutex);
        bool fresh = std::chrono::steady_clock::now() - g_photo_time < kPhotoLifetime;
        pixels.swap(g_photo);
        width = g_photo_width;
        height = g_photo_height;
        if (!fresh) {
            pixels.clear();
        }
    }
    if (pixels.empty()) {
        return {};
    }
    std::time_t now = std::time(nullptr);
    std::tm time{};
#ifdef _WIN32
    localtime_s(&time, &now);
#else
    localtime_r(&now, &time);
#endif
    std::error_code error;
    std::filesystem::create_directories(kPhotoFolder, error);
    std::string path;
    for (int attempt = 1; attempt < 1000; attempt++) {
        path = std::string(kPhotoFolder) + "/" + file_name(game::description().short_name, time, attempt);
        if (!std::filesystem::exists(path, error)) {
            break;
        }
    }
    std::thread([path, pixels = std::move(pixels), width, height]() mutable {
        color::apply(pixels, color::current());
        video::save_png(path.c_str(), pixels, width, height);
        log::write("photo", "%ux%u saved to %s", width, height, path.c_str());
    }).detach();
    return path;
}

}
