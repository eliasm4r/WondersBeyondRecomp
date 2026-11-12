#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace wp::screenshot {

constexpr const char* kFolder = "screenshots";

constexpr uint32_t kRecentWidth = 1400;
constexpr size_t kRecentImages = 360;
constexpr uint64_t kRecentBytes = 1024ull << 20;

std::string file_name(const char* prefix, const std::tm& time, int attempt);
uint32_t recent_level(uint32_t width);
size_t recent_count(uint32_t width, uint32_t height);
std::string recent_folder(const char* prefix, const std::tm& time);
std::string recent_name(size_t index, bool between);

}
