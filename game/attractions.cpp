#include "attractions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "wp/attraction.h"
#include "wp/canvas.h"
#include "wp/log.h"
#include "wp/memory.h"

namespace wp::game {

namespace {

constexpr uint32_t kSceneManagerGet = 0x80074a90;
constexpr uint32_t kEndScene = 0x802076a0;
constexpr uint32_t kAddSceneParameter = 0x80207730;
constexpr uint32_t kAttractionIdName = 0x8045394c;
constexpr uint32_t kNextSceneParameters = 0xac;

enum Scene : int {
    kScenePark = 0,
    kSceneAttraction = 4,
};

constexpr attraction::Mode kAttractionModes[] = {attraction::Mode::Aim, attraction::Mode::Dance, attraction::Mode::Shake, attraction::Mode::Fly};
constexpr const char* kAttractionNames[] = {"Cake Contraption", "Dance Inferno", "Power Bash", "Flight of Fancy"};

std::atomic<int> g_scene{-1};
std::atomic<int> g_attraction{-1};

void update_mode() {
    int scene = g_scene;
    int attraction = g_attraction;
    attraction::Mode mode = attraction::Mode::None;
    if (scene == kSceneAttraction && attraction >= 0 && attraction < 4) {
        mode = kAttractionModes[attraction];
    }
    attraction::set_mode(mode);
}

bool guest_text_is(uint32_t address, const char* text) {
    size_t length = std::strlen(text);
    for (size_t i = 0; i <= length; i++) {
        if (rd8(address + static_cast<uint32_t>(i)) != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

int g_test_request = 0;

void read_test_request() {
    static bool done = false;
    static std::chrono::steady_clock::time_point first{};
    const char* setting = std::getenv("WP_ATTRACTION");
    if (done || !setting || !*setting) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (first == std::chrono::steady_clock::time_point{}) {
        first = now;
    }
    const char* comma = std::strchr(setting, ',');
    double wait = comma ? std::atof(comma + 1) : 0.0;
    if (std::chrono::duration<double>(now - first).count() >= wait) {
        done = true;
        g_test_request = std::atoi(setting);
    }
}

}

void pick_scene(Cpu& c, HleFunction original) {
    int scene = static_cast<int>(c.r[3]);
    if (scene != g_scene) {
        log::write("game", "scene %d", scene);
    }
    g_scene = scene;
    update_mode();
    original(c);
}

void add_scene_parameter(Cpu& c, HleFunction original) {
    if (guest_text_is(c.r[4], "AttractionId")) {
        int attraction = static_cast<int>(c.r[5]);
        g_attraction = attraction;
        log::write("game", "attraction %d (%s)", attraction, attraction >= 0 && attraction < 4 ? kAttractionNames[attraction] : "unknown");
        update_mode();
    }
    original(c);
}

void scene_manager_run_begin(Cpu& c, HleFunction original) {
    original(c);
    if (g_scene != kScenePark) {
        return;
    }
    read_test_request();
    int request = g_test_request;
    if (request < 1 || request > 4) {
        return;
    }
    g_test_request = 0;
    log::write("game", "test: entering attraction %d", request);
    call(c, kSceneManagerGet);
    c.r[3] += kNextSceneParameters;
    c.r[4] = kAttractionIdName;
    c.r[5] = static_cast<uint32_t>(request - 1);
    call(c, kAddSceneParameter);
    call(c, kSceneManagerGet);
    call(c, kEndScene);
}

namespace {

constexpr uint32_t kDanceSlotTicks = 0x1f54;
constexpr uint32_t kChartCues = 0xbcc;
constexpr int kChartSlots = 151;
constexpr uint32_t kCueBeat = 0x1f;
constexpr uint32_t kCueHeld = 0;
constexpr uint32_t kCueNone = 0xff;
constexpr uint32_t kSwingReports = 6;

struct DanceMove {
    uint32_t cue;
    int icon;
    uint32_t move;
};

constexpr DanceMove kMoves[] = {
    {0x10, 0, attraction::kMoveLeft}, {0x12, 2, attraction::kMoveUp},   {0x16, 1, attraction::kMoveDown}, {0x14, 7, attraction::kMoveRight},
    {0x13, 3, attraction::kMoveKick}, {0x20, 4, attraction::kMovePose}, {0x21, 5, attraction::kMovePose}, {0x22, 6, attraction::kMovePose},
};
constexpr int kMoveCount = static_cast<int>(sizeof kMoves / sizeof kMoves[0]);

struct DancePart {
    uint32_t tick;
    uint32_t chart;
};

constexpr DancePart kParts[2] = {{0x1f4c, 0x1f04}, {0x1f50, 0x1f08}};

std::atomic<uint32_t> g_dance{0};
std::atomic<uint32_t> g_dance_vtable{0};
std::atomic<int> g_dance_part{0};
std::atomic<int64_t> g_dance_seen{0};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Song {
    int32_t tick = 0;
    int32_t slot_ticks = 0;
    uint32_t cues = 0;
};

bool song(Song& current) {
    uint32_t dance = g_dance;
    if (!dance || now_ms() - g_dance_seen > 250 || rd32(dance) != g_dance_vtable) {
        return false;
    }
    const DancePart& part = kParts[g_dance_part];
    current.tick = static_cast<int32_t>(rd32(dance + part.tick));
    current.slot_ticks = static_cast<int32_t>(rd32(dance + kDanceSlotTicks));
    uint32_t chart = rd32(dance + part.chart);
    if (current.slot_ticks <= 0 || current.slot_ticks > 1000 || current.tick < -1000 || current.tick > 1000000 || chart < 0x80000000u) {
        return false;
    }
    current.cues = chart + kChartCues;
    return true;
}

int move_of(uint32_t cue) {
    for (int i = 0; i < kMoveCount; i++) {
        if (kMoves[i].cue == cue) {
            return i;
        }
    }
    return -1;
}

uint32_t cue_at(uint32_t cues, int slot) {
    return slot >= 0 && slot < kChartSlots ? rd32(cues + 4 * static_cast<uint32_t>(slot)) : kCueNone;
}

int move_at(uint32_t cues, int slot) {
    for (; slot >= 0; slot--) {
        uint32_t cue = cue_at(cues, slot);
        if (cue != kCueBeat && cue != kCueHeld) {
            return move_of(cue);
        }
    }
    return -1;
}

bool is_cue(uint32_t cue) {
    return cue == kCueBeat || move_of(cue) >= 0;
}

int nearest_cue(uint32_t cues, int32_t tick, int32_t slot_ticks) {
    int slot = tick / slot_ticks;
    int best = -1;
    int32_t best_distance = slot_ticks + 1;
    for (int i = slot - 1; i <= slot + 2; i++) {
        int32_t distance = std::abs(tick - i * slot_ticks);
        if (is_cue(cue_at(cues, i)) && distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

uint32_t automatic_moves(const Song& current) {
    static int last_slot = -1;
    int slot = (current.tick + 2) / current.slot_ticks;
    uint32_t cue = cue_at(current.cues, slot);
    int move = move_at(current.cues, slot);
    if (cue == kCueNone || move < 0) {
        return 0;
    }
    if (cue != kCueBeat && cue != kCueHeld && slot != last_slot) {
        last_slot = slot;
        return 0;
    }
    return kMoves[move].move;
}

bool dance_playing() {
    Song current;
    return song(current);
}

int dance_swing(uint32_t held, uint32_t report) {
    static uint32_t last = 0;
    static uint32_t swing_end = 0;
    static uint32_t holding = 0;
    static int sign = 1;
    static int hold_move = -1;
    static int done = -1;
    static int part = -1;
    static int32_t last_tick = 0;
    Song current;
    bool playing = song(current);
    if (playing && (g_dance_part != part || current.tick < last_tick)) {
        part = g_dance_part;
        done = -1;
        holding = 0;
    }
    if (playing) {
        last_tick = current.tick;
        static const bool automatic = std::getenv("WP_DANCE_AUTO") != nullptr;
        if (automatic) {
            held = automatic_moves(current);
        }
    }
    uint32_t pressed = held & ~last;
    last = held;
    bool swing = false;
    if (!playing) {
        holding = 0;
        swing = pressed != 0;
    } else {
        if (pressed) {
            int slot = nearest_cue(current.cues, current.tick, current.slot_ticks);
            int move = slot >= 0 ? move_at(current.cues, slot) : -1;
            holding = 0;
            if (move >= 0 && (pressed & kMoves[move].move)) {
                swing = true;
                done = slot;
                if (kMoves[move].move != attraction::kMovePose) {
                    holding = kMoves[move].move;
                    hold_move = move;
                }
            }
        }
        if (!(held & holding) || cue_at(current.cues, (current.tick + current.slot_ticks / 2) / current.slot_ticks) == kCueNone) {
            holding = 0;
        }
        if (holding && !swing) {
            int slot = (current.tick + 1) / current.slot_ticks;
            int32_t at = slot * current.slot_ticks;
            if (slot > done && current.tick + 1 >= at && is_cue(cue_at(current.cues, slot))) {
                if (move_at(current.cues, slot) == hold_move) {
                    swing = true;
                    done = slot;
                } else {
                    holding = 0;
                }
            }
        }
    }
    if (swing) {
        sign = -sign;
        swing_end = report + kSwingReports;
    }
    return static_cast<int32_t>(swing_end - report) > 0 ? sign : 0;
}

constexpr int kIconCount = 8;
constexpr int kIconSize = 160;
constexpr int kBadge = 58;
constexpr int kBadgeEdge = 5;
constexpr int kBadgeInset = 10;
constexpr int kBadgeWide = 104;
constexpr int kBadgeFont = 32;
constexpr uint32_t kArchiveMagic = 0x55aa382d;
constexpr uint32_t kTplMagic = 0x0020af30;
constexpr uint32_t kFormatCi8 = 9;
constexpr int64_t kSearchPeriodMs = 1000;

struct Icons {
    uint32_t dance = 0;
    uint32_t archive = 0;
    int64_t searched = 0;
    uint32_t tpl[kIconCount] = {};
    std::vector<uint8_t> original[kIconCount];
    std::string drawn;
};

Icons g_icons;

bool is_archive(uint32_t header) {
    return header && rd32(header) == kArchiveMagic && rd32(header + 4) == 0x20;
}

uint32_t find_archive() {
    struct Range {
        uint32_t base;
        uint32_t size;
    };
    constexpr Range kRam[] = {{0x90000000u, 0x04000000u}, {0x80000000u, 0x01800000u}};
    static const std::string kName = "At01UIDanceIcon00.tpl";
    for (const Range& range : kRam) {
        const char* start = reinterpret_cast<const char*>(host(range.base));
        const char* end = start + range.size;
        std::boyer_moore_horspool_searcher searcher(kName.begin(), kName.end());
        for (const char* at = std::search(start, end, searcher); at != end; at = std::search(at + 1, end, searcher)) {
            uint32_t name = range.base + static_cast<uint32_t>(at - start);
            if (at + kName.size() >= end || at[kName.size()] != 0) {
                continue;
            }
            for (uint32_t header = (name & ~3u) - 4; header > range.base && header + 0x40000 > name; header -= 4) {
                if (!is_archive(header)) {
                    continue;
                }
                uint32_t count = rd32(header + 0x28);
                uint32_t names = header + 0x20 + count * 12;
                if (count > 1 && count < 10000 && name >= names && name < header + 0x20 + rd32(header + 8)) {
                    return header;
                }
            }
        }
    }
    return 0;
}

uint32_t archive_file(uint32_t header, const char* name) {
    uint32_t count = rd32(header + 0x28);
    uint32_t names = header + 0x20 + count * 12;
    for (uint32_t i = 1; i < count; i++) {
        uint32_t node = header + 0x20 + i * 12;
        if (rd8(node) == 0 && guest_text_is(names + (rd32(node) & 0xffffff), name)) {
            return header + rd32(node + 4);
        }
    }
    return 0;
}

uint32_t tpl_at(uint32_t tpl, uint32_t value) {
    uint32_t address = value >= 0x80000000u ? value : tpl + value;
    return address >= tpl && address - tpl < 0x40000u ? address : 0;
}

struct Picture {
    uint32_t image = 0;
    int colours = 0;
    uint8_t rgba[256][4] = {};
};

bool picture(uint32_t tpl, Picture& result) {
    if (rd32(tpl) != kTplMagic || rd32(tpl + 4) != 1) {
        return false;
    }
    uint32_t table = tpl_at(tpl, rd32(tpl + 8));
    uint32_t image = table ? tpl_at(tpl, rd32(table)) : 0;
    uint32_t palette = table ? tpl_at(tpl, rd32(table + 4)) : 0;
    if (!image || !palette || rd16(image) != kIconSize || rd16(image + 2) != kIconSize || rd32(image + 4) != kFormatCi8) {
        return false;
    }
    result.image = tpl_at(tpl, rd32(image + 8));
    uint32_t colours = tpl_at(tpl, rd32(palette + 8));
    if (!result.image || !colours) {
        return false;
    }
    result.colours = std::min<int>(rd16(palette), 256);
    uint32_t format = rd32(palette + 4);
    for (int i = 0; i < result.colours; i++) {
        uint32_t v = rd16(colours + 2 * static_cast<uint32_t>(i));
        uint8_t* c = result.rgba[i];
        if (format == 2 && (v & 0x8000)) {
            c[0] = static_cast<uint8_t>((v >> 10 & 31) * 255 / 31);
            c[1] = static_cast<uint8_t>((v >> 5 & 31) * 255 / 31);
            c[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
            c[3] = 255;
        } else if (format == 2) {
            c[0] = static_cast<uint8_t>((v >> 8 & 15) * 17);
            c[1] = static_cast<uint8_t>((v >> 4 & 15) * 17);
            c[2] = static_cast<uint8_t>((v & 15) * 17);
            c[3] = static_cast<uint8_t>((v >> 12 & 7) * 255 / 7);
        } else if (format == 1) {
            c[0] = static_cast<uint8_t>((v >> 11 & 31) * 255 / 31);
            c[1] = static_cast<uint8_t>((v >> 5 & 63) * 255 / 63);
            c[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
            c[3] = 255;
        } else {
            c[0] = c[1] = c[2] = static_cast<uint8_t>(v);
            c[3] = static_cast<uint8_t>(v >> 8);
        }
    }
    return result.colours > 0;
}

uint32_t ci8_byte(uint32_t image, int x, int y) {
    return image + static_cast<uint32_t>(((y / 4) * (kIconSize / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8);
}

int nearest_colour(const Picture& picture, uint32_t argb) {
    int r = argb >> 16 & 0xff;
    int g = argb >> 8 & 0xff;
    int b = argb & 0xff;
    int best = 0;
    long best_distance = -1;
    for (int i = 0; i < picture.colours; i++) {
        const uint8_t* c = picture.rgba[i];
        long da = (255 - c[3]) * 2;
        long dr = c[0] - r;
        long dg = c[1] - g;
        long db = c[2] - b;
        long distance = da * da + dr * dr + dg * dg + db * db;
        if (best_distance < 0 || distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

uint32_t darkest_colour(const Picture& picture) {
    int best = 0;
    int darkness = -1;
    for (int i = 0; i < picture.colours; i++) {
        const uint8_t* c = picture.rgba[i];
        int value = c[3] == 255 ? 765 - c[0] - c[1] - c[2] : -1;
        if (value > darkness) {
            darkness = value;
            best = i;
        }
    }
    const uint8_t* c = picture.rgba[best];
    return 0xff000000u | static_cast<uint32_t>(c[0]) << 16 | static_cast<uint32_t>(c[1]) << 8 | c[2];
}

void draw_arrow(canvas::Canvas& badge, attraction::Arrow arrow, uint32_t colour) {
    float centre = kBadge / 2.0f;
    float half = kBadge / 2.0f - kBadgeEdge - 8.0f;
    std::vector<std::array<float, 2>> shape = {{-half, 0.0f},         {0.0f, -half},        {0.0f, -half * 0.45f}, {half, -half * 0.45f},
                                                {half, half * 0.45f}, {0.0f, half * 0.45f}, {0.0f, half}};
    for (std::array<float, 2>& point : shape) {
        float x = point[0];
        float y = point[1];
        if (arrow == attraction::Arrow::Right) {
            x = -x;
        } else if (arrow == attraction::Arrow::Up) {
            std::swap(x, y);
        } else if (arrow == attraction::Arrow::Down) {
            std::swap(x, y);
            y = -y;
        }
        point = {centre + x, centre + y};
    }
    badge.fill_polygon(shape, colour);
}

void draw_badge(const Picture& picture, const attraction::Label& label) {
    uint32_t dark = darkest_colour(picture);
    int padding = kBadgeEdge + 4;
    int size = kBadgeFont;
    canvas::Face face = canvas::make_face(canvas::FontId::Heavy, size);
    int width = kBadge;
    if (label.arrow == attraction::Arrow::None) {
        int wanted = static_cast<int>(canvas::text_width(face, label.text)) + 2 * padding;
        width = std::clamp(wanted, kBadge, kBadgeWide);
        while (size > 12 && canvas::text_width(face, label.text) > width - 2 * padding) {
            face = canvas::make_face(canvas::FontId::Heavy, --size);
        }
    }
    canvas::Canvas badge;
    badge.width = width;
    badge.height = kBadge;
    badge.pixels.assign(static_cast<size_t>(width) * kBadge, 0);
    badge.fill_round(0, 0, width, kBadge, 14, dark);
    badge.fill_round(kBadgeEdge, kBadgeEdge, width - kBadgeEdge, kBadge - kBadgeEdge, 10, 0xffffffffu);
    if (label.arrow != attraction::Arrow::None) {
        draw_arrow(badge, label.arrow, dark);
    } else {
        badge.text(face, dark, label.text, padding, kBadgeEdge, width - padding, kBadge - kBadgeEdge, canvas::Align::Center, true);
    }
    int left = kIconSize - width - kBadgeInset;
    int top = kIconSize - kBadge - kBadgeInset;
    for (int y = 0; y < kBadge; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = badge.pixels[static_cast<size_t>(y) * width + x];
            if ((pixel >> 24) >= 128) {
                wr8(ci8_byte(picture.image, left + x, top + y), static_cast<uint8_t>(nearest_colour(picture, pixel)));
            }
        }
    }
}

void forget_icons() {
    g_icons.archive = 0;
    for (int i = 0; i < kIconCount; i++) {
        g_icons.tpl[i] = 0;
        g_icons.original[i].clear();
    }
    g_icons.drawn.clear();
}

void label_icons(uint32_t dance) {
    if (dance != g_icons.dance) {
        g_icons.dance = dance;
        forget_icons();
    }
    if (!is_archive(g_icons.archive) || !g_icons.tpl[0] || rd32(g_icons.tpl[0]) != kTplMagic) {
        int64_t now = now_ms();
        if (now - g_icons.searched < kSearchPeriodMs) {
            return;
        }
        g_icons.searched = now;
        forget_icons();
        g_icons.archive = find_archive();
        if (!g_icons.archive) {
            return;
        }
        for (int i = 0; i < kIconCount; i++) {
            char name[32];
            std::snprintf(name, sizeof name, "At01UIDanceIcon%02d.tpl", i);
            g_icons.tpl[i] = archive_file(g_icons.archive, name);
        }
        log::write("game", "dance icons at %08x", g_icons.archive);
    }
    attraction::Label labels[kMoveCount];
    std::string key;
    for (int m = 0; m < kMoveCount; m++) {
        labels[m] = attraction::move_label(kMoves[m].move);
        key += std::to_string(static_cast<int>(labels[m].arrow)) + labels[m].text + '\n';
    }
    if (key == g_icons.drawn) {
        return;
    }
    g_icons.drawn = key;
    for (int m = 0; m < kMoveCount; m++) {
        int icon = kMoves[m].icon;
        Picture current;
        if (!g_icons.tpl[icon] || !picture(g_icons.tpl[icon], current)) {
            continue;
        }
        std::vector<uint8_t>& original = g_icons.original[icon];
        uint8_t* pixels = host(current.image);
        if (original.empty()) {
            original.assign(pixels, pixels + kIconSize * kIconSize);
        }
        std::memcpy(pixels, original.data(), original.size());
        if (labels[m].arrow != attraction::Arrow::None || !labels[m].text.empty()) {
            draw_badge(current, labels[m]);
        }
    }
}

void note_dance(Cpu& c, int part) {
    static bool registered = false;
    if (!registered) {
        registered = true;
        attraction::set_dance(dance_swing, dance_playing);
    }
    uint32_t dance = c.r[3];
    if (dance != g_dance) {
        log::write("game", "dance at %08x, part %d, %u ticks a slot", dance, part + 1, rd32(dance + kDanceSlotTicks));
    }
    g_dance = dance;
    g_dance_vtable = rd32(dance);
    g_dance_part = part;
    g_dance_seen = now_ms();
    label_icons(dance);
}

}

void dance_frame(Cpu& c, HleFunction original) {
    note_dance(c, 0);
    original(c);
}

void dance_frame_second_part(Cpu& c, HleFunction original) {
    note_dance(c, 1);
    original(c);
}

}
