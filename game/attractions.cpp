#include "attractions.h"
#include "prompts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
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
constexpr uint32_t kMovieName = 0x804b0360;
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

void label_bomb_button();

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
    update_prompts();
    if (attraction::mode() == attraction::Mode::Shake) {
        label_bomb_button();
    }
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
    const char* movie = std::getenv("WP_MOVIE");
    c.r[4] = movie ? kMovieName : kAttractionIdName;
    c.r[5] = movie ? static_cast<uint32_t>(std::atoi(movie)) : static_cast<uint32_t>(request - 1);
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
constexpr uint32_t kCuePose = 0x20;
constexpr uint32_t kDanceTickOffset = 0x5318;
constexpr uint32_t kDancePoseSwings = 0x5294;
constexpr uint32_t kDanceVerdicts = 0x52a4;
constexpr uint32_t kDanceGreatWindow = 0x1f34;
constexpr uint32_t kDancePoseWindow = 0x1f38;
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
constexpr size_t kSearchChunk = 4 << 20;

struct ArchiveSearch {
    size_t range = 0;
    uint32_t offset = 0;
    int64_t resume = 0;
};
constexpr const char* kBombButtonName = "AtAButton00.tpl";
constexpr int kBombDiscRadius = 22;
constexpr int kBombGlyphHalf = 16;
constexpr int kBombGlyphDarkness = 330;
constexpr int kBombFont = 34;
constexpr uint32_t kBombInkMinAlpha = 24;

struct Icons {
    uint32_t dance = 0;
    uint32_t archive = 0;
    ArchiveSearch search;
    uint32_t tpl[kIconCount] = {};
    std::vector<uint8_t> original[kIconCount];
    std::string drawn;
};

Icons g_icons;

bool is_archive(uint32_t header) {
    return header && rd32(header) == kArchiveMagic && rd32(header + 4) == 0x20;
}

struct RamRange {
    uint32_t base;
    uint32_t size;
};

constexpr RamRange kRam[] = {{0x90000000u, 0x04000000u}, {0x80000000u, 0x01800000u}};

uint32_t archive_holding(const RamRange& range, uint32_t name) {
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
    return 0;
}

