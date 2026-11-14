#include "prompts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "wp/canvas.h"
#include "wp/control_names.h"
#include "wp/gamepad.h"
#include "wp/input.h"
#include "wp/keymap.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/padmap.h"
#include "wp/real_wiimote.h"
#include "wp/settings.h"

namespace wp::game {

namespace {

constexpr uint32_t kFontPacked = 0x52464e54;
constexpr uint32_t kFontReady = 0x52464e55;
constexpr uint32_t kGlyphBlock = 0x54474c50;
constexpr uint32_t kMapBlock = 0x434d4150;
constexpr uint32_t kWidthBlock = 0x43574448;
constexpr uint16_t kFirstButton = 0xe041;
constexpr int kButtons = 8;
constexpr int kMaxFonts = 4;
constexpr int kMaxCell = 64 * 64;
constexpr int kFormatI4 = 0;
constexpr int kLevels = 15;

enum Button { kMove, kA, kB, kHome, kPlus, kMinus, kOne, kTwo };

constexpr keymap::Action kKeyActions[kButtons] = {keymap::Action::Up,   keymap::Action::A,     keymap::Action::B,   keymap::Action::Home,
                                                  keymap::Action::Plus, keymap::Action::Minus, keymap::Action::One, keymap::Action::Two};
constexpr int kPadActions[kButtons] = {-1, 0, 1, 6, 4, 5, 2, 3};

enum class Look { Original, Key, Arrow, Space, Mouse, Face, Shape, Pill, Stick, KeyCluster, Arrows, Menu, View, House };

struct Prompt {
    Look look = Look::Original;
    std::string text;
    int detail = 0;
    std::array<std::string, 4> keys;
    std::array<int, 4> arrows{};
};

struct Font {
    uint32_t address = 0;
    uint32_t glyph_block = 0;
    uint32_t sheets = 0;
    int cell_width = 0;
    int cell_height = 0;
    int baseline = 0;
    int per_row = 0;
    int per_sheet = 0;
    uint32_t sheet_size = 0;
    int sheet_width = 0;
    std::array<int, kButtons> glyph{};
    std::array<int, kButtons> width{};
    std::array<std::vector<uint8_t>, kButtons> original;
};

std::vector<Font> g_fonts;
std::string g_drawn;

int glyph_of(uint32_t font, uint16_t code) {
    uint32_t end = font + rd32(font + 8);
    for (uint32_t block = font + rd16(font + 0xc); block + 8 <= end;) {
        uint32_t size = rd32(block + 4);
        if (rd32(block) == kMapBlock) {
            uint16_t low = rd16(block + 8);
            uint16_t high = rd16(block + 10);
            uint16_t method = rd16(block + 12);
            if (code >= low && code <= high) {
                if (method == 0) {
                    return rd16(block + 0x14) + (code - low);
                }
                if (method == 1) {
                    uint16_t glyph = rd16(block + 0x14 + 2 * (code - low));
                    return glyph == 0xffff ? -1 : glyph;
                }
                if (method == 2) {
                    int count = rd16(block + 0x14);
                    for (int i = 0; i < count; i++) {
                        if (rd16(block + 0x16 + 4 * i) == code) {
                            return rd16(block + 0x18 + 4 * i);
                        }
                    }
                }
            }
        }
        if (size < 8) {
            break;
        }
        block += size;
    }
    return -1;
}

int drawn_width(uint32_t font, int glyph) {
    uint32_t end = font + rd32(font + 8);
    for (uint32_t block = font + rd16(font + 0xc); block + 8 <= end;) {
        uint32_t size = rd32(block + 4);
        if (size < 8) {
            break;
        }
        if (rd32(block) == kWidthBlock) {
            int first = rd16(block + 8);
            int last = rd16(block + 10);
            if (glyph >= first && glyph <= last) {
                return rd8(block + 0x10 + 3 * static_cast<uint32_t>(glyph - first) + 1);
            }
        }
        block += size;
    }
    return 0;
}

uint32_t i4_byte(const Font& font, int sheet, int x, int y) {
    int tiles = font.sheet_width / 8;
    return font.sheets + static_cast<uint32_t>(sheet) * font.sheet_size + static_cast<uint32_t>(((y / 8) * tiles + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2);
}

int get_pixel(const Font& font, int sheet, int x, int y) {
    uint8_t value = rd8(i4_byte(font, sheet, x, y));
    return x % 2 ? value & 15 : value >> 4;
}

void set_pixel(const Font& font, int sheet, int x, int y, int level) {
    uint32_t address = i4_byte(font, sheet, x, y);
    uint8_t value = rd8(address);
    wr8(address, static_cast<uint8_t>(x % 2 ? (value & 0xf0) | level : (value & 0x0f) | (level << 4)));
}

bool still_loaded(const Font& font) {
    return rd32(font.address) == kFontReady && rd32(font.glyph_block) == kGlyphBlock && rd8(font.glyph_block + 8) == font.cell_width &&
           rd8(font.glyph_block + 9) == font.cell_height;
}

class Coverage {
public:
    explicit Coverage(int size) : size_(size), values_(static_cast<size_t>(size) * size, 0.0f) {}

    int size() const { return size_; }
    float at(int x, int y) const { return values_[static_cast<size_t>(y) * size_ + x]; }

    template <typename Distance> void paint(Distance distance, float stroke) {
        for (int y = 0; y < size_; y++) {
            for (int x = 0; x < size_; x++) {
                float d = distance(x + 0.5f, y + 0.5f);
                float value = stroke > 0.0f ? stroke / 2.0f + 0.5f - std::fabs(d) : 0.5f - d;
                put(x, y, value);
            }
        }
    }

    void ring(float cx, float cy, float radius, float stroke) {
        paint([=](float x, float y) { return std::hypot(x - cx, y - cy) - radius; }, stroke);
    }

    void box(float left, float top, float right, float bottom, float radius, float stroke) {
        float cx = (left + right) / 2.0f;
        float cy = (top + bottom) / 2.0f;
        float hx = (right - left) / 2.0f - radius;
        float hy = (bottom - top) / 2.0f - radius;
        paint(
            [=](float x, float y) {
                float qx = std::fabs(x - cx) - hx;
                float qy = std::fabs(y - cy) - hy;
                return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
            },
            stroke);
    }

    void line(float x0, float y0, float x1, float y1, float stroke) {
        paint(
            [=](float x, float y) {
                float dx = x1 - x0;
                float dy = y1 - y0;
                float length = dx * dx + dy * dy;
                float t = length > 0.0f ? std::clamp(((x - x0) * dx + (y - y0) * dy) / length, 0.0f, 1.0f) : 0.0f;
                return std::hypot(x - x0 - t * dx, y - y0 - t * dy) - stroke / 2.0f;
            },
            0.0f);
    }

    void text(const std::string& value, float left, float top, float right, float bottom, int largest) {
        if (value.empty() || !canvas::font_ok(canvas::FontId::Heavy)) {
            return;
        }
        int height = largest;
        canvas::Face face = canvas::make_face(canvas::FontId::Heavy, height);
        while (height > 6 && canvas::text_width(face, value) > right - left) {
            face = canvas::make_face(canvas::FontId::Heavy, --height);
        }
        canvas::Canvas target{size_, size_, std::vector<uint32_t>(static_cast<size_t>(size_) * size_, 0)};
        target.text(face, 0xffffff, value, static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top)), static_cast<int>(std::ceil(right)),
                    static_cast<int>(std::ceil(bottom)), canvas::Align::Center, false);
        for (int y = 0; y < size_; y++) {
            for (int x = 0; x < size_; x++) {
                put(x, y, static_cast<float>(target.pixels[static_cast<size_t>(y) * size_ + x] >> 24) / 255.0f);
            }
        }
    }

private:
    void put(int x, int y, float value) {
        float& slot = values_[static_cast<size_t>(y) * size_ + x];
        slot = std::max(slot, std::clamp(value, 0.0f, 1.0f));
    }

