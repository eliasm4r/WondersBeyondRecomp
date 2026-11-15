#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wp/options.h"

namespace wp::hint {

void show(const char* label_key, double progress);
void hold(const char* label_key, double seconds, double elapsed);
void busy(const char* label_key);
void hide();
void note(const std::vector<std::string>& lines, double progress);
void clear_note();
bool take(uint64_t& version, options::Image& image, bool& visible);
options::Rect place(int client_width, int client_height, int width, int height);

}
