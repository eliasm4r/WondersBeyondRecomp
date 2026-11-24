#include "wp/ghosts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>

#include "wp/canvas.h"
#include "wp/disc.h"
#include "wp/online.h"
#include "wp/ui_text.h"

namespace wp::ghosts {

namespace {

constexpr float kEfbWidth = 640.0f;
constexpr float kEfbHeight = 528.0f;
constexpr float kPictureHeight = 456.0f;
constexpr float kNearest = 0.1f;
constexpr uint32_t kBox = 0x9E080A0E;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr auto kStale = std::chrono::milliseconds(500);
constexpr auto kToastTime = std::chrono::seconds(5);
constexpr auto kListEvery = std::chrono::milliseconds(250);
constexpr size_t kMostToasts = 5;
constexpr const char* kHeroLabels[] = {"online.hero.0", "online.hero.1", "online.hero.2", "online.hero.3"};

struct Label {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int tip_x = 0;
    int tip_y = 0;
    std::string name;
    uint32_t color = kWhite;
};

struct Line {
    std::string text;
    uint32_t color = kWhite;
    bool operator==(const Line& other) const { return text == other.text && color == other.color; }
};

struct Toast {
    Line line;
    std::chrono::steady_clock::time_point until;
};

struct PanelBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    const std::vector<Line>* lines = nullptr;
};

std::mutex g_mutex;
std::atomic<bool> g_active{false};
Projection g_projection;
bool g_have_projection = false;
View g_view{};
std::vector<Marker> g_markers;
std::chrono::steady_clock::time_point g_updated{};
options::Rect g_picture;
options::Rect g_placed;
bool g_dirty = false;
bool g_shown = false;
uint64_t g_version = 0;
bool g_markers_fresh = false;
std::atomic<bool> g_list_open{false};
std::vector<Toast> g_toasts;
std::vector<Line> g_toast_lines;
std::vector<Line> g_list_lines;
std::chrono::steady_clock::time_point g_list_built{};
online::Problem g_problem_shown = online::Problem::None;
bool g_reconnecting_shown = false;

const char* game_label(const char* key) {
    std::string language = disc::game_language();
    return language.empty() ? ui::label(key) : ui::label(key, ui::language_of(language));
}

std::string zone_text(uint32_t zone) {
    if (zone == 0) {
        return game_label("online.list.away");
    }
    std::string name = disc::field_name(zone);
    if (!name.empty()) {
        return name;
    }
    char digits[16];
    std::snprintf(digits, sizeof digits, "%04u", zone);
    return std::string(game_label("online.list.zone")) + " " + digits;
}

const char* hero_text(uint32_t character) {
    return character < std::size(kHeroLabels) ? game_label(kHeroLabels[character]) : "?";
}

void add_toast(std::string text, uint32_t color) {
    g_toasts.push_back({{std::move(text), color}, std::chrono::steady_clock::now() + kToastTime});
    if (g_toasts.size() > kMostToasts) {
        g_toasts.erase(g_toasts.begin());
    }
}

void read_notices() {
    std::vector<presence::Peer> peers = online::peers();
    auto color_of = [&](const std::string& name) {
        for (const presence::Peer& peer : peers) {
            if (peer.name == name) {
                return online::player_color(peer.id);
            }
        }
        return kWhite;
    };
    for (const online::Notice& notice : online::take_notices()) {
        switch (notice.kind) {
        case online::Notice::Kind::Joined:
            add_toast(notice.name + " " + game_label("online.notice.joined"), color_of(notice.name));
            break;
        case online::Notice::Kind::Left:
            add_toast(notice.name + " " + game_label("online.notice.left"), kWhite);
            break;
        case online::Notice::Kind::Zone:
            add_toast(notice.name + " : " + zone_text(notice.zone), color_of(notice.name));
            break;
        case online::Notice::Kind::Version:
            if (!notice.name.empty()) {
                add_toast(std::string(game_label("online.notice.version")) + " " + notice.name, kWhite);
            }
            break;
        }
    }
    online::Problem problem = online::state() == online::State::Failed ? online::problem() : online::Problem::None;
    if (problem != g_problem_shown) {
        g_problem_shown = problem;
        if (problem == online::Problem::HostLeft) {
            add_toast(game_label("online.state.host_left"), kWhite);
        } else if (problem == online::Problem::Lost) {
            add_toast(game_label("online.state.lost"), kWhite);
        } else if (problem == online::Problem::Version) {
            add_toast(std::string(game_label("online.state.version")) + " (" + online::other_version() + ")", kWhite);
        }
    }
    bool reconnecting = online::reconnecting();
    if (reconnecting != g_reconnecting_shown) {
        g_reconnecting_shown = reconnecting;
        if (reconnecting) {
            add_toast(game_label("online.state.reconnecting"), kWhite);
        }
    }
}

std::vector<Line> build_list() {
    std::vector<Line> lines;
    if (!online::running()) {
        return lines;
    }
    std::string name;
    uint32_t character = 0;
    uint32_t zone = 0;
    online::own_state(name, character, zone);
    lines.push_back({name + " (" + game_label("online.you") + ") - " + hero_text(character) + " - " + zone_text(zone), online::player_color(online::own_id())});
    for (const presence::Peer& peer : online::peers()) {
        std::string ping = peer.ping_ms >= 0 ? std::to_string(peer.ping_ms) + " ms" : "...";
        lines.push_back({peer.name + " - " + hero_text(peer.pose.character) + " - " + zone_text(peer.zone) + " - " + ping, online::player_color(peer.id)});
    }
    return lines;
}

void refresh_panels() {
    read_notices();
    auto now = std::chrono::steady_clock::now();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const Toast& toast) { return toast.until <= now; }), g_toasts.end());
    std::vector<Line> toasts;
    for (const Toast& toast : g_toasts) {
        toasts.push_back(toast.line);
    }
    if (toasts != g_toast_lines) {
        g_toast_lines = std::move(toasts);
        g_dirty = true;
    }
    if (!g_list_open.load()) {
        if (!g_list_lines.empty()) {
            g_list_lines.clear();
            g_dirty = true;
        }
    } else if (now - g_list_built >= kListEvery) {
        g_list_built = now;
        std::vector<Line> list = build_list();
        if (list != g_list_lines) {
            g_list_lines = std::move(list);
            g_dirty = true;
        }
    }
}

