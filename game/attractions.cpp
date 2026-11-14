#include "attractions.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "wp/attraction.h"
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