uint32_t search_archive(ArchiveSearch& search, const std::string& wanted) {
    int64_t now = now_ms();
    if (now < search.resume) {
        return 0;
    }
    const RamRange& range = kRam[search.range];
    const char* start = reinterpret_cast<const char*>(host(range.base));
    const char* end = start + range.size;
    const char* from = start + search.offset;
    const char* to = std::min(end, from + kSearchChunk);
    const char* limit = std::min(end, to + wanted.size());
    std::boyer_moore_horspool_searcher searcher(wanted.begin(), wanted.end());
    for (const char* at = std::search(from, limit, searcher); at < to; at = std::search(at + 1, limit, searcher)) {
        if (at + wanted.size() >= end || at[wanted.size()] != 0) {
            continue;
        }
        if (uint32_t header = archive_holding(range, range.base + static_cast<uint32_t>(at - start))) {
            search = ArchiveSearch{};
            return header;
        }
    }
    search.offset = static_cast<uint32_t>(to - start);
    if (to == end) {
        search.offset = 0;
        search.range = (search.range + 1) % std::size(kRam);
        if (search.range == 0) {
            search.resume = now + kSearchPeriodMs;
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
    int width = 0;
    int height = 0;
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
    if (!image || !palette || rd32(image + 4) != kFormatCi8) {
        return false;
    }
    result.height = rd16(image);
    result.width = rd16(image + 2);
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

uint32_t ci8_byte(const Picture& picture, int x, int y) {
    return picture.image + static_cast<uint32_t>(((y / 4) * ((picture.width + 7) / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8);
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

std::vector<std::array<float, 2>> ring_points(float cx, float cy, float radius, int corners, float turn) {
    std::vector<std::array<float, 2>> points;
    for (int i = 0; i < corners; i++) {
        float angle = turn + 6.2831853f * static_cast<float>(i) / static_cast<float>(corners);
        points.push_back({cx + radius * std::cos(angle), cy + radius * std::sin(angle)});
    }
    return points;
}

void draw_shape(canvas::Canvas& target, attraction::Shape shape, int middle_x, int middle_y, uint32_t colour, uint32_t hollow) {
    float x = static_cast<float>(middle_x);
    float y = static_cast<float>(middle_y);
    if (shape == attraction::Shape::Cross) {
        target.cross(middle_x, middle_y, 12, 6, colour);
    } else if (shape == attraction::Shape::Circle) {
        target.fill_polygon(ring_points(x, y, 16.0f, 48, 0.0f), colour);
        target.fill_polygon(ring_points(x, y, 10.0f, 48, 0.0f), hollow);
    } else if (shape == attraction::Shape::Square) {
        target.fill(middle_x - 15, middle_y - 15, middle_x + 15, middle_y + 15, colour);
        target.fill(middle_x - 9, middle_y - 9, middle_x + 9, middle_y + 9, hollow);
    } else if (shape == attraction::Shape::Triangle) {
        target.fill_polygon(ring_points(x, y + 3.0f, 20.0f, 3, -1.5707963f), colour);
        target.fill_polygon(ring_points(x, y + 3.0f, 10.0f, 3, -1.5707963f), hollow);
    } else if (shape == attraction::Shape::SpaceBar) {
        target.fill_round(middle_x - 16, middle_y + 3, middle_x + 16, middle_y + 9, 3, colour);
        target.fill_round(middle_x - 16, middle_y - 7, middle_x - 10, middle_y + 9, 3, colour);
        target.fill_round(middle_x + 10, middle_y - 7, middle_x + 16, middle_y + 9, 3, colour);
    }
}

void draw_badge(const Picture& picture, const attraction::Label& label) {
    uint32_t dark = darkest_colour(picture);
    int padding = kBadgeEdge + 4;
    int size = kBadgeFont;
    canvas::Face face = canvas::make_face(canvas::FontId::Heavy, size);
    int width = kBadge;
    if (label.arrow == attraction::Arrow::None && label.shape == attraction::Shape::None) {
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
    } else if (label.shape != attraction::Shape::None) {
        draw_shape(badge, label.shape, kBadge / 2, kBadge / 2, dark, 0xffffffffu);
    } else {
        badge.text(face, dark, label.text, padding, kBadgeEdge, width - padding, kBadge - kBadgeEdge, canvas::Align::Center, true);
    }
    int left = kIconSize - width - kBadgeInset;
    int top = kIconSize - kBadge - kBadgeInset;
    for (int y = 0; y < kBadge; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = badge.pixels[static_cast<size_t>(y) * width + x];
            if ((pixel >> 24) >= 128) {
                wr8(ci8_byte(picture, left + x, top + y), static_cast<uint8_t>(nearest_colour(picture, pixel)));
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
        forget_icons();
        g_icons.archive = search_archive(g_icons.search, "At01UIDanceIcon00.tpl");
        if (!g_icons.archive) {
            return;
        }
        g_icons.search.resume = now_ms() + kSearchPeriodMs;
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
        key += std::to_string(static_cast<int>(labels[m].arrow)) + std::to_string(static_cast<int>(labels[m].shape)) + labels[m].text + '\n';
    }
    if (key == g_icons.drawn) {
        return;
    }
    g_icons.drawn = key;
    for (int m = 0; m < kMoveCount; m++) {
        int icon = kMoves[m].icon;
        Picture current;
        if (!g_icons.tpl[icon] || !picture(g_icons.tpl[icon], current) || current.width != kIconSize || current.height != kIconSize) {
            continue;
        }
        std::vector<uint8_t>& original = g_icons.original[icon];
        uint8_t* pixels = host(current.image);
        if (original.empty()) {
            original.assign(pixels, pixels + kIconSize * kIconSize);
        }
        std::memcpy(pixels, original.data(), original.size());
        if (labels[m].arrow != attraction::Arrow::None || labels[m].shape != attraction::Shape::None || !labels[m].text.empty()) {
            draw_badge(current, labels[m]);
        }
    }
}

struct BombButton {
    uint32_t archive = 0;
    uint32_t tpl = 0;
    ArchiveSearch search;
    std::vector<uint8_t> original;
    std::string drawn;
};

BombButton g_bomb_button;

bool dark_colour(const Picture& picture, uint8_t index) {
    const uint8_t* c = picture.rgba[index];
    return c[3] >= 128 && c[0] + c[1] + c[2] < kBombGlyphDarkness;
}

void draw_bomb_button(const Picture& picture, const attraction::Label& label) {
    int middle_x = picture.width / 2;
    int middle_y = picture.height / 2;
    int glyph = -1;
    int glyph_darkness = -1;
    for (int dy = -kBombGlyphHalf; dy <= kBombGlyphHalf; dy++) {
        int y = middle_y + dy;
        int reach = static_cast<int>(std::sqrt(static_cast<double>(kBombDiscRadius * kBombDiscRadius - dy * dy)));
        uint8_t fill = rd8(ci8_byte(picture, middle_x - reach + 2, y));
        for (int x = middle_x - kBombGlyphHalf; x <= middle_x + kBombGlyphHalf; x++) {
            uint32_t at = ci8_byte(picture, x, y);
            uint8_t index = rd8(at);
            if (dark_colour(picture, index)) {
                const uint8_t* c = picture.rgba[index];
                int darkness = 765 - c[0] - c[1] - c[2];
                if (darkness > glyph_darkness) {
                    glyph_darkness = darkness;
                    glyph = index;
                }
            }
            wr8(at, fill);
        }
    }
    if (glyph < 0) {
        return;
    }
    const uint8_t* g = picture.rgba[glyph];
    uint32_t ink = 0xff000000u | static_cast<uint32_t>(g[0]) << 16 | static_cast<uint32_t>(g[1]) << 8 | g[2];
    const uint8_t* h = picture.rgba[rd8(ci8_byte(picture, middle_x, middle_y))];
    uint32_t hollow = 0xff000000u | static_cast<uint32_t>(h[0]) << 16 | static_cast<uint32_t>(h[1]) << 8 | h[2];
    canvas::Canvas symbol;
    symbol.width = picture.width;
    symbol.height = picture.height;
    symbol.pixels.assign(static_cast<size_t>(picture.width) * picture.height, 0);
    if (label.shape != attraction::Shape::None) {
        draw_shape(symbol, label.shape, middle_x, middle_y, ink, hollow);
    } else {
        int room = 2 * kBombDiscRadius - 8;
        int size = kBombFont;
        canvas::Face face = canvas::make_face(canvas::FontId::Heavy, size);
        while (size > 10 && canvas::text_width(face, label.text) > room) {
            face = canvas::make_face(canvas::FontId::Heavy, --size);
        }
        symbol.text(face, ink, label.text, middle_x - room / 2, 0, middle_x + room / 2, picture.height, canvas::Align::Center, true);
    }
    for (int y = 0; y < picture.height; y++) {
        for (int x = 0; x < picture.width; x++) {
            uint32_t pixel = symbol.pixels[static_cast<size_t>(y) * picture.width + x];
            uint32_t alpha = pixel >> 24;
            if (alpha < kBombInkMinAlpha) {
                continue;
            }
            uint32_t at = ci8_byte(picture, x, y);
            const uint8_t* below = picture.rgba[rd8(at)];
            uint32_t mixed = 0xff000000u;
            for (int channel = 0; channel < 3; channel++) {
                uint32_t ink_part = pixel >> (16 - 8 * channel) & 0xff;
                uint32_t value = (ink_part * alpha + below[channel] * (255 - alpha) + 127) / 255;
                mixed |= value << (16 - 8 * channel);
            }
            wr8(at, static_cast<uint8_t>(nearest_colour(picture, mixed)));
        }
    }
}

void label_bomb_button() {
    BombButton& state = g_bomb_button;
    if (!is_archive(state.archive) || !state.tpl || rd32(state.tpl) != kTplMagic) {
        state.original.clear();
        state.drawn.clear();
        state.archive = search_archive(state.search, kBombButtonName);
        state.tpl = state.archive ? archive_file(state.archive, kBombButtonName) : 0;
        if (!state.tpl) {
            if (state.archive) {
                state.search.resume = now_ms() + kSearchPeriodMs;
            }
            return;
        }
        log::write("game", "bomb button at %08x", state.tpl);
    }
    attraction::Label label = attraction::move_label(attraction::kMovePose);
    std::string key = std::to_string(static_cast<int>(label.shape)) + label.text;
    if (key == state.drawn) {
        return;
    }
    Picture current;
    if (!picture(state.tpl, current) || current.width > 64 || current.height > 64) {
        return;
    }
    state.drawn = key;
    uint8_t* pixels = host(current.image);
    size_t bytes = static_cast<size_t>((current.width + 7) / 8) * ((current.height + 3) / 4) * 32;
    if (state.original.empty()) {
        state.original.assign(pixels, pixels + bytes);
    }
    std::memcpy(pixels, state.original.data(), state.original.size());
    if (label.shape != attraction::Shape::None || !label.text.empty()) {
        draw_bomb_button(current, label);
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
        log::write("game", "dance at %08x, part %d, %u ticks a slot, windows %d and %d", dance, part + 1, rd32(dance + kDanceSlotTicks),
                   static_cast<int32_t>(rd32(dance + kDanceGreatWindow)), static_cast<int32_t>(rd32(dance + kDancePoseWindow)));
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

void dance_judge(Cpu& c, HleFunction original) {
    static uint32_t late_dance = 0;
    static int32_t late_slot = -1;
    static uint32_t late_player = 0;
    uint32_t dance = c.r[3];
    uint32_t player = c.r[4];
    uint32_t cues = c.r[5];
    int32_t slot_ticks = static_cast<int32_t>(rd32(dance + kDanceSlotTicks));
    if (player >= 4 || slot_ticks <= 0 || slot_ticks > 1000) {
        original(c);
        return;
    }
    int32_t half = slot_ticks / 2;
    int32_t at = static_cast<int32_t>(c.r[8]) + half + static_cast<int32_t>(rd32(dance + kDanceTickOffset));
    int32_t slot = at >= 0 ? at / slot_ticks : -1;
    int32_t from_centre = at - slot * slot_ticks - half;
    uint32_t cue = slot >= 0 && slot < kChartSlots ? rd32(cues + 4 * static_cast<uint32_t>(slot)) : kCueNone;
    uint32_t swings = dance + kDancePoseSwings + 4 * player;
    uint32_t verdict = dance + kDanceVerdicts + 4 * player;
    bool pose = (cue & 0xf0) == kCuePose && rd32(verdict) == 0;
    if (pose && c.r[6] != 0 && from_centre >= 0 && from_centre < half - 1 && rd32(swings) == 0) {
        wr32(swings, 1);
        bool in_time = from_centre <= static_cast<int32_t>(rd32(dance + kDanceGreatWindow));
        late_dance = in_time ? dance : 0;
        late_slot = slot;
        late_player = player;
    }
    bool lenient = pose && dance == late_dance && slot == late_slot && player == late_player;
    uint32_t still_window = rd32(dance + kDancePoseWindow);
    if (lenient) {
        wr32(dance + kDancePoseWindow, static_cast<uint32_t>(half - 1));
    }
    original(c);
    if (lenient) {
        wr32(dance + kDancePoseWindow, still_window);
    }
}

}
