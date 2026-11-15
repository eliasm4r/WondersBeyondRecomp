#include "wp/free_camera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

#include "wp/input.h"
#include "wp/settings.h"

namespace wp::free_camera {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMouseRadians = 0.0016f;
constexpr float kStickYawSpeed = 2.6f;
constexpr float kStickPitchSpeed = 1.6f;
constexpr float kStickDeadZone = 0.2f;
constexpr float kRecenterRate = 8.0f;
constexpr float kRecenterDone = 0.01f;
constexpr float kTeleport = 40.0f;
constexpr int kMinSpeed = 10;
constexpr int kMaxSpeed = 300;
constexpr auto kDrivingWindow = std::chrono::milliseconds(300);

using Clock = std::chrono::steady_clock;
constexpr auto kGameStep = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 59.94));
constexpr size_t kMostMouseSamples = 4096;

struct MouseSample {
    Clock::time_point time;
    double x;
    double y;
};

std::mutex g_mouse_mutex;
std::deque<MouseSample> g_mouse_samples;
double g_mouse_total_x = 0.0;
double g_mouse_total_y = 0.0;
MouseSample g_mouse_taken{Clock::time_point{}, 0.0, 0.0};

MouseSample mouse_at(Clock::time_point time) {
    MouseSample before = g_mouse_taken;
    for (const MouseSample& sample : g_mouse_samples) {
        if (sample.time > time) {
            if (sample.time <= before.time) {
                return before;
            }
            double part = std::chrono::duration<double>(time - before.time) / std::chrono::duration<double>(sample.time - before.time);
            part = std::clamp(part, 0.0, 1.0);
            return {time, before.x + (sample.x - before.x) * part, before.y + (sample.y - before.y) * part};
        }
        before = sample;
    }
    return {time, g_mouse_total_x, g_mouse_total_y};
}
std::atomic<bool> g_recenter{false};
std::atomic<uint32_t> g_last_buttons{0};
std::atomic<bool> g_pointer{false};
std::atomic<int64_t> g_driven{0};
std::atomic<bool> g_fly{false};

float speed(const settings::LiveNumber& number) {
    return static_cast<float>(std::clamp(number(), kMinSpeed, kMaxSpeed)) / 100.0f;
}