    int size_;
    std::vector<float> values_;
};

void arrow(Coverage& target, float cx, float cy, float half, int direction, float stroke) {
    float dx[4] = {0.0f, 0.0f, -1.0f, 1.0f};
    float dy[4] = {-1.0f, 1.0f, 0.0f, 0.0f};
    float ux = dx[direction];
    float uy = dy[direction];
    float tip_x = cx + ux * half;
    float tip_y = cy + uy * half;
    target.line(cx - ux * half, cy - uy * half, tip_x, tip_y, stroke);
    float head = half * 0.65f;
    target.line(tip_x, tip_y, tip_x - ux * head - uy * head, tip_y - uy * head + ux * head, stroke);
    target.line(tip_x, tip_y, tip_x - ux * head + uy * head, tip_y - uy * head - ux * head, stroke);
}

void space_bar(Coverage& target, float cx, float cy, float half, float stroke) {
    float rise = half * 0.45f;
    target.line(cx - half, cy - rise, cx - half, cy + rise * 0.6f, stroke);
    target.line(cx - half, cy + rise * 0.6f, cx + half, cy + rise * 0.6f, stroke);
    target.line(cx + half, cy + rise * 0.6f, cx + half, cy - rise, stroke);
}

void playstation_shape(Coverage& target, int shape, float cx, float cy, float half, float stroke) {
    if (shape == 0) {
        target.line(cx - half, cy - half, cx + half, cy + half, stroke);
        target.line(cx - half, cy + half, cx + half, cy - half, stroke);
    } else if (shape == 1) {
        target.ring(cx, cy, half, stroke);
    } else if (shape == 2) {
        target.box(cx - half, cy - half, cx + half, cy + half, stroke / 2.0f, stroke);
    } else {
        float top = cy - half * 1.05f;
        float bottom = cy + half * 0.75f;
        float side = half * 1.1f;
        target.line(cx, top, cx - side, bottom, stroke);
        target.line(cx - side, bottom, cx + side, bottom, stroke);
        target.line(cx + side, bottom, cx, top, stroke);
    }
}

void draw_prompt(Coverage& target, const Prompt& prompt) {
    float s = static_cast<float>(target.size());
    float stroke = std::max(1.6f, s / 15.0f);
    float inset = stroke / 2.0f + 0.5f;
    float mid = s / 2.0f;
    int text_height = static_cast<int>(s * 0.62f);
    switch (prompt.look) {
    case Look::Original:
        break;
    case Look::Key:
        target.box(inset, inset, s - inset, s - inset, s * 0.18f, stroke);
        target.text(prompt.text, stroke * 2.0f, stroke, s - stroke * 2.0f, s - stroke * 1.5f, text_height);
        break;
    case Look::Arrow:
        target.box(inset, inset, s - inset, s - inset, s * 0.18f, stroke);
        arrow(target, mid, mid, s * 0.26f, prompt.detail, stroke);
        break;
    case Look::Space:
        target.box(inset, inset, s - inset, s - inset, s * 0.18f, stroke);
        space_bar(target, mid, mid + s * 0.05f, s * 0.27f, stroke);
        break;
    case Look::Mouse: {
        float left = s * 0.2f;
        float right = s - s * 0.2f;
        float split = s * 0.45f;
        float radius = (right - left) * 0.48f;
        target.box(left, inset, right, s - inset, radius, stroke);
        target.line(left, split, right, split, stroke);
        if (prompt.detail == 2) {
            target.line(mid, s * 0.12f, mid, split - stroke, stroke * 1.6f);
            break;
        }
        target.line(mid, inset, mid, split, stroke);
        Coverage body(target.size());
        body.box(left, inset, right, s - inset, radius, 0.0f);
        float from = prompt.detail == 0 ? left : mid;
        float to = prompt.detail == 0 ? mid : right;
        target.paint(
            [&](float x, float y) {
                bool inside = x >= from && x <= to && y <= split && body.at(static_cast<int>(x), static_cast<int>(y)) > 0.5f;
                return inside ? -1.0f : 1.0f;
            },
            0.0f);
        break;
    }
    case Look::Face:
        target.ring(mid, mid, mid - inset, stroke);
        target.text(prompt.text, stroke * 2.0f, stroke * 1.5f, s - stroke * 2.0f, s - stroke * 1.5f, text_height);
        break;
    case Look::Shape:
        target.ring(mid, mid, mid - inset, stroke);
        playstation_shape(target, prompt.detail, mid, mid, s * 0.2f, stroke);
        break;
    case Look::Pill:
        target.box(inset, s * 0.18f, s - inset, s - s * 0.18f, s * 0.2f, stroke);
        target.text(prompt.text, stroke * 1.5f, s * 0.18f + stroke, s - stroke * 1.5f, s - s * 0.18f - stroke, static_cast<int>(s * 0.46f));
        break;
    case Look::Stick:
        target.ring(mid, mid, mid - inset, stroke);
        target.ring(mid, mid, s * 0.27f, stroke);
        target.text(prompt.text, mid - s * 0.22f, mid - s * 0.22f, mid + s * 0.22f, mid + s * 0.22f, static_cast<int>(s * 0.36f));
        break;
    case Look::Menu:
        target.ring(mid, mid, mid - inset, stroke);
        for (int i = -1; i <= 1; i++) {
            target.line(mid - s * 0.2f, mid + i * s * 0.14f, mid + s * 0.2f, mid + i * s * 0.14f, stroke);
        }
        break;
    case Look::View:
        target.ring(mid, mid, mid - inset, stroke);
        target.box(mid - s * 0.2f, mid - s * 0.16f, mid + s * 0.06f, mid + s * 0.06f, stroke / 2.0f, stroke);
        target.box(mid - s * 0.06f, mid - s * 0.06f, mid + s * 0.2f, mid + s * 0.16f, stroke / 2.0f, stroke);
        break;
    case Look::House:
        target.ring(mid, mid, mid - inset, stroke);
        target.line(mid - s * 0.22f, mid - s * 0.01f, mid, mid - s * 0.22f, stroke);
        target.line(mid, mid - s * 0.22f, mid + s * 0.22f, mid - s * 0.01f, stroke);
        target.box(mid - s * 0.14f, mid - s * 0.06f, mid + s * 0.14f, mid + s * 0.2f, stroke / 4.0f, stroke);
        break;
    case Look::Arrows:
        for (int i = 0; i < 4; i++) {
            float dx[4] = {0.0f, 0.0f, -1.0f, 1.0f};
            float dy[4] = {-1.0f, 1.0f, 0.0f, 0.0f};
            arrow(target, mid + dx[i] * s * 0.27f, mid + dy[i] * s * 0.27f, s * 0.2f, i, stroke);
        }
        break;
    case Look::KeyCluster: {
        float key = s * 0.32f;
        float gap = (s - key * 3.0f) / 2.0f;
        float line_width = std::max(1.2f, stroke * 0.7f);
        float lefts[4] = {gap + key, gap + key, gap, gap + key * 2.0f};
        float tops[4] = {s * 0.5f - key - gap * 0.5f, s * 0.5f + gap * 0.5f, s * 0.5f + gap * 0.5f, s * 0.5f + gap * 0.5f};
        for (int i = 0; i < 4; i++) {
            float l = lefts[i] + line_width / 2.0f;
            float t = tops[i] + line_width / 2.0f;
            float r = lefts[i] + key - line_width / 2.0f;
            float b = tops[i] + key - line_width / 2.0f;
            target.box(l, t, r, b, key * 0.2f, line_width);
            if (prompt.arrows[i] >= 0) {
                arrow(target, (l + r) / 2.0f, (t + b) / 2.0f, key * 0.24f, prompt.arrows[i], line_width);
            } else {
                target.text(prompt.keys[i], l, t, r, b, static_cast<int>(key * 0.75f));
            }
        }
        break;
    }
    }
}

int arrow_of(const std::string& name) {
    return name == "Up" ? 0 : name == "Down" ? 1 : name == "Left" ? 2 : name == "Right" ? 3 : -1;
}

Prompt key_prompt(int button) {
    Prompt prompt;
    const keymap::Bindings& bindings = input::bindings();
    if (button == kMove) {
        constexpr keymap::Action kDirections[4] = {keymap::Action::Up, keymap::Action::Down, keymap::Action::Left, keymap::Action::Right};
        for (int i = 0; i < 4; i++) {
            const std::vector<int>& codes = bindings.of(kDirections[i]);
            if (codes.empty()) {
                return prompt;
            }
            prompt.arrows[i] = arrow_of(keymap::key_name(codes.front()));
            prompt.keys[i] = control_names::key(codes.front());
        }
        bool arrows = prompt.arrows[0] == 0 && prompt.arrows[1] == 1 && prompt.arrows[2] == 2 && prompt.arrows[3] == 3;
        prompt.look = arrows ? Look::Arrows : Look::KeyCluster;
        return prompt;
    }
    const std::vector<int>& codes = bindings.of(kKeyActions[button]);
    if (codes.empty()) {
        return prompt;
    }
    std::string name = keymap::key_name(codes.front());
    if (name == "MouseLeft" || name == "MouseRight" || name == "MouseMiddle") {
        prompt.look = Look::Mouse;
        prompt.detail = name == "MouseLeft" ? 0 : name == "MouseRight" ? 1 : 2;
    } else if (name == "Space") {
        prompt.look = Look::Space;
    } else if (arrow_of(name) >= 0) {
        prompt.look = Look::Arrow;
        prompt.detail = arrow_of(name);
    } else {
        prompt.look = Look::Key;
        prompt.text = control_names::key(codes.front());
    }
    return prompt;
}

Prompt pad_prompt(int button) {
    Prompt prompt;
    padmap::Family family = gamepad::shown_family();
    if (button == kMove) {
        prompt.look = Look::Stick;
        prompt.text = "L";
        return prompt;
    }
    padmap::Binding binding = padmap::current_mapping()[static_cast<size_t>(kPadActions[button])];
    if (binding.empty()) {
        return prompt;
    }
    padmap::Combo combo = binding.front();
    int physical = -1;
    for (int i = 0; i < padmap::kPhysicalCount; i++) {
        if (combo == (padmap::Combo{1} << i)) {
            physical = i;
        }
    }
    bool playstation = family == padmap::Family::PlayStation3 || family == padmap::Family::PlayStation4 || family == padmap::Family::PlayStation5;
    if (physical >= padmap::kSouth && physical <= padmap::kNorth) {
        if (playstation) {
            prompt.look = Look::Shape;
            prompt.detail = physical - padmap::kSouth;
        } else {
            prompt.look = Look::Face;
            prompt.text = control_names::pad_button(physical, family);
        }
    } else if (family == padmap::Family::Switch && (physical == padmap::kStart || physical == padmap::kBack)) {
        prompt.look = Look::Face;
        prompt.text = control_names::pad_button(physical, family);
    } else if (family == padmap::Family::Switch && physical == padmap::kGuide) {
        prompt.look = Look::House;
    } else if (physical == padmap::kStart && family != padmap::Family::Xbox360 && family != padmap::Family::PlayStation3) {
        prompt.look = Look::Menu;
    } else if (physical == padmap::kBack && family == padmap::Family::Xbox) {
        prompt.look = Look::View;
    } else if (physical >= padmap::kDpadUp && physical <= padmap::kDpadRight) {
        prompt.look = Look::Arrow;
        prompt.detail = physical - padmap::kDpadUp;
    } else {
        prompt.look = Look::Pill;
        prompt.text = physical >= 0 ? control_names::pad_button(physical, family) : control_names::pad_binding({combo}, family);
    }
    return prompt;
}

std::string describe(const Prompt& prompt) {
    std::string result = std::to_string(static_cast<int>(prompt.look)) + ":" + std::to_string(prompt.detail) + ":" + prompt.text;
    for (int i = 0; i < 4; i++) {
        result += ":" + prompt.keys[i] + std::to_string(prompt.arrows[i]);
    }
    return result + "\n";
}

void draw_font(Font& font, const std::array<Prompt, kButtons>& prompts) {
    for (int i = 0; i < kButtons; i++) {
        if (font.glyph[i] < 0) {
            continue;
        }
        int sheet = font.glyph[i] / font.per_sheet;
        int slot = font.glyph[i] % font.per_sheet;
        int x0 = (slot % font.per_row) * (font.cell_width + 1);
        int y0 = (slot / font.per_row) * (font.cell_height + 1);
        std::vector<uint8_t>& original = font.original[i];
        if (original.empty()) {
            original.resize(static_cast<size_t>(font.cell_width) * font.cell_height);
            for (int y = 0; y < font.cell_height; y++) {
                for (int x = 0; x < font.cell_width; x++) {
                    original[static_cast<size_t>(y) * font.cell_width + x] = static_cast<uint8_t>(get_pixel(font, sheet, x0 + x, y0 + y));
                }
            }
        }
        int size = std::min(font.baseline, font.width[i]) - 2;
        bool redraw = prompts[i].look != Look::Original && size > 4;
        Coverage icon(std::max(size, 1));
        if (redraw) {
            draw_prompt(icon, prompts[i]);
        }
        int left = (font.width[i] - size) / 2;
        int top = std::max(0, font.baseline - size - 1);
        for (int y = 0; y < font.cell_height; y++) {
            for (int x = 0; x < font.cell_width; x++) {
                int level = original[static_cast<size_t>(y) * font.cell_width + x];
                if (redraw) {
                    int ix = x - left;
                    int iy = y - top;
                    level = ix >= 0 && iy >= 0 && ix < size && iy < size ? static_cast<int>(std::lround(icon.at(ix, iy) * kLevels)) : 0;
                }
                set_pixel(font, sheet, x0 + x, y0 + y, level);
            }
        }
    }
}

constexpr uint32_t kTplMagic = 0x0020af30;
constexpr uint32_t kFormatCi8 = 9;
constexpr uint32_t kPaletteRgb5a3 = 2;
constexpr int kMaxPictures = 4;

enum class Spot { Bubble, Title };

struct PictureKind {
    Spot spot;
    int width;
    int height;
    int colours;
};

constexpr PictureKind kPictureKinds[] = {{Spot::Bubble, 110, 71, 80}, {Spot::Title, 290, 34, 64}};

struct Picture {
    const PictureKind* kind = nullptr;
    uint32_t tpl = 0;
    uint32_t header = 0;
    uint32_t image = 0;
    std::vector<std::array<int, 4>> colours;
    std::vector<uint8_t> original;
    float cx = 0.0f;
    float cy = 0.0f;
    float ring = 0.0f;
    std::array<int, 3> ink{};
};

std::vector<Picture> g_pictures;
std::array<Prompt, kButtons> g_prompts;
bool g_prompts_ready = false;

uint32_t ci8_byte(const Picture& picture, int x, int y) {
    int tiles = (picture.kind->width + 7) / 8;
    return picture.image + static_cast<uint32_t>(((y / 4) * tiles + x / 8) * 32 + (y % 4) * 8 + x % 8);
}

const std::array<int, 4>& colour_at(const Picture& picture, int x, int y) {
    return picture.colours[picture.original[static_cast<size_t>(y) * picture.kind->width + x]];
}

bool picture_here(const Picture& picture) {
    return rd32(picture.tpl) == kTplMagic && rd32(picture.tpl + 4) == 1 && rd32(rd32(picture.tpl + 8)) == picture.header && rd32(picture.header + 8) == picture.image;
}

uint8_t nearest(const Picture& picture, int r, int g, int b, int a) {
    int best = 0;
    long best_distance = -1;
    for (size_t i = 0; i < picture.colours.size(); i++) {
        const std::array<int, 4>& c = picture.colours[i];
        long dr = c[0] - r;
        long dg = c[1] - g;
        long db = c[2] - b;
        long da = c[3] - a;
        long distance = (dr * dr + dg * dg + db * db) * (c[3] + a) / 510 + 4 * da * da;
        if (best_distance < 0 || distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(i);
        }
    }
    return static_cast<uint8_t>(best);
}

bool find_spot(Picture& picture) {
    int width = picture.kind->width;
    int height = picture.kind->height;
    if (picture.kind->spot == Spot::Title) {
        int right = -1;
        for (int x = width - 1; x >= 0 && right < 0; x--) {
            for (int y = 0; y < height; y++) {
                if (colour_at(picture, x, y)[3] >= 128) {
                    right = x;
                    break;
                }
            }
        }
        picture.ring = height / 2.0f;
        picture.cx = right + 1.0f - picture.ring;
        picture.cy = height / 2.0f;
        return right > height;
    }
    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;
    int darkest = 255;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const std::array<int, 4>& c = colour_at(picture, x, y);
            int high = std::max({c[0], c[1], c[2]});
            int low = std::min({c[0], c[1], c[2]});
            if (c[3] >= 250 && high - low < 16 && high < 160) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                if (high < darkest) {
                    darkest = high;
                    picture.ink = {c[0], c[1], c[2]};
                }
            }
        }
    }
    if (right < left || bottom < top) {
        return false;
    }
    picture.cx = (left + right + 1) / 2.0f;
    picture.cy = (top + bottom + 1) / 2.0f;
    picture.ring = std::max(right - left + 1, bottom - top + 1) / 2.0f;
    return picture.ring >= 5.0f;
}

