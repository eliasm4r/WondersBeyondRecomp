#include "wp/perf_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>

#include "wp/canvas.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::perf {

namespace {

constexpr uint32_t kBox = 0x9E080A0E;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kCpu = 0x4FC3F7;
constexpr uint32_t kGpu = 0x81C784;
constexpr uint32_t kRam = 0xF48FB1;
constexpr uint32_t kSystem = 0xFFB74D;
constexpr uint32_t kGrey = 0x9AA0A6;
constexpr const char* kDash = "\xE2\x80\x93";
constexpr const char* kDot = " \xC2\xB7 ";
constexpr const char* kTimes = "\xC3\x97";
constexpr double kMargin = 0.014;

std::string number(const char* format, double value) {
    if (value < 0.0) {
        return kDash;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, format, value);
    return buffer;
}

std::string total_gigabytes(double value) {
    return value < 0.0 ? kDash : number(value >= 10.0 ? "/ %.0f GB" : "/ %.1f GB", value);
}

std::string text_or_dash(const std::string& text) {
    return text.empty() ? kDash : text;
}

std::mutex g_mutex;
options::Image g_image;
bool g_visible = false;
Corner g_corner = Corner::TopLeft;
uint64_t g_version = 0;

}

Level level_from(const std::string& value) {
    return value == "minimal" ? Level::Minimal : value == "standard" ? Level::Standard : Level::Full;
}

Corner corner_from(const std::string& value) {
    return value == "top_right" ? Corner::TopRight : value == "bottom_left" ? Corner::BottomLeft : value == "bottom_right" ? Corner::BottomRight : Corner::TopLeft;
}

std::vector<Line> lines(const Sample& sample, Level level) {
    std::vector<Line> result;
    result.push_back({{"FPS", kWhite}, {number("%.0f", sample.fps), kWhite}, {number("%.1f ms", sample.frame_ms), kGrey}});
    if (level == Level::Minimal) {
        return result;
    }
    if (level == Level::Standard) {
        result.push_back({{"CPU", kCpu}, {number("%.0f %%", sample.cpu_percent), kWhite}, {"", kGrey}});
        result.push_back({{"GPU", kGpu}, {number("%.0f %%", sample.gpu_percent), kWhite}, {number("%.1f GB", sample.vram_used_gb), kGrey}});
        result.push_back({{"RAM", kRam}, {number("%.1f GB", sample.ram_used_gb), kWhite}, {"", kGrey}});
        return result;
    }
    result.push_back({{"Game", kWhite}, {number("%.0f fps", sample.game_fps), kWhite}, {number("%.0f late/s", sample.late_per_second), kGrey}});
    result.push_back({{"CPU", kCpu}, {number("%.0f %%", sample.cpu_percent), kWhite}, {text_or_dash(sample.cpu_name), kGrey}});
    result.push_back({{"Game", kCpu}, {number("%.0f %%", sample.game_thread_percent), kWhite}, {number("GPU thr. %.0f %%", sample.gpu_thread_percent), kGrey}});
    result.push_back({{"GPU", kGpu}, {number("%.0f %%", sample.gpu_percent), kWhite}, {number("%.1f ms", sample.gpu_ms), kGrey}});
    result.push_back({{"VRAM", kGpu}, {number("%.1f GB", sample.vram_used_gb), kWhite}, {total_gigabytes(sample.vram_total_gb), kGrey}});
    result.push_back({{"RAM", kRam}, {number("%.1f GB", sample.ram_used_gb), kWhite}, {total_gigabytes(sample.ram_total_gb), kGrey}});
    std::string size = sample.render_width > 0 && sample.render_height > 0 ? std::to_string(sample.render_width) + kTimes + std::to_string(sample.render_height) : kDash;
    std::string mode = sample.scale > 0 ? std::to_string(sample.scale) + "x" + kDot + "VSync " + (sample.vsync ? "on" : "off") : kDash;
    result.push_back({{text_or_dash(sample.api), kSystem}, {size, kWhite}, {mode, kGrey}});
    std::string device = sample.gpu_name;
    if (sample.refresh_hz > 0) {
        device += (device.empty() ? "" : kDot) + std::to_string(sample.refresh_hz) + " Hz";
    }
    result.push_back({{text_or_dash(device), kGrey}});
    return result;
}

