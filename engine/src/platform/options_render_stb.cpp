#include "wp/options_window.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wp/log.h"
#include "wp/ui_text.h"

namespace wp::options {

namespace {

constexpr uint32_t kPanel = 0x161618;
constexpr uint32_t kSelected = 0x2C2C30;
constexpr uint32_t kSeparator = 0x3A3A3E;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMuted = 0xA1A1A6;
constexpr uint32_t kAccent = 0xD4146F;
constexpr uint32_t kAccentHover = 0xE8307F;
constexpr uint32_t kHover = 0x202023;
constexpr uint32_t kControl = 0x1E1E21;
constexpr uint32_t kControlHover = 0x2A2A2E;
constexpr uint32_t kOff = 0x4A4A50;
constexpr uint32_t kOffHover = 0x5C5C63;
constexpr uint32_t kDanger = 0xC42B1C;
constexpr uint32_t kPanelAlpha = 250;
constexpr char32_t kEllipsis = 0x2026;

enum class Align { Left, Right, Center };

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

std::string matched_font(const char* pattern) {
#ifdef _WIN32
    (void)pattern;
    return "";
#else
    std::string command = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "";
    }
    std::string path;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        path += buffer;
    }
    pclose(pipe);
    return path;
#endif
}

Font load_font(bool bold) {
    std::vector<std::string> candidates = {matched_font(bold ? "sans-serif:weight=180" : "sans-serif")};
    const char* fallbacks[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",      "/Library/Fonts/Arial.ttf",
    };
    candidates.insert(candidates.end(), std::begin(fallbacks), std::end(fallbacks));
    if (const char* windows = std::getenv("WINDIR")) {
        std::string folder = std::string(windows) + "/Fonts/";
        candidates.insert(candidates.begin(), folder + (bold ? "seguisb.ttf" : "segoeui.ttf"));
        candidates.push_back(folder + "segoeui.ttf");
        candidates.push_back(folder + "arial.ttf");
    }
    Font font;
    for (const std::string& path : candidates) {
        std::error_code error;
        if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        font.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        int offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
        if (offset >= 0 && stbtt_InitFont(&font.info, font.data.data(), offset)) {
            font.ok = true;
            log::write("options", "menu font %s", path.c_str());
            return font;
        }
    }
    log::write("options", "no usable font for the options menu");
    return font;
}

const Font& font(bool bold) {
    static const Font regular = load_font(false);
    static const Font semibold = load_font(true);
    return bold && semibold.ok ? semibold : regular;
}

std::u32string decode(const std::string& text) {
    std::u32string result;
    for (size_t i = 0; i < text.size();) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
        char32_t value = length == 1 ? lead : lead & (0x7F >> length);
        for (int k = 1; k < length && i + k < text.size(); k++) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        result.push_back(value);
        i += static_cast<size_t>(length);
    }
    return result;
}

struct Face {
    const Font* font;
    float scale;
    int ascent;
    int descent;
};

Face make_face(bool bold, int height) {
    const Font& chosen = font(bold);
    Face face{&chosen, stbtt_ScaleForMappingEmToPixels(&chosen.info, static_cast<float>(height)), 0, 0};
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&chosen.info, &ascent, &descent, &gap);
    face.ascent = static_cast<int>(std::lround(ascent * face.scale));
    face.descent = static_cast<int>(std::lround(descent * face.scale));
    return face;
}

float advance(const Face& face, char32_t codepoint, char32_t next) {
    int width = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&face.font->info, static_cast<int>(codepoint), &width, &bearing);
    int kern = next ? stbtt_GetCodepointKernAdvance(&face.font->info, static_cast<int>(codepoint), static_cast<int>(next)) : 0;
    return (width + kern) * face.scale;
}

float text_width(const Face& face, const std::u32string& text) {
    float width = 0.0f;
    for (size_t i = 0; i < text.size(); i++) {
        width += advance(face, text[i], i + 1 < text.size() ? text[i + 1] : 0);
    }
    return width;
}

struct Canvas {
    int width;
    int height;
    std::vector<uint32_t> pixels;

    void fill(int left, int top, int right, int bottom, uint32_t color) {
        left = std::max(left, 0);
        top = std::max(top, 0);
        right = std::min(right, width);
        bottom = std::min(bottom, height);
        for (int y = top; y < bottom; y++) {
            std::fill(pixels.begin() + static_cast<std::ptrdiff_t>(y) * width + left, pixels.begin() + static_cast<std::ptrdiff_t>(y) * width + right, color);
        }
    }

