#pragma once

#include <cstdint>

#include "wp/options.h"

namespace wp::touch {

enum class Phase { Down, Move, Up };

struct State {
    uint32_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool menu = false;
};

bool enabled();
void set_enabled(bool on);
void finger(int64_t id, Phase phase, float x, float y, int width, int height);
void other_input();
State poll(bool sideways);
bool take_menu_press();
bool take(uint64_t& version, options::Image& image, bool& visible, int width, int height);

}
