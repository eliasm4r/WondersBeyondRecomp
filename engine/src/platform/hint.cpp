#include "wp/hint.h"

#include <algorithm>
#include <chrono>
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
constexpr uint32_t kDim = 0xC8D2DC;
constexpr uint32_t kTrack = 0x50FFFFFF;
constexpr uint32_t kFill = 0xFFFFFFFF;
constexpr int kSteps = 32;
constexpr int kNoteSteps = 200;
constexpr int kTimedSteps = 240;
constexpr int kBusySteps = 96;
constexpr double kBusySeconds = 1.2;
constexpr double kBusyWidth = 0.3;
constexpr double kMargin = 0.04;

enum class Motion { Still, Timed, Busy };

struct Entry {
    bool on = false;
    std::vector<std::string> lines;
    int step = 0;
    int steps = kSteps;
    Motion motion = Motion::Still;
    std::chrono::steady_clock::time_point started{};
    double seconds = 0.0;
};

bool operator==(const Entry& a, const Entry& b) {
    return a.on == b.on && a.lines == b.lines && a.step == b.step && a.steps == b.steps && a.motion == b.motion;
}

std::mutex g_mutex;
options::Image g_image;
bool g_visible = false;
uint64_t g_version = 0;
Entry g_hint;
Entry g_note;
Entry g_drawn;
int g_drawn_height = -1;

options::Image render(const Entry& entry, int client_height) {
    options::Image image;
    if (!canvas::font_ok(canvas::FontId::Body) || entry.lines.empty()) {
        return image;
    }
    int text_height = std::max(10, client_height / 36);
    canvas::Face face = canvas::make_face(canvas::FontId::Body, text_height);
    int small_text = std::max(9, text_height * 4 / 5);
    canvas::Face small = canvas::make_face(canvas::FontId::Body, small_text);
    int line_height = text_height * 5 / 4;
    int small_height = small_text * 5 / 4;
    int padding = std::max(5, text_height * 3 / 5);
    int bar = std::max(3, text_height / 5);
    int gap = std::max(3, text_height / 3);
    int width = 0;
    for (size_t i = 0; i < entry.lines.size(); i++) {
        width = std::max(width, static_cast<int>(std::ceil(canvas::text_width(i == 0 ? face : small, entry.lines[i]))));
    }
    width += padding * 2;
    int body = line_height + small_height * static_cast<int>(entry.lines.size() - 1);
    int height = padding + body + gap + bar + padding;
    canvas::Canvas canvas{width, height, std::vector<uint32_t>(static_cast<size_t>(width) * height, 0)};
    canvas.fill_round(0, 0, width, height, std::max(3, line_height / 3), kBox);
    int top = padding;
    for (size_t i = 0; i < entry.lines.size(); i++) {
        int next = top + (i == 0 ? line_height : small_height);
        canvas.text(i == 0 ? face : small, i == 0 ? kWhite : kDim, entry.lines[i], padding, top, width - padding, next, canvas::Align::Left, false);
        top = next;
    }
    top += gap;
    canvas.fill_round(padding, top, width - padding, top + bar, bar / 2, kTrack);
    int span = width - padding * 2;
    if (entry.motion == Motion::Busy) {
        double head = static_cast<double>(entry.step) / entry.steps * (1.0 + kBusyWidth);
        int from = padding + static_cast<int>(span * std::clamp(head - kBusyWidth, 0.0, 1.0));
        int to = padding + static_cast<int>(span * std::clamp(head, 0.0, 1.0));
        if (to - from > bar) {
            canvas.fill_round(from, top, to, top + bar, bar / 2, kFill);
        }
    } else {
        int filled = padding + span * entry.step / entry.steps;
        if (filled > padding + bar) {
            canvas.fill_round(padding, top, filled, top + bar, bar / 2, kFill);
        }
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels = std::move(canvas.pixels);
    return image;
}

void refresh(std::unique_lock<std::mutex>& lock) {
    while (true) {
        Entry wanted = g_hint.on ? g_hint : g_note;
        if (!wanted.on) {
            if (g_visible) {
                g_visible = false;
                g_drawn = Entry{};
                g_version++;
            }
            return;
        }
        int client_width = 0;
        int client_height = 0;
        lock.unlock();
        bool sized = video::client_size(client_width, client_height) && client_height > 0;
        lock.lock();
        if (!sized || (g_visible && wanted == g_drawn && client_height == g_drawn_height)) {
            return;
        }
        lock.unlock();
        options::Image image = render(wanted, client_height);
        lock.lock();
        if (!(wanted == (g_hint.on ? g_hint : g_note))) {
            continue;
        }
        g_image = std::move(image);
        g_drawn = std::move(wanted);
        g_drawn_height = client_height;
        g_visible = true;
        g_version++;
        return;
    }
}

int step_of(double progress, int steps) {
    return std::clamp(static_cast<int>(progress * steps), 0, steps);
}

bool advance(Entry& entry) {
    if (!entry.on || entry.motion == Motion::Still) {
        return false;
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - entry.started).count();
    int step = entry.motion == Motion::Timed ? step_of(entry.seconds > 0.0 ? seconds / entry.seconds : 1.0, entry.steps)
                                             : static_cast<int>(std::fmod(seconds, kBusySeconds) / kBusySeconds * entry.steps);
    if (step == entry.step) {
        return false;
    }
    entry.step = step;
    return true;
}

void start(const char* label_key, Motion motion, double seconds, double elapsed) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::string label = ui::label(label_key);
    if (!(g_hint.on && g_hint.motion == motion && g_hint.lines.size() == 1 && g_hint.lines[0] == label && g_hint.seconds == seconds)) {
        g_hint = Entry{};
        g_hint.on = true;
        g_hint.lines.assign(1, label);
        g_hint.motion = motion;
        g_hint.steps = motion == Motion::Timed ? kTimedSteps : kBusySteps;
        g_hint.seconds = seconds;
        g_hint.started = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(elapsed));
    }
    advance(g_hint);
    refresh(lock);
}

}

void show(const char* label_key, double progress) {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_hint.on = true;
    g_hint.motion = Motion::Still;
    g_hint.lines.assign(1, ui::label(label_key));
    g_hint.step = step_of(progress, kSteps);
    g_hint.steps = kSteps;
    refresh(lock);
}

void hold(const char* label_key, double seconds, double elapsed) {
    start(label_key, Motion::Timed, seconds, elapsed);
}

void busy(const char* label_key) {
    start(label_key, Motion::Busy, kBusySeconds, 0.0);
}

void hide() {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_hint.on) {
        return;
    }
    g_hint = Entry{};
    refresh(lock);
}

void note(const std::vector<std::string>& lines, double progress) {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_note.on = !lines.empty();
    g_note.lines = lines;
    g_note.step = step_of(progress, kNoteSteps);
    g_note.steps = kNoteSteps;
    refresh(lock);
}

void clear_note() {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_note.on) {
        return;
    }
    g_note = Entry{};
    refresh(lock);
}

bool take(uint64_t& version, options::Image& image, bool& visible) {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (advance(g_hint) || (!g_hint.on && advance(g_note))) {
        refresh(lock);
    }
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
