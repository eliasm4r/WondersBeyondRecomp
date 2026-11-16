#include "wp/touch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "wp/attraction.h"
#include "wp/canvas.h"
#include "wp/free_camera.h"
#include "wp/input.h"
#include "wp/settings.h"

namespace wp::touch {

namespace {

using Clock = std::chrono::steady_clock;

enum class Kind { Button, Menu };
enum class Corner { TopLeft, TopRight, BottomRight };

struct Place {
    Kind kind;
    const char* label;
    uint32_t button;
    Corner corner;
    float x;
    float y;
    float radius;
};

struct Spot {
    Kind kind;
    const char* label;
    uint32_t button;
    float x;
    float y;
    float radius;
};

struct Finger {
    int64_t id = 0;
    int spot = -1;
    bool stick = false;
    bool look = false;
    float x = 0.0f;
    float y = 0.0f;
    float start_x = 0.0f;
    float start_y = 0.0f;
    Clock::time_point start;
};

constexpr int kNone = -1;
constexpr float kStickZone = 0.45f;
constexpr float kStickRadius = 0.13f;
constexpr float kStickDead = 0.3f;
constexpr float kGrab = 1.3f;
constexpr float kLetGo = 1.8f;
constexpr float kImageScale = 0.5f;
constexpr float kLookCounts = 1900.0f;
constexpr float kTapTravel = 0.04f;
constexpr auto kTapTime = std::chrono::milliseconds(300);
constexpr auto kFireTime = std::chrono::milliseconds(150);
constexpr auto kShakeTime = std::chrono::milliseconds(250);
constexpr float kShakeForce = 1.1f;
constexpr uint32_t kFace = 0x46FFFFFF;
constexpr uint32_t kFacePressed = 0x96FFFFFF;
constexpr uint32_t kLabel = 0xE6FFFFFF;
constexpr uint32_t kBase = 0x30FFFFFF;
constexpr uint32_t kKnob = 0x78FFFFFF;
constexpr float kKnobShare = 0.45f;
constexpr float kNarrowest = 1.3f;
constexpr float kLargestDp = 430.0f;

constexpr Place kPlaces[] = {
    {Kind::Menu, "Menu", 0, Corner::TopLeft, 0.09f, 0.08f, 0.055f},
    {Kind::Button, "+", input::kButtonPlus, Corner::TopRight, 0.08f, 0.08f, 0.05f},
    {Kind::Button, "-", input::kButtonMinus, Corner::TopRight, 0.21f, 0.08f, 0.05f},
    {Kind::Button, "2", input::kButtonTwo, Corner::BottomRight, 0.15f, 0.17f, 0.11f},
    {Kind::Button, "1", input::kButtonOne, Corner::BottomRight, 0.39f, 0.12f, 0.09f},
    {Kind::Button, "A", input::kButtonA, Corner::BottomRight, 0.19f, 0.44f, 0.09f},
    {Kind::Button, "B", input::kButtonB, Corner::BottomRight, 0.42f, 0.36f, 0.075f},
    {Kind::Button, "~", input::kMotionShake, Corner::BottomRight, 0.10f, 0.66f, 0.065f},
};

const settings::LiveFlag g_wanted("input.touch", nullptr);
const settings::LiveFlag g_phone_shake("input.phone_shake", nullptr);
const settings::LiveNumber g_size("input.touch_size", nullptr);
const settings::LiveNumber g_opacity("input.touch_opacity", nullptr);
const settings::LiveNumber g_touch_speed("camera.touch_speed", nullptr);
const settings::LiveNumber g_mouse_speed("camera.mouse_speed", nullptr);

std::mutex g_lock;
bool g_enabled = [] {
#ifdef __ANDROID__
    return true;
#else
    const char* wanted = std::getenv("WP_TOUCH");
    return wanted && *wanted && *wanted != '0';
#endif
}();
bool g_hidden = false;
bool g_menu_pressed = false;
int g_width = 0;
int g_height = 0;
int g_inset_left = 0;
int g_inset_top = 0;
int g_inset_right = 0;
int g_inset_bottom = 0;
float g_density = 0.0f;
int g_drawn_size = 0;
int g_drawn_opacity = 0;
std::vector<Finger> g_fingers;
uint32_t g_held = 0;
bool g_stick_active = false;
float g_stick_origin_x = 0.0f;
float g_stick_origin_y = 0.0f;
float g_stick_x = 0.0f;
float g_stick_y = 0.0f;
bool g_pointer_set = false;
float g_pointer_x = 0.0f;
float g_pointer_y = 0.0f;
Clock::time_point g_fire_until;
Clock::time_point g_shake_until;
uint64_t g_version = 1;

bool active() {
    return g_enabled && g_wanted();
}

float size_factor() {
    return std::clamp(g_size(), 50, 200) / 100.0f;
}

float unit(int height) {
    float u = static_cast<float>(height);
    if (g_width > 0) {
        u = std::min(u, static_cast<float>(g_width) / kNarrowest);
    }
    if (g_density > 0.0f) {
        u = std::min(u, kLargestDp * g_density);
    }
    return u * size_factor();
}

bool aiming() {
    return attraction::mode() == attraction::Mode::Aim;
}

std::vector<Spot> spots(int width, int height) {
    float u = unit(height);
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);
    std::vector<Spot> result;
    for (const Place& place : kPlaces) {
        float x = place.corner == Corner::TopLeft ? g_inset_left + place.x * u : w - g_inset_right - place.x * u;
        float y = place.corner == Corner::BottomRight ? h - g_inset_bottom - place.y * u : g_inset_top + place.y * u;
        result.push_back({place.kind, place.label, place.button, x, y, place.radius * u});
    }
    return result;
}

float stick_radius(int height) {
    return kStickRadius * unit(height);
}

float rest_x(int height) {
    return g_inset_left + 0.24f * unit(height);
}

float rest_y(int height) {
    return static_cast<float>(height) - g_inset_bottom - 0.26f * unit(height);
}

int nearest_spot(const std::vector<Spot>& all, float x, float y) {
    int best = kNone;
    float best_distance = 0.0f;
    for (size_t i = 0; i < all.size(); i++) {
        float distance = std::hypot(x - all[i].x, y - all[i].y);
        if (distance <= all[i].radius * kGrab && (best == kNone || distance < best_distance)) {
            best = static_cast<int>(i);
            best_distance = distance;
        }
    }
    return best;
}

void update_held(const std::vector<Spot>& all) {
    uint32_t held = 0;
    for (const Finger& finger : g_fingers) {
        if (finger.spot >= 0 && all[static_cast<size_t>(finger.spot)].kind == Kind::Button) {
            held |= all[static_cast<size_t>(finger.spot)].button;
        }
    }
    if (held != g_held) {
        g_held = held;
        g_version++;
    }
}

void move_stick(float x, float y) {
    float radius = stick_radius(g_height);
    float dx = (x - g_stick_origin_x) / radius;
    float dy = (y - g_stick_origin_y) / radius;
    float length = std::hypot(dx, dy);
    if (length > 1.0f) {
        dx /= length;
        dy /= length;
    }
    g_stick_x = dx;
    g_stick_y = dy;
}

void move_look(Finger& finger, float x, float y) {
    if (aiming()) {
        g_pointer_set = true;
        g_pointer_x = x;
        g_pointer_y = y;
    } else if (g_height > 0) {
        float scale = kLookCounts / static_cast<float>(g_height) * static_cast<float>(std::max(1, g_touch_speed())) /
                      static_cast<float>(std::max(1, g_mouse_speed()));
        free_camera::add_mouse((x - finger.x) * scale, (y - finger.y) * scale);
    }
    finger.x = x;
    finger.y = y;
}

void end_look(const Finger& finger) {
    if (!aiming()) {
        return;
    }
    float travel = std::hypot(finger.x - finger.start_x, finger.y - finger.start_y);
    if (Clock::now() - finger.start < kTapTime && travel < kTapTravel * static_cast<float>(g_height)) {
        g_fire_until = Clock::now() + kFireTime;
    }
}

uint32_t faded(uint32_t color) {
    uint32_t alpha = (color >> 24) * static_cast<uint32_t>(std::clamp(g_opacity(), 10, 100)) / 100;
    return (alpha << 24) | (color & 0xFFFFFF);
}

void draw_circle(canvas::Canvas& image, float x, float y, float radius, uint32_t color) {
    int left = static_cast<int>(std::lround(x - radius));
    int top = static_cast<int>(std::lround(y - radius));
    int size = static_cast<int>(std::lround(radius * 2.0f));
    image.fill_round(left, top, left + size, top + size, size / 2, faded(color));
}

options::Image render(int width, int height) {
    options::Image result;
    int image_width = std::max(1, static_cast<int>(width * kImageScale));
    int image_height = std::max(1, static_cast<int>(height * kImageScale));
    canvas::Canvas image;
    image.width = image_width;
    image.height = image_height;
    image.pixels.assign(static_cast<size_t>(image_width) * image_height, 0);
    std::vector<Spot> all = spots(width, height);
    bool fonts = canvas::font_ok(canvas::FontId::Heavy);
    for (size_t i = 0; i < all.size(); i++) {
        const Spot& spot = all[i];
        bool pressed = spot.kind == Kind::Button && (g_held & spot.button) != 0;
        float x = spot.x * kImageScale;
        float y = spot.y * kImageScale;
        float radius = spot.radius * kImageScale;
        draw_circle(image, x, y, radius, pressed ? kFacePressed : kFace);
        if (fonts) {
            int size = static_cast<int>(radius * (std::string(spot.label).size() > 1 ? 0.55f : 1.1f));
            canvas::Face face = canvas::make_face(canvas::FontId::Heavy, std::max(8, size));
            image.text(face, faded(kLabel), spot.label, static_cast<int>(x - radius), static_cast<int>(y - radius), static_cast<int>(x + radius),
                       static_cast<int>(y + radius), canvas::Align::Center, false);
        }
    }
    float base_x = (g_stick_active ? g_stick_origin_x : rest_x(height)) * kImageScale;
    float base_y = (g_stick_active ? g_stick_origin_y : rest_y(height)) * kImageScale;
    float radius = stick_radius(height) * kImageScale;
    draw_circle(image, base_x, base_y, radius, kBase);
    result.pixels = std::move(image.pixels);
    result.width = static_cast<uint32_t>(image_width);
    result.height = static_cast<uint32_t>(image_height);
    return result;
}

options::Image render_knob(int height) {
    options::Image result;
    float radius = stick_radius(height) * kImageScale * kKnobShare;
    int size = std::max(2, static_cast<int>(std::lround(radius * 2.0f)) + 2);
    canvas::Canvas image;
    image.width = size;
    image.height = size;
    image.pixels.assign(static_cast<size_t>(size) * size, 0);
    draw_circle(image, size * 0.5f, size * 0.5f, radius, kKnob);
    result.pixels = std::move(image.pixels);
    result.width = static_cast<uint32_t>(size);
    result.height = static_cast<uint32_t>(size);
    return result;
}

}