void restore_picture(const Picture& picture) {
    for (int y = 0; y < picture.kind->height; y++) {
        for (int x = 0; x < picture.kind->width; x++) {
            wr8(ci8_byte(picture, x, y), picture.original[static_cast<size_t>(y) * picture.kind->width + x]);
        }
    }
}

float spread(const Coverage& icon, int x, int y, int reach) {
    float best = 0.0f;
    for (int dy = -reach; dy <= reach; dy++) {
        for (int dx = -reach; dx <= reach; dx++) {
            int ix = x + dx;
            int iy = y + dy;
            if (dx * dx + dy * dy > reach * reach || ix < 0 || iy < 0 || ix >= icon.size() || iy >= icon.size()) {
                continue;
            }
            best = std::max(best, icon.at(ix, iy));
        }
    }
    return best;
}

void draw_picture(const Picture& picture, const Prompt& prompt) {
    restore_picture(picture);
    if (prompt.look == Look::Original) {
        return;
    }
    bool title = picture.kind->spot == Spot::Title;
    int size = static_cast<int>(std::lround(picture.ring * 2.0f)) + (title ? -4 : 2);
    Coverage icon(size);
    draw_prompt(icon, prompt);
    float wipe = picture.ring + 1.5f;
    float left = picture.cx - size / 2.0f;
    float top = picture.cy - size / 2.0f;
    for (int y = 0; y < picture.kind->height; y++) {
        for (int x = 0; x < picture.kind->width; x++) {
            if (std::hypot(x + 0.5f - picture.cx, y + 0.5f - picture.cy) > wipe) {
                continue;
            }
            int ix = static_cast<int>(std::floor(x - left));
            int iy = static_cast<int>(std::floor(y - top));
            uint8_t index;
            if (title) {
                float core = ix >= 0 && iy >= 0 && ix < size && iy < size ? icon.at(ix, iy) : 0.0f;
                float edge = std::min(1.0f, spread(icon, ix, iy, 1) * 1.5f);
                int grey = static_cast<int>(0x66 + (255 - 0x66) * std::min(1.0f, core * 2.0f));
                index = nearest(picture, grey, grey, grey, static_cast<int>(edge * 255.0f));
            } else {
                float a = ix >= 0 && iy >= 0 && ix < size && iy < size ? icon.at(ix, iy) : 0.0f;
                auto mix = [a](int ink) { return static_cast<int>(std::lround(255.0f + (ink - 255.0f) * a)); };
                index = nearest(picture, mix(picture.ink[0]), mix(picture.ink[1]), mix(picture.ink[2]), 255);
            }
            wr8(ci8_byte(picture, x, y), index);
        }
    }
}

}

