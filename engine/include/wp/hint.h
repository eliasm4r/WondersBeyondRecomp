#pragma once

#include <cstdint>

#include "wp/options.h"

namespace wp::hint {

void show(const char* label_key, double progress);
void hide();
bool take(uint64_t& version, options::Image& image, bool& visible);
options::Rect place(int client_width, int client_height, int width, int height);

}