    void fill_round(int left, int top, int right, int bottom, int radius, uint32_t color) {
        radius = std::max(0, std::min({radius, (right - left) / 2, (bottom - top) / 2}));
        for (int y = std::max(top, 0); y < std::min(bottom, height); y++) {
            for (int x = std::max(left, 0); x < std::min(right, width); x++) {
                double cx = x + 0.5;
                double cy = y + 0.5;
                double ox = cx < left + radius ? left + radius - cx : cx > right - radius ? cx - (right - radius) : 0.0;
                double oy = cy < top + radius ? top + radius - cy : cy > bottom - radius ? cy - (bottom - radius) : 0.0;
                double coverage = 1.0;
                if (ox > 0.0 && oy > 0.0) {
                    coverage = std::clamp(radius - std::sqrt(ox * ox + oy * oy) + 0.5, 0.0, 1.0);
                }
                blend(x, y, color, static_cast<unsigned>(coverage * 255.0 + 0.5));
            }
        }
    }

    void dot(int x, int y, int stroke, uint32_t color) {
        fill(x - stroke / 2, y - stroke / 2, x - stroke / 2 + stroke, y - stroke / 2 + stroke, color);
    }

    void cross(int cx, int cy, int half, int stroke, uint32_t color) {
        for (int k = -half; k <= half; k++) {
            dot(cx + k, cy + k, stroke, color);
            dot(cx + k, cy - k, stroke, color);
        }
    }

    void chevron(int cx, int cy, int half, int direction, int stroke, uint32_t color) {
        for (int k = 0; k <= half; k++) {
            int x = cx + direction * (half / 2 - k);
            dot(x, cy - half + k, stroke, color);
            dot(x, cy + half - k, stroke, color);
        }
    }

    void blend(int x, int y, uint32_t color, unsigned coverage) {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) {
            return;
        }
        uint32_t& target = pixels[static_cast<size_t>(y) * width + x];
        uint32_t result = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            unsigned below = (target >> shift) & 0xFF;
            unsigned above = (color >> shift) & 0xFF;
            unsigned mixed = (below * (255 - coverage) + above * coverage + 127) / 255;
            result |= mixed << shift;
        }
        target = result;
    }

    void text(const Face& face, uint32_t color, const std::string& value, int left, int top, int right, int bottom, Align align, bool ellipsis) {
        if (!face.font->ok) {
            return;
        }
        std::u32string glyphs = decode(value);
        if (ellipsis && text_width(face, glyphs) > right - left) {
            while (!glyphs.empty() && text_width(face, glyphs + kEllipsis) > right - left) {
                glyphs.pop_back();
            }
            glyphs.push_back(kEllipsis);
        }
        float width = text_width(face, glyphs);
        float x = align == Align::Left ? static_cast<float>(left) : align == Align::Right ? right - width : left + (right - left - width) / 2.0f;
        int baseline = top + (bottom - top - (face.ascent - face.descent)) / 2 + face.ascent;
        for (size_t i = 0; i < glyphs.size(); i++) {
            int codepoint = static_cast<int>(glyphs[i]);
            float origin = std::floor(x);
            float shift = x - origin;
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetCodepointBitmapBoxSubpixel(&face.font->info, codepoint, face.scale, face.scale, shift, 0.0f, &x0, &y0, &x1, &y1);
            int glyph_width = x1 - x0;
            int glyph_height = y1 - y0;
            if (glyph_width > 0 && glyph_height > 0) {
                std::vector<unsigned char> bitmap(static_cast<size_t>(glyph_width) * glyph_height);
                stbtt_MakeCodepointBitmapSubpixel(&face.font->info, bitmap.data(), glyph_width, glyph_height, glyph_width, face.scale, face.scale, shift, 0.0f,
                                                  codepoint);
                for (int gy = 0; gy < glyph_height; gy++) {
                    for (int gx = 0; gx < glyph_width; gx++) {
                        blend(static_cast<int>(origin) + x0 + gx, baseline + y0 + gy, color, bitmap[static_cast<size_t>(gy) * glyph_width + gx]);
                    }
                }
            }
            x += advance(face, glyphs[i], i + 1 < glyphs.size() ? glyphs[i + 1] : 0);
        }
    }
};

uint32_t corner_alpha(int x, int y, int width, int height, int radius) {
    double cx = x + 0.5;
    double cy = y + 0.5;
    double ox = cx < radius ? radius - cx : cx > width - radius ? cx - (width - radius) : 0.0;
    double oy = cy < radius ? radius - cy : cy > height - radius ? cy - (height - radius) : 0.0;
    if (ox <= 0.0 || oy <= 0.0) {
        return kPanelAlpha;
    }
    double coverage = std::clamp(radius - std::sqrt(ox * ox + oy * oy) + 0.5, 0.0, 1.0);
    return static_cast<uint32_t>(kPanelAlpha * coverage + 0.5);
}

