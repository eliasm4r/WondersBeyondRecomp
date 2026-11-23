#include "wp/ghosts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>

#include "wp/canvas.h"

namespace wp::ghosts {

namespace {

constexpr float kEfbWidth = 640.0f;
constexpr float kEfbHeight = 528.0f;
constexpr float kPictureHeight = 456.0f;
constexpr float kNearest = 0.1f;
constexpr uint32_t kBox = 0x9E080A0E;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr auto kStale = std::chrono::milliseconds(500);

struct Label {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int tip_x = 0;
    int tip_y = 0;
    std::string name;
};

std::mutex g_mutex;
std::atomic<bool> g_active{false};
Projection g_projection;
bool g_have_projection = false;
std::array<float, 3> g_eye{};
std::array<float, 3> g_target{};
std::vector<Marker> g_markers;
std::chrono::steady_clock::time_point g_updated{};
options::Rect g_picture;
options::Rect g_placed;
bool g_dirty = false;
bool g_shown = false;
uint64_t g_version = 0;

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

options::Image render(options::Rect& placed) {
    options::Image image;
    if (!g_have_projection || g_picture.width <= 0 || g_picture.height <= 0 || !canvas::font_ok(canvas::FontId::Body)) {
        return image;
    }
    int text_height = std::max(10, g_picture.height / 40);
    canvas::Face face = canvas::make_face(canvas::FontId::Body, text_height);
    int padding = std::max(3, text_height / 2);
    int box_height = text_height * 5 / 4 + padding;
    int tip = std::max(3, text_height / 2);
    std::vector<Label> labels;
    int left = g_picture.x + g_picture.width;
    int top = g_picture.y + g_picture.height;
    int right = g_picture.x;
    int bottom = g_picture.y;
    for (const Marker& marker : g_markers) {
        float u = 0.0f;
        float v = 0.0f;
        if (!to_picture(g_projection, g_eye, g_target, marker.position, u, v)) {
            continue;
        }
        int width = std::min(g_picture.width, static_cast<int>(std::ceil(canvas::text_width(face, marker.name))) + padding * 2);
        int tip_x = g_picture.x + static_cast<int>(u * g_picture.width);
        int tip_y = g_picture.y + static_cast<int>(v * g_picture.height);
        tip_y = std::clamp(tip_y, g_picture.y + box_height + tip, std::max(g_picture.y + box_height + tip, g_picture.y + g_picture.height));
        Label label;
        label.left = std::clamp(tip_x - width / 2, g_picture.x, g_picture.x + g_picture.width - width);
        label.right = label.left + width;
        label.bottom = tip_y - tip;
        label.top = label.bottom - box_height;
        label.tip_x = tip_x;
        label.tip_y = tip_y;
        label.name = marker.name;
        left = std::min(left, label.left);
        top = std::min(top, label.top);
        right = std::max(right, label.right);
        bottom = std::max(bottom, tip_y);
        labels.push_back(std::move(label));
    }
    if (labels.empty() || right <= left || bottom <= top) {
        return image;
    }
    int width = right - left;
    int height = bottom - top;
    canvas::Canvas canvas{width, height, std::vector<uint32_t>(static_cast<size_t>(width) * height, 0)};
    for (const Label& label : labels) {
        int box_left = label.left - left;
        int box_right = label.right - left;
        int box_top = label.top - top;
        int box_bottom = label.bottom - top;
        float x = static_cast<float>(std::clamp(label.tip_x - left, box_left + tip, std::max(box_left + tip, box_right - tip)));
        canvas.fill_round(box_left, box_top, box_right, box_bottom, std::max(3, box_height / 3), kBox);
        canvas.fill_polygon({{x - tip, static_cast<float>(box_bottom)}, {x + tip, static_cast<float>(box_bottom)}, {x, static_cast<float>(label.tip_y - top)}}, kBox);
        canvas.text(face, kWhite, label.name, box_left + padding, box_top, box_right - padding, box_bottom, canvas::Align::Center, true);
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels = std::move(canvas.pixels);
    placed = options::Rect{left, top, width, height};
    return image;
}

}

bool to_picture(const Projection& p, const std::array<float, 3>& eye, const std::array<float, 3>& target, const std::array<float, 3>& point, float& u, float& v) {
    std::array<float, 3> forward{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    float length = std::sqrt(dot(forward, forward));
    if (!(length > 1e-5f)) {
        return false;
    }
    for (float& value : forward) {
        value /= length;
    }
    std::array<float, 3> right{-forward[2], 0.0f, forward[0]};
    length = std::sqrt(dot(right, right));
    if (!(length > 1e-5f)) {
        return false;
    }
    right[0] /= length;
    right[2] /= length;
    std::array<float, 3> up{right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2], right[0] * forward[1] - right[1] * forward[0]};
    std::array<float, 3> offset{point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
    float eye_x = dot(right, offset);
    float eye_y = dot(up, offset);
    float eye_z = -dot(forward, offset);
    if (eye_z > -kNearest) {
        return false;
    }
    float x = (p.a * eye_x + p.b * eye_z) / -eye_z * p.x_scale + p.x_offset;
    float y = (p.c * eye_y + p.d * eye_z) / -eye_z * p.y_scale + p.y_offset;
    float efb_x = (x + 1.0f) * 0.5f * kEfbWidth;
    float efb_y = (1.0f - y) * 0.5f * kEfbHeight;
    u = (efb_x + p.margin_x) / (kEfbWidth + 2.0f * p.margin_x);
    v = (efb_y + p.margin_y) / (kPictureHeight + 2.0f * p.margin_y);
    return std::isfinite(u) && std::isfinite(v) && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

bool active() {
    return g_active.load(std::memory_order_relaxed);
}

void set_projection(const Projection& projection) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_projection = projection;
    g_have_projection = true;
}

void set_view(const std::array<float, 3>& eye, const std::array<float, 3>& target, std::vector<Marker> markers) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_eye = eye;
    g_target = target;
    g_markers = std::move(markers);
    g_updated = std::chrono::steady_clock::now();
    g_dirty = true;
    g_active.store(true, std::memory_order_relaxed);
}

void set_picture(const options::Rect& picture) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (picture.x != g_picture.x || picture.y != g_picture.y || picture.width != g_picture.width || picture.height != g_picture.height) {
        g_picture = picture;
        g_dirty = true;
    }
}

bool take(uint64_t& version, options::Image& image, bool& visible) {
    if (!active()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    bool live = !g_markers.empty() && std::chrono::steady_clock::now() - g_updated < kStale;
    if (!live) {
        g_dirty = false;
        if (g_shown) {
            g_shown = false;
            g_version++;
        }
    } else if (g_dirty) {
        g_dirty = false;
        options::Rect placed;
        options::Image drawn = render(placed);
        bool shown = !drawn.pixels.empty();
        if (shown || g_shown) {
            g_shown = shown;
            g_placed = placed;
            image = std::move(drawn);
            g_version++;
        }
    }
    if (version == g_version) {
        return false;
    }
    version = g_version;
    visible = g_shown;
    return true;
}

options::Rect place() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_placed;
}

}
