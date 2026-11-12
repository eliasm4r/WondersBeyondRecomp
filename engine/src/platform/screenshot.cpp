#include "wp/screenshot.h"

#include <algorithm>
#include <cstdio>

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

}
