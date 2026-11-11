#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wp/options.h"
#include "wp/perf.h"

namespace wp::perf {

enum class Level { Minimal, Standard, Full };

enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };

struct Cell {
    std::string text;
    uint32_t color = 0xFFFFFF;
};

using Line = std::vector<Cell>;

Level level_from(const std::string& value);
Corner corner_from(const std::string& value);
std::vector<Line> lines(const Sample& sample, Level level);
options::Image render(const std::vector<Line>& lines, int client_height);
options::Rect corner_rect(int client_width, int client_height, int width, int height, Corner corner);
void show(options::Image image, Corner corner);
void hide();
bool take(uint64_t& version, options::Image& image, bool& visible, Corner& corner);
void update();

}