// The menu is drawn in the spirit of PokéPark's own menus (a wooden board, a parchment sheet, sunny yellow and leaf
// green), with shapes drawn here rather than images from the game.
constexpr uint32_t kWoodTop = 0x9A6232;
constexpr uint32_t kWoodBottom = 0x74441F;
constexpr uint32_t kWoodRim = 0x4E2C12;
constexpr uint32_t kWoodGrain = 0x6A3D1B;
constexpr uint32_t kWoodLight = 0xB27840;
constexpr uint32_t kWoodHover = 0xC48A4E;
constexpr uint32_t kParchment = 0xFFF4D6;
constexpr uint32_t kParchmentLine = 0xEAD7AC;
constexpr uint32_t kParchmentShadow = 0x5A3416;
constexpr uint32_t kInk = 0x4A2A10;
constexpr uint32_t kInkSoft = 0x9C7A54;
constexpr uint32_t kCream = 0xFFF8E7;
constexpr uint32_t kSun = 0xFFD23F;
constexpr uint32_t kSunDark = 0xE0A000;
constexpr uint32_t kSunPale = 0xFFE89A;
constexpr uint32_t kSunHover = 0xFCEFC4;
constexpr uint32_t kLeaf = 0x4CAF50;
constexpr uint32_t kLeafDark = 0x2E7D32;
constexpr uint32_t kOffTrack = 0xCDB68C;
constexpr uint32_t kPill = 0xF2E0B6;
constexpr uint32_t kPillHover = 0xF8EACB;
constexpr uint32_t kBerry = 0xE53935;
constexpr uint32_t kBerryHover = 0xF4554F;
constexpr uint32_t kOrange = 0xF57C00;
constexpr uint32_t kOrangeHover = 0xFF9420;
constexpr uint32_t kWhite = 0xFFFFFF;

uint32_t mix(uint32_t a, uint32_t b, double t) {
    uint32_t result = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        double x = ((a >> shift) & 0xFF) * (1.0 - t) + ((b >> shift) & 0xFF) * t;
        result |= static_cast<uint32_t>(x + 0.5) << shift;
    }
    return result;
}

// A rounded shape with a darker copy below it, like the raised buttons of the game.
void raised(Canvas& canvas, const Rect& r, int radius, int depth, uint32_t face, uint32_t shadow) {
    canvas.fill_round(r.x, r.y + depth, r.x + r.width, r.y + r.height + depth, radius, shadow);
    canvas.fill_round(r.x, r.y, r.x + r.width, r.y + r.height, radius, face);
}

void wood(Canvas& canvas, int unit) {
    for (int y = 0; y < canvas.height; y++) {
        canvas.fill(0, y, canvas.width, y + 1, mix(kWoodTop, kWoodBottom, static_cast<double>(y) / std::max(1, canvas.height - 1)));
    }
    int plank = unit * 4;
    for (int y = plank; y < canvas.height; y += plank) {
        canvas.fill(0, y, canvas.width, y + std::max(1, unit / 10), kWoodGrain);
    }
    // Grain: a few long, slightly darker streaks at fixed places.
    for (int i = 0; i < canvas.height / std::max(1, unit / 2); i++) {
        int y = (i * 37 + 11) * unit / 6 % std::max(1, canvas.height);
        int left = (i * 53) % std::max(1, canvas.width / 2);
        int length = canvas.width / 3 + (i * 29) % std::max(1, canvas.width / 2);
        for (int x = left; x < std::min(canvas.width, left + length); x++) {
            canvas.blend(x, y, kWoodGrain, 70);
        }
    }
    int rim = std::max(2, unit / 4);
    canvas.fill(0, 0, canvas.width, rim, kWoodRim);
    canvas.fill(0, canvas.height - rim, canvas.width, canvas.height, kWoodRim);
    canvas.fill(0, 0, rim, canvas.height, kWoodRim);
    canvas.fill(canvas.width - rim, 0, canvas.width, canvas.height, kWoodRim);
}

}