void update_prompts() {
    if (g_fonts.empty() && g_pictures.empty()) {
        return;
    }
    g_fonts.erase(std::remove_if(g_fonts.begin(), g_fonts.end(), [](const Font& font) { return !still_loaded(font); }), g_fonts.end());
    g_pictures.erase(std::remove_if(g_pictures.begin(), g_pictures.end(), [](const Picture& picture) { return !picture_here(picture); }), g_pictures.end());
    std::array<Prompt, kButtons> prompts;
    static const settings::LiveFlag icons("game.button_icons", "WP_BUTTON_ICONS");
    bool original = real_wiimote::present(0) || !icons();
    bool pad = input::gamepad_in_use();
    std::string key = original ? "wii" : pad ? "pad" : "keys";
    for (int i = 0; i < kButtons && !original; i++) {
        prompts[i] = pad ? pad_prompt(i) : key_prompt(i);
        key += describe(prompts[i]);
    }
    if (key == g_drawn) {
        return;
    }
    g_drawn = key;
    g_prompts = prompts;
    g_prompts_ready = true;
    for (Font& font : g_fonts) {
        draw_font(font, prompts);
    }
    for (const Picture& picture : g_pictures) {
        draw_picture(picture, prompts[kTwo]);
    }
}

void texture_bind(Cpu& c, HleFunction original) {
    uint32_t tpl = c.r[3];
    original(c);
    if (!tpl || rd32(tpl) != kTplMagic || rd32(tpl + 4) != 1) {
        return;
    }
    uint32_t header = rd32(rd32(tpl + 8));
    uint32_t palette = rd32(rd32(tpl + 8) + 4);
    if (!header || !palette || rd32(header + 4) != kFormatCi8 || rd32(palette + 4) != kPaletteRgb5a3) {
        return;
    }
    const PictureKind* kind = nullptr;
    for (const PictureKind& candidate : kPictureKinds) {
        if (rd16(header + 2) == candidate.width && rd16(header) == candidate.height && rd16(palette) == candidate.colours) {
            kind = &candidate;
        }
    }
    if (!kind) {
        return;
    }
    g_pictures.erase(std::remove_if(g_pictures.begin(), g_pictures.end(), [tpl](const Picture& picture) { return picture.tpl == tpl || !picture_here(picture); }),
                     g_pictures.end());
    if (static_cast<int>(g_pictures.size()) >= kMaxPictures) {
        return;
    }
    Picture picture;
    picture.kind = kind;
    picture.tpl = tpl;
    picture.header = header;
    picture.image = rd32(header + 8);
    uint32_t data = rd32(palette + 8);
    for (int i = 0; i < kind->colours; i++) {
        uint16_t value = rd16(data + 2 * static_cast<uint32_t>(i));
        if (value & 0x8000) {
            picture.colours.push_back({(value >> 10 & 31) * 255 / 31, (value >> 5 & 31) * 255 / 31, (value & 31) * 255 / 31, 255});
        } else {
            picture.colours.push_back({(value >> 8 & 15) * 17, (value >> 4 & 15) * 17, (value & 15) * 17, (value >> 12 & 7) * 255 / 7});
        }
    }
    picture.original.resize(static_cast<size_t>(kind->width) * kind->height);
    for (int y = 0; y < kind->height; y++) {
        for (int x = 0; x < kind->width; x++) {
            uint8_t index = rd8(ci8_byte(picture, x, y));
            picture.original[static_cast<size_t>(y) * kind->width + x] = index < kind->colours ? index : 0;
        }
    }
    if (!find_spot(picture)) {
        return;
    }
    log::write("game", "button picture %08x (%dx%d): disc at %.1f, %.1f, radius %.1f", tpl, kind->width, kind->height, picture.cx, picture.cy, picture.ring);
    if (g_prompts_ready) {
        draw_picture(picture, g_prompts[kTwo]);
    }
    g_pictures.push_back(std::move(picture));
    if (!g_prompts_ready) {
        g_drawn.clear();
        update_prompts();
    }
}

