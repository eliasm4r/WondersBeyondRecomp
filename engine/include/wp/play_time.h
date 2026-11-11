#pragma once

#include <cstdint>
#include <string>

#include "wp/ui_text.h"

namespace wp::play_time {

uint64_t read_file(const std::string& path);
bool write_file(const std::string& path, uint64_t seconds);
std::string stats_path(const std::string& settings_file);
void start(const std::string& path);
uint64_t seconds();
void save();
std::string format(uint64_t seconds, ui::Language language);

}
