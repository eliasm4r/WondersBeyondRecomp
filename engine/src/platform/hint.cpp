#include "wp/hint.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "wp/canvas.h"
#include "wp/ui_text.h"
#include "wp/video.h"

namespace wp::hint {

namespace {

constexpr uint32_t kBox = 0x9E080A0E;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kTrack = 0x50FFFFFF;
constexpr uint32_t kFill = 0xFFFFFFFF;
constexpr int kSteps = 32;
constexpr double kMargin = 0.04;

std::mutex g_mutex;
options::Image g_image;
bool g_visible = false;
uint64_t g_version = 0;
std::string g_drawn_text;
int g_drawn_step = -1;
int g_drawn_height = -1;

options::Image render(const std::string& text, int step, int client_height) {
    options::Image image;
    if (!canvas::font_ok(canvas::FontId::Body)) {
        return image;
    }
    int text_height = std::max(10, client_height / 36);
    canvas::Face face = canvas::make_face(canvas::FontId::Body, text_height);
    int line_height = text_height * 5 / 4;
    int padding = std::max(5, text_height * 3 / 5);
    int bar = std::max(3, text_height / 5);
    int gap = std::max(3, text_height / 3);
    int width = static_cast<int>(std::ceil(canvas::text_width(face, text))) + padding * 2;
    int height = padding + line_height + gap + bar + padding;
    canvas::Canvas canvas{width, height, std::vector<uint32_t>(static_cast<size_t>(width) * height, 0)};
    canvas.fill_round(0, 0, width, height, std::max(3, line_height / 3), kBox);
    canvas.text(face, kWhite, text, padding, padding, width - padding, padding + line_height, canvas::Align::Left, false);
    int top = padding + line_height + gap;
    canvas.fill_round(padding, top, width - padding, top + bar, bar / 2, kTrack);
    int filled = padding + (width - padding * 2) * step / kSteps;
    if (filled > padding + bar) {
        canvas.fill_round(padding, top, filled, top + bar, bar / 2, kFill);
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels = std::move(canvas.pixels);
    return image;
}

}

void show(const char* label_key, double progress) {
    int client_width = 0;
    int client_height = 0;
    if (!video::client_size(client_width, client_height) || client_height <= 0) {
        return;
    }
    std::string text = ui::label(label_key);
    int step = std::clamp(static_cast<int>(progress * kSteps), 0, kSteps);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_visible && step == g_drawn_step && client_height == g_drawn_height && text == g_drawn_text) {
            return;
        }
    }
    options::Image image = render(text, step, client_height);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_image = std::move(image);
    g_drawn_text = text;
    g_drawn_step = step;
    g_drawn_height = client_height;
    g_visible = true;
    g_version++;
}

void hide() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_visible) {
        return;
    }
    g_visible = false;
    g_drawn_step = -1;
    g_version++;
}

bool take(uint64_t& version, options::Image& image, bool& visible) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (version == g_version) {
        return false;
    }
    version = g_version;
    visible = g_visible;
    image = std::move(g_image);
    g_image = options::Image{};
    return true;
}

options::Rect place(int client_width, int client_height, int width, int height) {
    options::Rect result;
    if (client_width <= 0 || client_height <= 0 || width <= 0 || height <= 0) {
        return result;
    }
    int margin = static_cast<int>(std::lround(std::min(client_width, client_height) * kMargin));
    double scale = std::min({1.0, static_cast<double>(client_width - margin * 2) / width, static_cast<double>(client_height - margin * 2) / height});
    result.width = std::max(1, static_cast<int>(width * scale));
    result.height = std::max(1, static_cast<int>(height * scale));
    result.x = client_width - margin - result.width;
    result.y = client_height - margin - result.height;
    return result;
}

}