Image render(const Menu& menu, int client_height) {
    Image image;
    std::vector<Row> rows = menu.rows();
    Layout frame = layout(client_height, rows.size(), menu.scroll());
    if (frame.width <= 0 || frame.height <= 0 || !font(false).ok) {
        return image;
    }
    Canvas canvas{frame.width, frame.height, std::vector<uint32_t>(static_cast<size_t>(frame.width) * frame.height, kWoodTop)};
    int unit = frame.unit;
    int depth = std::max(2, unit / 6);
    int stroke = std::max(2, unit / 7);
    Face title_face = make_face(true, unit * 3 / 2);
    Face tab_face = make_face(true, unit * 9 / 10);
    Face row_face = make_face(false, unit);
    Face value_face = make_face(true, unit * 9 / 10);
    Face small_face = make_face(false, unit * 4 / 5);
    const ui::Text& strings = ui::text();
    const Hover& hover = menu.hover();
    wood(canvas, unit);

    // Title on a leaf-green banner, with a sun-yellow berry on its left.
    Rect close = close_rect(frame);
    Rect banner{frame.padding, frame.padding, close.x - unit - frame.padding, frame.title_height - unit};
    raised(canvas, banner, banner.height / 2, depth, kLeaf, kLeafDark);
    int berry = banner.height - unit / 2;
    int berry_x = banner.x + unit / 2;
    int berry_y = banner.y + (banner.height - berry) / 2;
    canvas.fill_round(berry_x, berry_y, berry_x + berry, berry_y + berry, berry / 2, kSun);
    canvas.fill_round(berry_x + berry / 5, berry_y + berry / 5, berry_x + berry / 2, berry_y + berry / 2, berry / 6, kSunPale);
    canvas.text(title_face, kCream, strings.menu_title, berry_x + berry + unit / 2, banner.y, banner.x + banner.width - unit, banner.y + banner.height, Align::Left, true);
    bool close_hover = hover.part == Part::Close;
    raised(canvas, close, close.width / 2, depth, close_hover ? kBerryHover : kBerry, kWoodRim);
    canvas.cross(close.x + close.width / 2, close.y + close.height / 2, unit / 3, stroke, kWhite);

    for (size_t tab = 0; tab < kTabCount; tab++) {
        Rect r = tab_rect(frame, tab);
        bool active = tab == menu.tab();
        bool hovered = hover.part == Part::Tab && hover.tab == static_cast<int>(tab);
        raised(canvas, r, unit * 2 / 3, depth, active ? kSun : hovered ? kWoodHover : kWoodLight, active ? kSunDark : kWoodRim);
        canvas.text(tab_face, active ? kInk : kCream, tab_label(tab), r.x + unit / 3, r.y, r.x + r.width - unit / 3, r.y + r.height, Align::Center, true);
    }

    Rect sheet = rows_rect(frame);
    raised(canvas, sheet, unit, depth, kParchment, kParchmentShadow);
    size_t first = menu.scroll();
    size_t last = std::min(rows.size(), first + kVisibleRows);
    for (size_t i = first; i < last; i++) {
        const Row& row = rows[i];
        size_t visible = i - first;
        Rect line = row_rect(frame, visible);
        bool selected = i == menu.selection();
        bool hovered = hover.row == static_cast<int>(i);
        if (visible > 0) {
            canvas.fill(line.x + unit / 2, line.y, line.x + line.width - unit / 2, line.y + 1, kParchmentLine);
        }
        if (selected || hovered) {
            canvas.fill_round(line.x, line.y + 2, line.x + line.width, line.y + line.height - 2, unit / 2, selected ? kSunPale : kSunHover);
        }
        if (selected) {
            canvas.fill_round(line.x + unit / 4, line.y + unit / 2, line.x + unit / 4 + std::max(3, unit / 4), line.y + line.height - unit / 2,
                              std::max(2, unit / 8), kOrange);
        }
        Rect control = control_rect(frame, visible);
        int cy = control.y + control.height / 2;
        bool over_control = hovered && hover.part != Part::Label;
        int value_left = control.x;
        switch (row.kind) {
        case Kind::Toggle: {
            int width = unit * 3;
            int left = control.x + control.width - width;
            value_left = left;
            canvas.fill_round(left, control.y, left + width, control.y + control.height, control.height / 2,
                              row.on ? (over_control ? kLeaf : kLeafDark) : (over_control ? kPill : kOffTrack));
            int knob = control.height - unit / 2;
            int knob_left = row.on ? left + width - knob - unit / 4 : left + unit / 4;
            canvas.fill_round(knob_left, control.y + unit / 4, knob_left + knob, control.y + unit / 4 + knob, knob / 2, kWhite);
            break;
        }
        case Kind::Slider: {
            int left = control.x + unit / 2;
            int right = control.x + control.width - unit * 4;
            int track = std::max(4, unit / 3);
            canvas.fill_round(left, cy - track / 2, right, cy - track / 2 + track, track / 2, kOffTrack);
            int filled = left + static_cast<int>((right - left) * row.fraction + 0.5);
            canvas.fill_round(left, cy - track / 2, std::max(left + track, filled), cy - track / 2 + track, track / 2, kSunDark);
            int knob = unit;
            canvas.fill_round(filled - knob / 2, cy - knob / 2, filled + knob / 2, cy + knob / 2, knob / 2, kInk);
            canvas.fill_round(filled - knob / 2 + 2, cy - knob / 2 + 2, filled + knob / 2 - 2, cy + knob / 2 - 2, knob / 2 - 2, over_control ? kSunPale : kWhite);
            canvas.text(value_face, kInk, row.value, right + unit / 2, control.y, control.x + control.width, control.y + control.height, Align::Right, false);
            break;
        }
        case Kind::Button: {
            raised(canvas, control, control.height / 2, depth / 2, over_control ? kOrangeHover : kOrange, kWoodRim);
            canvas.text(value_face, kCream, ui::label("menu.reset"), control.x + unit / 2, control.y, control.x + control.width - unit / 2,
                        control.y + control.height, Align::Center, true);
            break;
        }
        case Kind::Key:
        case Kind::Pad: {
            bool waiting = selected && menu.capturing();
            canvas.fill_round(control.x, control.y, control.x + control.width, control.y + control.height, control.height / 2,
                              waiting ? kSun : over_control ? kPillHover : kPill);
            std::string shown = waiting ? ui::label(row.kind == Kind::Key ? "menu.press_key" : "menu.press_button") : row.value;
            canvas.text(value_face, kInk, shown, control.x + unit / 2, control.y, control.x + control.width - unit / 2, control.y + control.height, Align::Center,
                        true);
            break;
        }
        case Kind::Choice: {
            canvas.fill_round(control.x, control.y, control.x + control.width, control.y + control.height, control.height / 2, over_control ? kPillHover : kPill);
            int arrow = unit * 2;
            uint32_t previous_color = hovered && hover.part == Part::Previous ? kOrange : kInkSoft;
            uint32_t next_color = hovered && hover.part == Part::Next ? kOrange : kInkSoft;
            canvas.chevron(control.x + arrow / 2, cy, unit / 3, 1, stroke, previous_color);
            canvas.chevron(control.x + control.width - arrow / 2, cy, unit / 3, -1, stroke, next_color);
            canvas.text(value_face, kInk, row.value, control.x + arrow, control.y, control.x + control.width - arrow, control.y + control.height, Align::Center,
                        true);
            break;
        }
        }
        int label_right = value_left - unit / 2;
        if (!row.live) {
            int note_width = static_cast<int>(std::ceil(text_width(small_face, decode(strings.menu_restart))));
            int note_left = value_left - unit / 2 - note_width;
            canvas.text(small_face, kInkSoft, strings.menu_restart, note_left, line.y, value_left - unit / 2, line.y + line.height, Align::Right, false);
            label_right = note_left - unit / 2;
        }
        canvas.text(row_face, kInk, row.label, line.x + unit, line.y, std::max(line.x + unit * 2, label_right), line.y + line.height, Align::Left, true);
    }
    if (rows.size() > kVisibleRows) {
        int track_left = sheet.x + sheet.width - unit;
        int track_top = sheet.y + unit / 2;
        int track_height = sheet.height - unit;
        int width = std::max(4, unit / 3);
        canvas.fill_round(track_left, track_top, track_left + width, track_top + track_height, width / 2, kParchmentLine);
        int thumb = std::max(unit, static_cast<int>(track_height * kVisibleRows / rows.size()));
        int top = track_top + static_cast<int>((track_height - thumb) * first / (rows.size() - kVisibleRows));
        canvas.fill_round(track_left, top, track_left + width, top + thumb, width / 2, kSunDark);
    }

    int footer_top = sheet.y + sheet.height + depth;
    canvas.text(small_face, kCream, strings.menu_hint, frame.padding, footer_top, frame.width - frame.padding, frame.height - frame.padding / 2, Align::Center, true);

    image.width = static_cast<uint32_t>(frame.width);
    image.height = static_cast<uint32_t>(frame.height);
    image.pixels.resize(canvas.pixels.size());
    int radius = unit;
    for (int y = 0; y < frame.height; y++) {
        for (int x = 0; x < frame.width; x++) {
            size_t index = static_cast<size_t>(y) * image.width + static_cast<size_t>(x);
            image.pixels[index] = (canvas.pixels[index] & 0xFFFFFF) | (corner_alpha(x, y, frame.width, frame.height, radius) << 24);
        }
    }
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
    if (font(false).ok) {
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
