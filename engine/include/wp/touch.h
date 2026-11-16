#pragma once

#include <cstdint>

#include "wp/options.h"

namespace wp::touch {

enum class Phase { Down, Move, Up };

struct State {
    uint32_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool pointer_valid = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    int width = 0;
    int height = 0;
};

bool enabled();
void set_enabled(bool on);
bool in_use();
void set_insets(int left, int top, int right, int bottom);
void finger(int64_t id, Phase phase, float x, float y, int width, int height);
void other_input();
void phone_motion(float x, float y, float z);
State poll(bool sideways);
bool take_menu_press();
bool take(uint64_t& version, options::Image& image, options::Image& knob, bool& visible, int width, int height);
options::Rect knob_rect(int height);

}