bool enabled() {
    std::lock_guard<std::mutex> hold(g_lock);
    return active();
}

void set_enabled(bool on) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_enabled != on) {
        g_enabled = on;
        g_version++;
    }
}

bool in_use() {
    std::lock_guard<std::mutex> hold(g_lock);
    return active() && !g_hidden;
}

void set_density(float pixels_per_dp) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (pixels_per_dp != g_density) {
        g_density = pixels_per_dp;
        g_version++;
    }
}

void set_insets(int left, int top, int right, int bottom) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (left != g_inset_left || top != g_inset_top || right != g_inset_right || bottom != g_inset_bottom) {
        g_inset_left = std::max(0, left);
        g_inset_top = std::max(0, top);
        g_inset_right = std::max(0, right);
        g_inset_bottom = std::max(0, bottom);
        g_version++;
    }
}

void finger(int64_t id, Phase phase, float x, float y, int width, int height) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!active() || width <= 0 || height <= 0) {
        return;
    }
    if (width != g_width || height != g_height) {
        g_width = width;
        g_height = height;
        g_version++;
    }
    std::vector<Spot> all = spots(width, height);
    auto found = std::find_if(g_fingers.begin(), g_fingers.end(), [id](const Finger& finger) { return finger.id == id; });
    if (phase == Phase::Down) {
        if (g_hidden) {
            g_hidden = false;
            g_version++;
        }
        if (found != g_fingers.end()) {
            g_fingers.erase(found);
        }
        Finger added;
        added.id = id;
        added.x = added.start_x = x;
        added.y = added.start_y = y;
        added.start = Clock::now();
        added.spot = nearest_spot(all, x, y);
        if (added.spot >= 0 && all[static_cast<size_t>(added.spot)].kind == Kind::Menu) {
            g_menu_pressed = true;
            added.spot = kNone;
        } else if (added.spot == kNone && x < kStickZone * width && !g_stick_active) {
            added.stick = true;
            g_stick_active = true;
            g_stick_origin_x = x;
            g_stick_origin_y = y;
            g_stick_x = 0.0f;
            g_stick_y = 0.0f;
            g_version++;
        } else if (added.spot == kNone) {
            added.look = true;
            if (aiming()) {
                g_pointer_set = true;
                g_pointer_x = x;
                g_pointer_y = y;
            }
        }
        g_fingers.push_back(added);
        update_held(all);
        return;
    }
    if (found == g_fingers.end()) {
        return;
    }
    if (phase == Phase::Move) {
        if (found->stick) {
            move_stick(x, y);
        } else if (found->look) {
            move_look(*found, x, y);
        } else if (found->spot >= 0) {
            const Spot& spot = all[static_cast<size_t>(found->spot)];
            if (std::hypot(x - spot.x, y - spot.y) > spot.radius * kLetGo) {
                found->spot = nearest_spot(all, x, y);
                if (found->spot >= 0 && all[static_cast<size_t>(found->spot)].kind != Kind::Button) {
                    found->spot = kNone;
                }
            }
        } else {
            int spot = nearest_spot(all, x, y);
            if (spot >= 0 && all[static_cast<size_t>(spot)].kind == Kind::Button) {
                found->spot = spot;
            }
        }
        update_held(all);
        return;
    }
    if (found->stick) {
        g_stick_active = false;
        g_stick_x = 0.0f;
        g_stick_y = 0.0f;
        g_version++;
    }
    if (found->look) {
        move_look(*found, x, y);
        end_look(*found);
    }
    g_fingers.erase(found);
    update_held(all);
}