float stick_axis(float value) {
    float size = std::fabs(value);
    if (size <= kStickDeadZone) {
        return 0.0f;
    }
    return std::copysign(std::min(1.0f, (size - kStickDeadZone) / (1.0f - kStickDeadZone)), value);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

Settings current() {
    static const settings::LiveFlag enabled("camera.free", "WP_FREE_CAMERA");
    static const settings::LiveNumber mouse("camera.mouse_speed", nullptr);
    static const settings::LiveNumber stick("camera.stick_speed", nullptr);
    static const settings::LiveFlag invert("camera.invert_y", nullptr);
    return {enabled(), speed(mouse), speed(stick), invert()};
}

float wrap(float angle) {
    angle = std::fmod(angle + kPi, 2.0f * kPi);
    if (angle < 0.0f) {
        angle += 2.0f * kPi;
    }
    return angle - kPi;
}

View from_offset(float x, float y, float z) {
    float flat = std::sqrt(x * x + z * z);
    return {std::atan2(x, z), std::atan2(y, flat), std::sqrt(flat * flat + y * y)};
}

void to_offset(const View& view, float out[3]) {
    float flat = view.distance * std::cos(view.elevation);
    out[0] = flat * std::sin(view.yaw);
    out[1] = view.distance * std::sin(view.elevation);
    out[2] = flat * std::cos(view.yaw);
}

Turn turn(float mouse_x, float mouse_y, float stick_x, float stick_y, float seconds, const Settings& settings) {
    float vertical = settings.invert ? -1.0f : 1.0f;
    Turn result;
    result.yaw = -(mouse_x * kMouseRadians * settings.mouse + stick_axis(stick_x) * kStickYawSpeed * settings.stick * seconds);
    result.pitch = vertical * (mouse_y * kMouseRadians * settings.mouse + stick_axis(stick_y) * kStickPitchSpeed * settings.stick * seconds);
    return result;
}

void Orbit::update(const float game_offset[3], const float target[3], Turn input, bool recenter, float seconds, float out[3]) {
    View game = from_offset(game_offset[0], game_offset[1], game_offset[2]);
    float dx = target[0] - target_[0];
    float dy = target[1] - target_[1];
    float dz = target[2] - target_[2];
    if (!seeded_ || dx * dx + dy * dy + dz * dz > kTeleport * kTeleport) {
        seeded_ = true;
        recentering_ = false;
        yaw_ = game.yaw;
        pitch_ = 0.0f;
    }
    for (int i = 0; i < 3; i++) {
        target_[i] = target[i];
    }
    if (input.yaw != 0.0f || input.pitch != 0.0f) {
        recentering_ = false;
        yaw_ = wrap(yaw_ + input.yaw);
        pitch_ += input.pitch;
    }
    if (recenter) {
        recentering_ = true;
    }
    if (recentering_) {
        float gap = wrap(game.yaw - yaw_);
        float step = std::min(1.0f, seconds * kRecenterRate);
        yaw_ = wrap(yaw_ + gap * step);
        pitch_ -= pitch_ * step;
        if (std::fabs(wrap(game.yaw - yaw_)) < kRecenterDone && std::fabs(pitch_) < kRecenterDone) {
            yaw_ = game.yaw;
            pitch_ = 0.0f;
            recentering_ = false;
        }
    }
    pitch_ = std::clamp(pitch_, kMinElevation - game.elevation, kMaxElevation - game.elevation);
    game_elevation_ = game.elevation;
    to_offset(View{yaw_, game.elevation + pitch_, game.distance}, out);
}

void Orbit::rest_on(float x, float y, float z) {
    pitch_ = std::max(pitch_, from_offset(x, y, z).elevation - game_elevation_);
}

void Orbit::reset() {
    seeded_ = false;
    recentering_ = false;
}

void add_mouse(float x, float y) {
    std::lock_guard<std::mutex> lock(g_mouse_mutex);
    g_mouse_total_x += x;
    g_mouse_total_y += y;
    if (g_mouse_samples.size() >= kMostMouseSamples) {
        g_mouse_samples.pop_front();
    }
    g_mouse_samples.push_back({Clock::now(), g_mouse_total_x, g_mouse_total_y});
}

void take_mouse(float& x, float& y) {
    std::lock_guard<std::mutex> lock(g_mouse_mutex);
    Clock::time_point now = Clock::now();
    Clock::time_point at = g_mouse_taken.time == Clock::time_point{} ? now : g_mouse_taken.time + kGameStep;
    at = std::clamp(at, now - 2 * kGameStep, now);
    if (at < g_mouse_taken.time) {
        at = g_mouse_taken.time;
    }
    MouseSample reached = mouse_at(at);
    x = static_cast<float>(reached.x - g_mouse_taken.x);
    y = static_cast<float>(reached.y - g_mouse_taken.y);
    g_mouse_taken = reached;
    while (!g_mouse_samples.empty() && g_mouse_samples.front().time <= at) {
        g_mouse_samples.pop_front();
    }
}

void note_buttons(uint32_t buttons) {
    uint32_t before = g_last_buttons.exchange(buttons);
    if ((buttons & input::kButtonB) && !(before & input::kButtonB)) {
        g_recenter = true;
    }
}

bool take_recenter() {
    return g_recenter.exchange(false);
}

void set_pointer_held(bool held) {
    g_pointer = held;
}

bool pointer_held() {
    return g_pointer;
}

void note_driving() {
    g_driven = now_ms();
}

bool driving() {
    return now_ms() - g_driven.load() < kDrivingWindow.count();
}

void toggle_fly() {
    g_fly = !g_fly.load();
}

bool flying() {
    return g_fly.load();
}

}