void font_resource(Cpu& c, HleFunction original) {
    uint32_t address = c.r[4];
    original(c);
    if (!address || (rd32(address) != kFontPacked && rd32(address) != kFontReady)) {
        return;
    }
    for (const Font& font : g_fonts) {
        if (font.address == address) {
            return;
        }
    }
    if (static_cast<int>(g_fonts.size()) >= kMaxFonts) {
        return;
    }
    uint32_t end = address + rd32(address + 8);
    uint32_t block = address + rd16(address + 0xc);
    while (block + 8 <= end && rd32(block) != kGlyphBlock) {
        if (rd32(block + 4) < 8) {
            return;
        }
        block += rd32(block + 4);
    }
    if (block + 0x20 > end) {
        return;
    }
    Font font;
    font.address = address;
    font.glyph_block = block;
    font.cell_width = rd8(block + 8);
    font.cell_height = rd8(block + 9);
    font.baseline = static_cast<int8_t>(rd8(block + 10));
    font.sheet_size = rd32(block + 12);
    font.per_row = rd16(block + 20);
    font.per_sheet = rd16(block + 20) * rd16(block + 22);
    font.sheet_width = rd16(block + 24);
    uint32_t sheets = rd32(block + 28);
    font.sheets = sheets >= 0x80000000u ? sheets : address + sheets;
    if (rd16(block + 18) != kFormatI4 || font.cell_width * font.cell_height > kMaxCell || font.per_sheet == 0 || font.sheet_width < 8) {
        return;
    }
    bool any = false;
    for (int i = 0; i < kButtons; i++) {
        font.glyph[i] = glyph_of(address, static_cast<uint16_t>(kFirstButton + i));
        font.width[i] = font.cell_width;
        if (font.glyph[i] >= 0) {
            any = true;
            int width = drawn_width(address, font.glyph[i]);
            if (width > 0 && width < font.cell_width) {
                font.width[i] = width;
            }
        }
    }
    if (!any) {
        return;
    }
    log::write("game", "font %08x: cells %dx%d, baseline %d, button glyphs from %d, %d wide", address, font.cell_width, font.cell_height, font.baseline, font.glyph[0],
               font.width[1]);
    g_fonts.push_back(std::move(font));
    g_drawn.clear();
    update_prompts();
}

}
