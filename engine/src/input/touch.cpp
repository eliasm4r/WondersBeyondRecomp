#include "wp/touch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "wp/canvas.h"
#include "wp/input.h"

namespace wp::touch {

namespace {

enum class Kind { Button, Menu };

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
};

constexpr int kNone = -1;
constexpr float kStickZone = 0.45f;
constexpr float kStickRadius = 0.13f;
constexpr float kStickDead = 0.3f;
constexpr float kGrab = 1.3f;
constexpr float kLetGo = 1.8f;
constexpr float kImageScale = 0.5f;
constexpr uint32_t kFace = 0x46FFFFFF;
constexpr uint32_t kFacePressed = 0x96FFFFFF;
constexpr uint32_t kLabel = 0xE6FFFFFF;
constexpr uint32_t kBase = 0x30FFFFFF;
constexpr uint32_t kKnob = 0x78FFFFFF;

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
std::vector<Finger> g_fingers;
uint32_t g_held = 0;
bool g_stick_active = false;
float g_stick_origin_x = 0.0f;
float g_stick_origin_y = 0.0f;
float g_stick_x = 0.0f;
float g_stick_y = 0.0f;
uint64_t g_version = 1;

std::vector<Spot> spots(int width, int height) {
    float u = static_cast<float>(height);
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);
    return {
        {Kind::Menu, "Menu", 0, 0.09f * u, 0.08f * u, 0.055f * u},
        {Kind::Button, "+", input::kButtonPlus, w - 0.08f * u, 0.08f * u, 0.05f * u},
        {Kind::Button, "-", input::kButtonMinus, w - 0.21f * u, 0.08f * u, 0.05f * u},
        {Kind::Button, "2", input::kButtonTwo, w - 0.15f * u, h - 0.17f * u, 0.11f * u},
        {Kind::Button, "1", input::kButtonOne, w - 0.39f * u, h - 0.12f * u, 0.09f * u},
        {Kind::Button, "A", input::kButtonA, w - 0.19f * u, h - 0.44f * u, 0.09f * u},
        {Kind::Button, "B", input::kButtonB, w - 0.42f * u, h - 0.36f * u, 0.075f * u},
        {Kind::Button, "~", input::kMotionShake, w - 0.10f * u, h - 0.66f * u, 0.065f * u},
    };
}

float rest_x(int height) {
    return 0.24f * static_cast<float>(height);
}

float rest_y(int height) {
    return static_cast<float>(height) - 0.26f * static_cast<float>(height);
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
    float radius = kStickRadius * static_cast<float>(g_height);
    float dx = (x - g_stick_origin_x) / radius;
    float dy = (y - g_stick_origin_y) / radius;
    float length = std::hypot(dx, dy);
    if (length > 1.0f) {
        dx /= length;
        dy /= length;
    }
    if (std::fabs(dx - g_stick_x) > 0.02f || std::fabs(dy - g_stick_y) > 0.02f) {
        g_version++;
    }
    g_stick_x = dx;
    g_stick_y = dy;
}

void draw_circle(canvas::Canvas& image, float x, float y, float radius, uint32_t color) {
    int left = static_cast<int>(std::lround(x - radius));
    int top = static_cast<int>(std::lround(y - radius));
    int size = static_cast<int>(std::lround(radius * 2.0f));
    image.fill_round(left, top, left + size, top + size, size / 2, color);
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
            image.text(face, kLabel, spot.label, static_cast<int>(x - radius), static_cast<int>(y - radius), static_cast<int>(x + radius),
                       static_cast<int>(y + radius), canvas::Align::Center, false);
        }
    }
    float base_x = (g_stick_active ? g_stick_origin_x : rest_x(height)) * kImageScale;
    float base_y = (g_stick_active ? g_stick_origin_y : rest_y(height)) * kImageScale;
    float radius = kStickRadius * static_cast<float>(height) * kImageScale;
    draw_circle(image, base_x, base_y, radius, kBase);
    draw_circle(image, base_x + g_stick_x * radius, base_y + g_stick_y * radius, radius * 0.45f, kKnob);
    result.pixels = std::move(image.pixels);
    result.width = static_cast<uint32_t>(image_width);
    result.height = static_cast<uint32_t>(image_height);
    return result;
}

}

bool enabled() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_enabled;
}

void set_enabled(bool on) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_enabled != on) {
        g_enabled = on;
        g_version++;
    }
}

void finger(int64_t id, Phase phase, float x, float y, int width, int height) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_enabled || width <= 0 || height <= 0) {
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
    g_fingers.erase(found);
    update_held(all);
}

void other_input() {
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_enabled && !g_hidden && g_fingers.empty()) {
        g_hidden = true;
        g_version++;
    }
}

State poll(bool sideways) {
    std::lock_guard<std::mutex> hold(g_lock);
    State state;
    if (!g_enabled) {
        return state;
    }
    state.buttons = g_held;
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
    return state;
}

bool take_menu_press() {
    std::lock_guard<std::mutex> hold(g_lock);
    bool pressed = g_menu_pressed;
    g_menu_pressed = false;
    return pressed;
}

bool take(uint64_t& version, options::Image& image, bool& visible, int width, int height) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (width != g_width || height != g_height) {
        g_width = width;
        g_height = height;
        g_version++;
    }
    bool show = g_enabled && !g_hidden && width > 0 && height > 0;
    if (version == g_version && visible == show) {
        return false;
    }
    version = g_version;
    visible = show;
    if (show) {
        image = render(width, height);
    }
    return true;
}

}