options::Image render(options::Rect& placed, bool show_markers) {
    options::Image image;
    if (g_picture.width <= 0 || g_picture.height <= 0 || !canvas::font_ok(canvas::FontId::Body)) {
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
    static const std::vector<Marker> kNoMarkers;
    for (const Marker& marker : show_markers && g_have_projection ? g_markers : kNoMarkers) {
        float u = 0.0f;
        float v = 0.0f;
        if (!to_picture(g_projection, g_view, marker.position, u, v)) {
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
        label.color = marker.color;
        left = std::min(left, label.left);
        top = std::min(top, label.top);
        right = std::max(right, label.right);
        bottom = std::max(bottom, tip_y);
        labels.push_back(std::move(label));
    }
    std::vector<PanelBox> panels;
    int margin = std::max(6, text_height);
    auto add_panel = [&](const std::vector<Line>& lines, bool centered) {
        if (lines.empty()) {
            return;
        }
        int widest = 0;
        for (const Line& line : lines) {
            widest = std::max(widest, static_cast<int>(std::ceil(canvas::text_width(face, line.text))));
        }
        int panel_width = std::min(g_picture.width, widest + padding * 2);
        PanelBox panel;
        panel.left = centered ? g_picture.x + (g_picture.width - panel_width) / 2 : g_picture.x + std::min(margin, g_picture.width - panel_width);
        panel.top = g_picture.y + margin;
        panel.right = panel.left + panel_width;
        panel.bottom = std::min(panel.top + box_height * static_cast<int>(lines.size()), g_picture.y + g_picture.height);
        panel.lines = &lines;
        left = std::min(left, panel.left);
        top = std::min(top, panel.top);
        right = std::max(right, panel.right);
        bottom = std::max(bottom, panel.bottom);
        panels.push_back(panel);
    };
    add_panel(g_toast_lines, false);
    add_panel(g_list_lines, true);
    if ((labels.empty() && panels.empty()) || right <= left || bottom <= top) {
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
        canvas.text(face, label.color, label.name, box_left + padding, box_top, box_right - padding, box_bottom, canvas::Align::Center, true);
    }
    for (const PanelBox& panel : panels) {
        canvas.fill_round(panel.left - left, panel.top - top, panel.right - left, panel.bottom - top, std::max(3, box_height / 3), kBox);
        int y = panel.top - top;
        for (const Line& line : *panel.lines) {
            if (y + box_height > panel.bottom - top) {
                break;
            }
            canvas.text(face, line.color, line.text, panel.left - left + padding, y, panel.right - left - padding, y + box_height, canvas::Align::Left, true);
            y += box_height;
        }
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels = std::move(canvas.pixels);
    placed = options::Rect{left, top, width, height};
    return image;
}

}

bool to_picture(const Projection& p, const View& view, const std::array<float, 3>& point, float& u, float& v) {
    float eye_x = view[0] * point[0] + view[1] * point[1] + view[2] * point[2] + view[3];
    float eye_y = view[4] * point[0] + view[5] * point[1] + view[6] * point[2] + view[7];
    float eye_z = view[8] * point[0] + view[9] * point[1] + view[10] * point[2] + view[11];
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

void set_view(const View& view, std::vector<Marker> markers) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_view = view;
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
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_panels();
    bool fresh = !g_markers.empty() && std::chrono::steady_clock::now() - g_updated < kStale;
    if (fresh != g_markers_fresh) {
        g_markers_fresh = fresh;
        g_dirty = true;
    }
    bool live = fresh || !g_toast_lines.empty() || !g_list_lines.empty();
    if (!live) {
        g_dirty = false;
        if (g_shown) {
            g_shown = false;
            g_version++;
        }
    } else if (g_dirty) {
        g_dirty = false;
        options::Rect placed;
        options::Image drawn = render(placed, fresh);
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

void toggle_list() {
    g_list_open.store(!g_list_open.load());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_list_built = {};
}

bool list_open() {
    return g_list_open.load();
}

}