options::Image render(const std::vector<Line>& lines, int client_height) {
    options::Image image;
    if (lines.empty() || !canvas::font_ok(canvas::FontId::Mono)) {
        return image;
    }
    int text_height = std::max(9, client_height / 49);
    canvas::Face face = canvas::make_face(canvas::FontId::Mono, text_height);
    int line_height = text_height * 5 / 4;
    int padding = std::max(4, text_height / 2);
    int gap = std::max(6, text_height * 4 / 5);
    int widths[3] = {0, 0, 0};
    int spanning = 0;
    for (const Line& line : lines) {
        if (line.size() == 1) {
            spanning = std::max(spanning, static_cast<int>(std::ceil(canvas::text_width(face, line[0].text))));
            continue;
        }
        for (size_t i = 0; i < line.size() && i < 3; i++) {
            widths[i] = std::max(widths[i], static_cast<int>(std::ceil(canvas::text_width(face, line[i].text))));
        }
    }
    int columns = widths[0] + gap + widths[1] + (widths[2] > 0 ? gap + widths[2] : 0);
    int width = std::max(columns, spanning) + padding * 2;
    int height = static_cast<int>(lines.size()) * line_height + padding * 2;
    canvas::Canvas canvas{width, height, std::vector<uint32_t>(static_cast<size_t>(width) * height, 0)};
    canvas.fill_round(0, 0, width, height, std::max(3, line_height / 3), kBox);
    int value_right = padding + widths[0] + gap + widths[1];
    int extra_right = width - padding;
    for (size_t row = 0; row < lines.size(); row++) {
        const Line& line = lines[row];
        int top = padding + static_cast<int>(row) * line_height;
        int bottom = top + line_height;
        if (line.size() == 1) {
            canvas.text(face, line[0].color, line[0].text, padding, top, width - padding, bottom, canvas::Align::Left, true);
            continue;
        }
        canvas.text(face, line[0].color, line[0].text, padding, top, padding + widths[0], bottom, canvas::Align::Left, false);
        if (line.size() > 1) {
            canvas.text(face, line[1].color, line[1].text, value_right - widths[1], top, value_right, bottom, canvas::Align::Right, false);
        }
        if (line.size() > 2 && !line[2].text.empty()) {
            canvas.text(face, line[2].color, line[2].text, extra_right - widths[2], top, extra_right, bottom, canvas::Align::Right, false);
        }
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels = std::move(canvas.pixels);
    return image;
}

options::Rect corner_rect(int client_width, int client_height, int width, int height, Corner corner) {
    options::Rect result;
    if (client_width <= 0 || client_height <= 0 || width <= 0 || height <= 0) {
        return result;
    }
    int margin = static_cast<int>(std::lround(client_width * kMargin));
    margin = std::min({margin, client_width / 4, client_height / 4});
    double scale = std::min({1.0, static_cast<double>(client_width - margin * 2) / width, static_cast<double>(client_height - margin * 2) / height});
    result.width = std::max(1, static_cast<int>(width * scale));
    result.height = std::max(1, static_cast<int>(height * scale));
    bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
    bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
    result.x = right ? client_width - margin - result.width : margin;
    result.y = bottom ? client_height - margin - result.height : margin;
    return result;
}

void show(options::Image image, Corner corner) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_image = std::move(image);
    g_corner = corner;
    g_visible = true;
    g_version++;
}

void hide() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_visible) {
        return;
    }
    g_visible = false;
    g_version++;
}

bool take(uint64_t& version, options::Image& image, bool& visible, Corner& corner) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (version == g_version) {
        return false;
    }
    version = g_version;
    visible = g_visible;
    corner = g_corner;
    image = std::move(g_image);
    g_image = options::Image{};
    return true;
}

void update() {
    static const settings::LiveFlag shown("overlay.show", nullptr);
    static uint64_t drawn_revision = ~0ull;
    static std::string drawn_level;
    static std::string drawn_corner;
    static int drawn_height = -1;
    if (!shown()) {
        drawn_revision = ~0ull;
        hide();
        return;
    }
    system_figures();
    int client_width = 0;
    int client_height = 0;
    if (!video::client_size(client_width, client_height) || client_height <= 0) {
        return;
    }
    std::string level = settings::text("overlay.level", nullptr);
    std::string corner = settings::text("overlay.corner", nullptr);
    uint64_t current = revision();
    if (current == drawn_revision && level == drawn_level && corner == drawn_corner && client_height == drawn_height) {
        return;
    }
    drawn_revision = current;
    drawn_level = level;
    drawn_corner = corner;
    drawn_height = client_height;
    show(render(lines(latest(), level_from(level)), client_height), corner_from(corner));
}

}
