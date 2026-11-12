#pragma once

#include <cstdint>

namespace wp::free_camera {

constexpr float kMinElevation = -0.2f;
constexpr float kMaxElevation = 1.3f;

struct Settings {
    bool enabled = true;
    float mouse = 1.0f;
    float stick = 1.0f;
    bool invert = false;
};

struct View {
    float yaw = 0.0f;
    float elevation = 0.0f;
    float distance = 0.0f;
};

struct Turn {
    float yaw = 0.0f;
    float pitch = 0.0f;
};

Settings current();
float wrap(float angle);
View from_offset(float x, float y, float z);
void to_offset(const View& view, float out[3]);
Turn turn(float mouse_x, float mouse_y, float stick_x, float stick_y, float seconds, const Settings& settings);

class Orbit {
public:
    void update(const float game_offset[3], const float target[3], Turn input, bool recenter, float seconds, float out[3]);
    void rest_on(float x, float y, float z);
    void reset();

private:
    bool seeded_ = false;
    bool recentering_ = false;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float game_elevation_ = 0.0f;
    float target_[3] = {};
};

void add_mouse(float x, float y);
void take_mouse(float& x, float& y);
void note_buttons(uint32_t buttons);
bool take_recenter();
void set_pointer_held(bool held);
bool pointer_held();
void note_driving();
bool driving();

}