void other_input() {
    std::lock_guard<std::mutex> hold(g_lock);
    if (active() && !g_hidden && g_fingers.empty()) {
        g_hidden = true;
        g_version++;
    }
}

void phone_motion(float x, float y, float z) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!active() || !g_phone_shake()) {
        return;
    }
    if (std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f) > kShakeForce) {
        g_shake_until = Clock::now() + kShakeTime;
    }
}

State poll(bool sideways) {
    std::lock_guard<std::mutex> hold(g_lock);
    State state;
    if (!active()) {
        return state;
    }
    Clock::time_point now = Clock::now();
    state.buttons = g_held;
    if (now < g_shake_until) {
        state.buttons |= input::kMotionShake;
    }
    state.stick_x = g_stick_x;
    state.stick_y = g_stick_y;
    bool left = g_stick_x < -kStickDead;
    bool right = g_stick_x > kStickDead;
    bool up = g_stick_y < -kStickDead;
    bool down = g_stick_y > kStickDead;
    if (sideways) {
        state.buttons |= (left ? input::kButtonUp : 0) | (right ? input::kButtonDown : 0) | (up ? input::kButtonRight : 0) | (down ? input::kButtonLeft : 0);
    } else {
        state.buttons |= (left ? input::kButtonLeft : 0) | (right ? input::kButtonRight : 0) | (up ? input::kButtonUp : 0) | (down ? input::kButtonDown : 0);
    }
    attraction::Mode mode = attraction::mode();
    if (mode == attraction::Mode::Aim) {
        if (now < g_fire_until) {
            state.buttons |= input::kButtonA;
        }
        state.pointer_valid = g_pointer_set;
        state.pointer_x = g_pointer_x;
        state.pointer_y = g_pointer_y;
        state.width = g_width;
        state.height = g_height;
    } else {
        g_pointer_set = false;
    }
    if (mode == attraction::Mode::Shake && (state.buttons & input::kButtonA)) {
        state.buttons = (state.buttons & ~input::kButtonA) | input::kMotionShakeLeft;
    }
    return state;
}

