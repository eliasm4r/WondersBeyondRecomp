#include "wp/options_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/canvas.h"
#include "wp/ui_text.h"

namespace wp::options {

namespace {

using canvas::Align;
using canvas::Canvas;
using canvas::Face;
using canvas::FontId;

constexpr uint32_t kSelected = 0x2C2C30;
constexpr uint32_t kSeparator = 0x3A3A3E;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMuted = 0xA1A1A6;

Face make_face(bool bold, int height) {
    return canvas::make_face(bold ? FontId::SystemBold : FontId::System, height);
}

constexpr uint32_t kPanel = 0xF8F8F6;
constexpr uint32_t kOrange = 0xF27D0C;
constexpr uint32_t kOrangeHover = 0xF59A40;
constexpr uint32_t kInk = 0x3B3531;
constexpr uint32_t kInkMuted = 0x8D847C;
constexpr uint32_t kHelpInk = 0x6B625B;
constexpr uint32_t kChipOffInk = 0x9A928A;
constexpr uint32_t kChip = 0xECE9E4;
constexpr uint32_t kChipHover = 0xF5E6D6;
constexpr uint32_t kRowSelected = 0xFFEBD2;
constexpr uint32_t kRowHover = 0xFFF4E8;
constexpr uint32_t kMarker = 0xE2343B;
constexpr uint32_t kRule = 0xE2DDD6;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr float kTitleSpacing = 0.12f;
constexpr float kTabSpacing = 0.05f;
constexpr float kRowSpacing = 0.04f;

using Points = std::vector<std::array<float, 2>>;

constexpr float kOutline[][2] = {{0.04f, 0.03f}, {0.55f, 0.0f}, {0.96f, 0.02f}, {1.0f, 0.24f}, {0.98f, 0.95f},
                                 {0.70f, 0.98f}, {0.50f, 1.0f}, {0.30f, 0.97f}, {0.03f, 0.99f}, {0.0f, 0.46f}};

Points outline(float left, float top, float width, float height) {
    Points points;
    for (const auto& point : kOutline) {
        points.push_back({left + point[0] * width, top + point[1] * height});
    }
    return points;
}

void triangle(Canvas& canvas, float cx, float cy, float half, int direction, uint32_t color) {
    if (direction == 0) {
        canvas.fill_polygon({{cx - half, cy + half * 0.8f}, {cx + half, cy + half * 0.8f}, {cx, cy - half * 0.8f}}, color);
    } else if (direction == 2) {
        canvas.fill_polygon({{cx - half, cy - half * 0.8f}, {cx + half, cy - half * 0.8f}, {cx, cy + half * 0.8f}}, color);
    } else {
        float tip = direction > 0 ? half * 0.8f : -half * 0.8f;
        canvas.fill_polygon({{cx - tip, cy - half}, {cx + tip, cy}, {cx - tip, cy + half}}, color);
    }
}

void dashes(Canvas& canvas, int left, int right, int y, int unit) {
    int dash = std::max(3, unit / 2);
    int gap = std::max(2, unit / 3);
    int thickness = std::max(1, unit / 12);
    for (int x = left; x < right; x += dash + gap) {
        canvas.fill(x, y, std::min(x + dash, right), y + thickness, kRule);
    }
}

std::vector<std::string> wrap(const Face& face, const std::string& text, int width, size_t max_lines) {
    std::vector<std::string> lines;
    std::string line;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(' ', start);
        std::string word = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && canvas::text_width(face, candidate) > width) {
            lines.push_back(line);
            line = word;
            if (lines.size() + 1 == max_lines) {
                if (end != std::string::npos) {
                    line += text.substr(end);
                }
                break;
            }
        } else {
            line = candidate;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

struct Hint {
    std::vector<std::string> caps;
    const char* word;
};

void key_cap(Canvas& canvas, const Face& face, const std::string& cap, int x, int y, int width, int height) {
    canvas.fill_round(x, y, x + width, y + height, std::max(2, height / 4), kInk);
    float cx = x + width / 2.0f;
    float cy = y + height / 2.0f;
    float half = height * 0.22f;
    if (cap == "up") {
        triangle(canvas, cx, cy, half, 0, kWhite);
    } else if (cap == "down") {
        triangle(canvas, cx, cy, half, 2, kWhite);
    } else if (cap == "left") {
        triangle(canvas, cx, cy, half, -1, kWhite);
    } else if (cap == "right") {
        triangle(canvas, cx, cy, half, 1, kWhite);
    } else {
        canvas.text(face, kWhite, cap, x, y, x + width, y + height, Align::Center, false);
    }
}

int cap_width(const Face& face, const std::string& cap, int height) {
    if (cap == "up" || cap == "down" || cap == "left" || cap == "right") {
        return height;
    }
    return std::max(height, static_cast<int>(std::ceil(canvas::text_width(face, cap))) + height / 2);
}

void draw_hints(Canvas& canvas, const Rect& area, int unit) {
    Face cap_face = canvas::make_face(FontId::Body, unit * 3 / 4);
    Face word_face = canvas::make_face(FontId::Body, unit * 17 / 20, kRowSpacing);
    const Hint hints[] = {{{"up", "down"}, "hint.choose"}, {{"left", "right"}, "hint.change"}, {{"Q", "E"}, "hint.tab"}, {{ui::label("hint.escape")}, "hint.close"}};
    int cap_height = unit * 6 / 5;
    int inner = std::max(2, unit / 5);
    int after_caps = unit / 3;
    int between = unit + unit / 2;
    int total = 0;
    for (const Hint& hint : hints) {
        for (const std::string& cap : hint.caps) {
            total += cap_width(cap_face, cap, cap_height) + inner;
        }
        total += after_caps - inner + static_cast<int>(std::ceil(canvas::text_width(word_face, ui::label(hint.word)))) + between;
    }
    total -= between;
    int x = area.x + std::max(0, (area.width - total) / 2);
    int y = area.y + (area.height - cap_height) / 2;
    for (const Hint& hint : hints) {
        for (const std::string& cap : hint.caps) {
            int width = cap_width(cap_face, cap, cap_height);
            key_cap(canvas, cap_face, cap, x, y, width, cap_height);
            x += width + inner;
        }
        x += after_caps - inner;
        std::string word = ui::label(hint.word);
        int word_width = static_cast<int>(std::ceil(canvas::text_width(word_face, word)));
        canvas.text(word_face, kInkMuted, word, x, y, x + word_width + 1, y + cap_height, Align::Left, false);
        x += word_width + between;
    }
}

void draw_control(Canvas& canvas, const Menu& menu, const Row& row, bool selected, const Hover& hover, bool hovered, const Rect& control, int unit) {
    Face value_face = canvas::make_face(FontId::Body, unit * 9 / 10, kRowSpacing);
    int cy = control.y + control.height / 2;
    bool over_control = hovered && hover.part != Part::Label && hover.part != Part::None;
    switch (row.kind) {
    case Kind::Info:
        canvas.text(value_face, kInk, row.value, control.x, control.y, control.x + control.width, control.y + control.height, Align::Right, true);
        break;
    case Kind::Toggle: {
        int gap = std::max(2, unit / 3);
        int chip = (control.width - gap) / 2;
        const char* labels[] = {ui::label("menu.yes"), ui::label("menu.no")};
        for (int side = 0; side < 2; side++) {
            int left = control.x + side * (chip + gap);
            bool active = (side == 0) == row.on;
            bool over = over_control && !active && ((side == 0) == (hover.part != Part::Next));
            uint32_t fill = active ? kOrange : over ? kChipHover : kChip;
            canvas.fill_round(left, control.y, left + chip, control.y + control.height, control.height / 2, fill);
            canvas.text(value_face, active ? kWhite : kChipOffInk, labels[side], left + unit / 3, control.y, left + chip - unit / 3, control.y + control.height,
                        Align::Center, true);
        }
        break;
    }
    case Kind::Slider: {
        int left = control.x + unit / 2;
        int right = control.x + control.width - unit * 4;
        int thickness = std::max(4, unit / 3);
        int knob = std::max(5, unit * 11 / 20);
        int fill = left + static_cast<int>(std::lround((right - left) * row.fraction));
        canvas.fill_round(left, cy - thickness / 2, right, cy - thickness / 2 + thickness, thickness / 2, kChip);
        canvas.fill_round(left, cy - thickness / 2, std::max(fill, left + thickness), cy - thickness / 2 + thickness, thickness / 2, kOrange);
        canvas.fill_round(fill - knob, cy - knob, fill + knob, cy + knob, knob, over_control ? kOrangeHover : kOrange);
        canvas.fill_round(fill - knob / 2, cy - knob / 2, fill + knob / 2, cy + knob / 2, knob / 2, kWhite);
        canvas.text(value_face, kInk, row.value, right + unit, control.y, control.x + control.width, control.y + control.height, Align::Right, false);
        break;
    }
    case Kind::Key:
    case Kind::Pad: {
        bool waiting = selected && menu.capturing();
        uint32_t fill = waiting ? kOrange : over_control ? kChipHover : kChip;
        canvas.fill_round(control.x, control.y, control.x + control.width, control.y + control.height, control.height / 2, fill);
        std::string text = waiting ? ui::label(row.kind == Kind::Key ? "menu.press_key" : "menu.press_button") : row.value;
        canvas.text(value_face, waiting ? kWhite : kInk, text, control.x + unit / 2, control.y, control.x + control.width - unit / 2, control.y + control.height,
                    Align::Center, true);
        break;
    }
    case Kind::Button:
        canvas.fill_round(control.x, control.y, control.x + control.width, control.y + control.height, control.height / 2, over_control ? kOrangeHover : kOrange);
        canvas.text(value_face, kWhite, ui::label("menu.reset"), control.x + unit / 2, control.y, control.x + control.width - unit / 2, control.y + control.height,
                    Align::Center, true);
        break;
    case Kind::Choice: {
        float half = unit * 0.42f;
        float arrow = unit * 0.8f;
        bool over_previous = over_control && hover.part == Part::Previous;
        bool over_next = over_control && hover.part == Part::Next;
        triangle(canvas, control.x + arrow, static_cast<float>(cy), half, -1, over_previous ? kOrangeHover : kOrange);
        triangle(canvas, control.x + control.width - arrow, static_cast<float>(cy), half, 1, over_next ? kOrangeHover : kOrange);
        int margin = static_cast<int>(arrow * 2);
        canvas.text(value_face, kInk, row.value, control.x + margin, control.y, control.x + control.width - margin, control.y + control.height, Align::Center, true);
        break;
    }
    }
}

}

Image render(const Menu& menu, int client_height) {
    Image image;
    std::vector<Row> rows = menu.rows();
    Layout frame = layout(client_height, rows.size(), menu.scroll());
    if (frame.width <= 0 || frame.height <= 0 || !canvas::font_ok(FontId::Body)) {
        return image;
    }
    Canvas canvas{frame.width, frame.height, std::vector<uint32_t>(static_cast<size_t>(frame.width) * frame.height, 0)};
    int unit = frame.unit;
    const ui::Text& strings = ui::text();
    const Hover& hover = menu.hover();
    float panel_width = static_cast<float>(frame.width - frame.edge);
    float panel_height = static_cast<float>(frame.height - frame.edge);
    canvas.fill_polygon(outline(static_cast<float>(frame.edge), static_cast<float>(frame.edge), panel_width, panel_height), kOrange);
    canvas.fill_polygon(outline(0.0f, 0.0f, panel_width, panel_height), kPanel);

    Rect close = close_rect(frame);
    Face title_face = canvas::make_face(FontId::Heavy, unit * 3 / 2, kTitleSpacing);
    canvas.text(title_face, kOrange, strings.menu_title, frame.padding, frame.padding, close.x - unit, frame.padding + frame.title_height, Align::Left, true);
    canvas.fill_round(close.x, close.y, close.x + close.width, close.y + close.height, close.width / 2, hover.part == Part::Close ? kOrangeHover : kOrange);
    canvas.cross(close.x + close.width / 2, close.y + close.height / 2, close.width / 5, std::max(2, unit / 7), kWhite);

    Face tab_face = canvas::make_face(FontId::Body, unit, kTabSpacing);
    for (size_t tab = 0; tab < kTabCount; tab++) {
        Rect r = tab_rect(frame, tab);
        bool active = tab == menu.tab();
        bool over = !active && hover.part == Part::Tab && hover.tab == static_cast<int>(tab);
        float skew = std::tan(12.0f * 3.14159265f / 180.0f) * r.height;
        float left = static_cast<float>(r.x);
        float right = static_cast<float>(r.x + r.width);
        float top = static_cast<float>(r.y);
        float bottom = static_cast<float>(r.y + r.height);
        canvas.fill_polygon({{left + skew, top}, {right, top}, {right - skew, bottom}, {left, bottom}}, active ? kOrange : over ? kChipHover : kChip);
        canvas.text(tab_face, active ? kWhite : kInkMuted, tab_label(tab), r.x + unit / 2, r.y, r.x + r.width - unit / 2, r.y + r.height, Align::Center, true);
    }

    Face label_face = canvas::make_face(FontId::Body, unit, kRowSpacing);
    Face note_face = canvas::make_face(FontId::Body, unit * 3 / 4);
    size_t first = frame.scroll;
    size_t last = std::min(rows.size(), first + kVisibleRows);
    for (size_t index = first; index < last; index++) {
        const Row& row = rows[index];
        size_t visible = index - first;
        Rect r = row_rect(frame, visible);
        Rect control = control_rect(frame, visible);
        bool selected = index == menu.selection();
        bool hovered = hover.row == static_cast<int>(index);
        if (selected || hovered) {
            canvas.fill_round(r.x, r.y + 1, r.x + r.width, r.y + r.height - 1, unit / 3, selected ? kRowSelected : kRowHover);
        }
        if (selected) {
            float cy = r.y + r.height / 2.0f;
            float size = unit * 0.38f;
            canvas.fill_polygon({{r.x + unit * 0.35f, cy - size}, {r.x + unit * 0.35f + size * 1.3f, cy}, {r.x + unit * 0.35f, cy + size}}, kMarker);
        }
        if (index + 1 < last && !selected && index + 1 != menu.selection()) {
            dashes(canvas, r.x + unit / 2, r.x + r.width - unit / 2, r.y + r.height - 1, unit);
        }
        int label_left = r.x + unit + unit / 2;
        int label_right = control.x - unit / 2;
        if (!row.live) {
            int note_width = static_cast<int>(std::ceil(canvas::text_width(note_face, std::string(strings.menu_restart))));
            int note_left = std::max(label_left + unit * 4, label_right - note_width);
            canvas.text(note_face, kInkMuted, strings.menu_restart, note_left, r.y, label_right, r.y + r.height, Align::Right, true);
            label_right = note_left - unit / 2;
        }
        canvas.text(label_face, kInk, row.label, label_left, r.y, label_right, r.y + r.height, Align::Left, true);
        draw_control(canvas, menu, row, selected, hover, hovered, control, unit);
    }

    if (rows.size() > kVisibleRows) {
        Rect area = rows_rect(frame);
        int width = std::max(3, unit / 4);
        int x = area.x + area.width - unit * 3 / 4;
        int top = area.y + unit / 2;
        int height = area.height - unit;
        int thumb = std::max(unit, static_cast<int>(height * kVisibleRows / rows.size()));
        int offset = static_cast<int>((height - thumb) * static_cast<double>(first) / (rows.size() - kVisibleRows));
        canvas.fill_round(x, top, x + width, top + height, width / 2, kChip);
        canvas.fill_round(x, top + offset, x + width, top + offset + thumb, width / 2, kOrange);
    }

    Rect help = help_rect(frame);
    canvas.fill(help.x, help.y + unit / 3, help.x + help.width, help.y + unit / 3 + std::max(2, unit / 8), kOrange);
    size_t described = hover.row >= 0 && static_cast<size_t>(hover.row) < rows.size() ? static_cast<size_t>(hover.row) : menu.selection();
    if (described < rows.size()) {
        Face help_face = canvas::make_face(FontId::Body, unit * 17 / 20, 0.02f);
        int line_height = unit * 11 / 10;
        int text_left = help.x + unit + unit / 3;
        int text_top = help.y + unit * 2 / 3;
        triangle(canvas, help.x + unit * 0.4f, text_top + line_height / 2.0f, unit * 0.32f, 1, kOrange);
        std::vector<std::string> lines = wrap(help_face, rows[described].help, help.x + help.width - text_left, 2);
        for (size_t i = 0; i < lines.size(); i++) {
            int top = text_top + static_cast<int>(i) * line_height;
            canvas.text(help_face, kHelpInk, lines[i], text_left, top, help.x + help.width, top + line_height, Align::Left, true);
        }
    }
    draw_hints(canvas, hints_rect(frame), unit);

    image.width = static_cast<uint32_t>(frame.width);
    image.height = static_cast<uint32_t>(frame.height);
    image.pixels = std::move(canvas.pixels);
    return image;
}

Image render_titlebar(int width, int height, const std::string& title, int hover, int pressed, bool maximized, bool focused) {
    Image image;
    if (width <= 0 || height <= 0) {
        return image;
    }
    constexpr uint32_t kBar = 0x161618;
    constexpr uint32_t kCloseHover = 0xE81123;
    constexpr uint32_t kClosePressed = 0xB50E1C;
    constexpr int kButtonCount = 3;
    Canvas canvas{width, height, std::vector<uint32_t>(static_cast<size_t>(width) * height, kBar)};
    int button = height * 46 / 32;
    int stroke = std::max(1, height / 32);
    int half = std::max(4, height * 5 / 32);
    int step = std::max(2, height * 2 / 32);
    int buttons_left = width - button * kButtonCount;
    int margin = height * 12 / 32;
    if (canvas::font_ok(FontId::System)) {
        Face face = make_face(false, std::max(8, height * 13 / 32));
        canvas.text(face, focused ? kText : kMuted, title, margin, 0, std::max(margin, buttons_left - margin), height - stroke, Align::Left, true);
    }
    canvas.fill(0, height - stroke, width, height, kSeparator);
    for (int i = 0; i < kButtonCount; i++) {
        int left = buttons_left + i * button;
        bool close = i == kButtonCount - 1;
        bool over = i == hover;
        if (over) {
            canvas.fill(left, 0, left + button, height - stroke, close ? (pressed == i ? kClosePressed : kCloseHover) : (pressed == i ? kSeparator : kSelected));
        }
        uint32_t color = over && close ? kText : focused ? kText : kMuted;
        int cx = left + button / 2;
        int cy = (height - stroke) / 2;
        if (i == 0) {
            canvas.fill(cx - half, cy, cx + half + 1, cy + stroke, color);
        } else if (i == 1 && !maximized) {
            canvas.fill(cx - half, cy - half, cx + half + 1, cy - half + stroke, color);
            canvas.fill(cx - half, cy + half, cx + half + 1, cy + half + stroke, color);
            canvas.fill(cx - half, cy - half, cx - half + stroke, cy + half + stroke, color);
            canvas.fill(cx + half, cy - half, cx + half + stroke, cy + half + stroke, color);
        } else if (i == 1) {
            canvas.fill(cx - half, cy - half + step, cx + half - step + 1, cy - half + step + stroke, color);
            canvas.fill(cx - half, cy + half, cx + half - step + 1, cy + half + stroke, color);
            canvas.fill(cx - half, cy - half + step, cx - half + stroke, cy + half + stroke, color);
            canvas.fill(cx + half - step, cy - half + step, cx + half - step + stroke, cy + half + stroke, color);
            canvas.fill(cx - half + step, cy - half, cx + half + 1, cy - half + stroke, color);
            canvas.fill(cx + half, cy - half, cx + half + stroke, cy + half - step + stroke, color);
        } else {
            for (int k = -half; k <= half; k++) {
                canvas.fill(cx + k, cy + k, cx + k + stroke, cy + k + stroke, color);
                canvas.fill(cx + k, cy - k, cx + k + stroke, cy - k + stroke, color);
            }
        }
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.pixels.resize(canvas.pixels.size());
    for (size_t i = 0; i < canvas.pixels.size(); i++) {
        image.pixels[i] = (canvas.pixels[i] & 0xFFFFFF) | 0xFF000000u;
    }
    return image;
}

}