bool take_menu_press() {
    std::lock_guard<std::mutex> hold(g_lock);
    bool pressed = g_menu_pressed;
    g_menu_pressed = false;
    return pressed;
}

bool take(uint64_t& version, options::Image& image, options::Image& knob, bool& visible, int width, int height) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (width != g_width || height != g_height) {
        g_width = width;
        g_height = height;
        g_version++;
    }
    int size = g_size();
    int opacity = g_opacity();
    if (size != g_drawn_size || opacity != g_drawn_opacity) {
        g_drawn_size = size;
        g_drawn_opacity = opacity;
        g_version++;
    }
    bool show = active() && !g_hidden && width > 0 && height > 0;
    if (version == g_version && visible == show) {
        return false;
    }
    version = g_version;
    visible = show;
    if (show) {
        image = render(width, height);
        knob = render_knob(height);
    }
    return true;
}

options::Rect knob_rect(int height) {
    std::lock_guard<std::mutex> hold(g_lock);
    float base_x = g_stick_active ? g_stick_origin_x : rest_x(height);
    float base_y = g_stick_active ? g_stick_origin_y : rest_y(height);
    float radius = stick_radius(height);
    float half = radius * kKnobShare + 1.0f / kImageScale;
    float x = base_x + g_stick_x * radius;
    float y = base_y + g_stick_y * radius;
    return options::Rect{static_cast<int>(std::lround(x - half)), static_cast<int>(std::lround(y - half)), static_cast<int>(std::lround(half * 2.0f)),
                         static_cast<int>(std::lround(half * 2.0f))};
}

}
