#include "modifiers.h"

#include "puppets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wp/disc.h"
#include "wp/ghosts.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/online.h"
#include "wp/settings.h"

namespace wp::game {

namespace {

constexpr uint32_t kGlobalManagerGet = 0x80074c10;
constexpr uint32_t kSaveData = 0x48;
constexpr uint32_t kSaveBerries = 0x1474;
constexpr int32_t kMaxBerries = 99999;
constexpr uint32_t kTopSpeed = 0x30;
constexpr std::array<uint32_t, 5> kSpeedFields = {0x28, 0x2c, 0x30, 0x38, 0x3c};
constexpr int kChasedFrames = 4;
constexpr uint32_t kPlayerTable = 0x8044bb9c;
constexpr uint32_t kPlayerBody = 0x9c;
constexpr uint32_t kBodyPosition = 0x8;
constexpr uint32_t kPlayerAccessor = 0xc;
constexpr uint32_t kAccessorLife = 0x60;
constexpr uint32_t kMoveFrame = 0x20;
constexpr float kLongestStep = 3.0f;
constexpr uint32_t kCameraShownEye = 0x18;
constexpr uint32_t kCameraTarget = 0x28;
constexpr uint32_t kCameraView = 0x48;
constexpr float kMarkerHeight = 5.0f;

uint32_t g_player = 0;
uint32_t g_field_camera = 0;
int g_frames_since_move = 1000;
constexpr int kPlayerActiveFrames = 4;
constexpr uint32_t kBodyRotation = 0x14;
constexpr uint32_t kPlayerCharacter = 0xa;
constexpr uint32_t kCameraFollowed = 0xb8;
constexpr uint32_t kObjectHandle = 0x4;
uint32_t g_controlled_accessor = 0;
constexpr uint32_t kWholeBody = 0xffffffff;
constexpr uint32_t kLongestMotion = 16;
std::string g_motion;
uint32_t g_motion_flags = 0;
uint32_t g_motion_serial = 0;
constexpr uint32_t kSoundSlot = 0x38;
constexpr uint32_t kEffectSlot = 0x3c;
constexpr uint32_t kSoundOwner = 0x8;
constexpr int kEventFrames = 20;
uint32_t g_sound_module = 0;
uint32_t g_effect_module = 0;
std::array<float, 3> g_controlled_position{};
uint32_t g_event_serial = 0;
int g_event_frame = 0;

struct RecentEvent {
    int frame = 0;
    presence::Event event;
};

std::vector<RecentEvent> g_recent;
std::vector<uint32_t> g_playing;
constexpr uint32_t kSoundIsPlaying = 0x800be650;
constexpr uint32_t kSoundFade = 10;
constexpr size_t kMostPlaying = 24;

uint32_t module_at(uint32_t accessor, uint32_t slot) {
    return rd32(accessor + (rd32(rd32(rd32(accessor) + slot)) & 0xffff));
}

std::array<float, 3> vector_at(uint32_t address) {
    return {rdf32(address), rdf32(address + 4), rdf32(address + 8)};
}

void record(presence::Event event) {
    event.serial = ++g_event_serial;
    g_recent.push_back({g_event_frame, event});
}

std::array<float, 3> from_player(const std::array<float, 3>& point) {
    return {point[0] - g_controlled_position[0], point[1] - g_controlled_position[1], point[2] - g_controlled_position[2]};
}

struct ShownPlayer {
    bool valid = false;
    float distance = 0.0f;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    uint32_t character = 0;
};

ShownPlayer g_shown;

struct Chased {
    uint32_t accessor = 0;
    int age = 0;
};

std::array<Chased, 4> g_chased;

bool chased(uint32_t accessor) {
    for (Chased& entry : g_chased) {
        if (entry.accessor != accessor) {
            continue;
        }
        if (++entry.age > kChasedFrames) {
            entry.accessor = 0;
            return false;
        }
        return true;
    }
    return false;
}

}

void publish_player() {
    if (!g_shown.valid || !online::running()) {
        g_shown.valid = false;
        return;
    }
    uint32_t zone = disc::current_field();
    g_event_frame++;
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(), [](const RecentEvent& recent) { return g_event_frame - recent.frame > kEventFrames; }), g_recent.end());
    if (g_recent.size() > presence::kMostEvents) {
        g_recent.erase(g_recent.begin(), g_recent.end() - presence::kMostEvents);
    }
    std::vector<presence::Event> events;
    for (const RecentEvent& recent : g_recent) {
        events.push_back(recent.event);
    }
    online::update(g_shown.position, zone, {g_shown.rotation, g_shown.character, g_motion, g_motion_flags, g_motion_serial, std::move(events)});
    g_shown.valid = false;
    if (!g_field_camera) {
        return;
    }
    std::vector<ghosts::Marker> markers;
    for (const presence::Peer& peer : online::peers()) {
        if (peer.zone != 0 && peer.zone == zone) {
            markers.push_back({{peer.position[0], peer.position[1] + kMarkerHeight, peer.position[2]}, peer.name});
        }
    }
    ghosts::View view;
    for (uint32_t i = 0; i < view.size(); i++) {
        view[i] = rdf32(g_field_camera + kCameraView + 4 * i);
    }
    ghosts::set_view(view, std::move(markers));
}

void report_ended_sounds(Cpu& c) {
    if (g_playing.empty() || !g_sound_module || !online::running()) {
        g_playing.clear();
        return;
    }
    Cpu saved = c;
    uint32_t stack = c.r[1];
    c.r[1] = stack - kMoveFrame;
    wr32(c.r[1], stack);
    for (auto id = g_playing.begin(); id != g_playing.end();) {
        c.r[3] = g_sound_module;
        c.r[4] = *id;
        call(c, kSoundIsPlaying);
        if (c.r[3] != 0) {
            ++id;
            continue;
        }
        presence::Event event;
        event.kind = 4;
        event.id = *id;
        event.b = kSoundFade;
        record(event);
        id = g_playing.erase(id);
    }
    std::memcpy(c.r, saved.r, sizeof c.r);
    std::memcpy(c.f, saved.f, sizeof c.f);
    std::memcpy(c.ps1, saved.ps1, sizeof c.ps1);
    std::memcpy(c.cr, saved.cr, sizeof c.cr);
    c.lr = saved.lr;
    c.ctr = saved.ctr;
}

void modifiers_frame(Cpu& c, bool park) {
    report_ended_sounds(c);
    publish_player();
    puppets_frame(c, park, g_player && rd32(g_player) == kPlayerTable && g_frames_since_move < kPlayerActiveFrames);
    g_frames_since_move++;
    static const settings::LiveNumber gained("cheats.berries", "WP_CHEAT_BERRIES");
    static const settings::LiveNumber prices("cheats.prices", "WP_CHEAT_PRICES");
    static bool have = false;
    static int32_t last = 0;
    if (!park) {
        have = false;
        return;
    }
    uint32_t saved = c.r[3];
    call(c, kGlobalManagerGet);
    uint32_t manager = c.r[3];
    c.r[3] = saved;
    uint32_t data = manager ? rd32(manager + kSaveData) : 0;
    if (!data) {
        have = false;
        return;
    }
    uint32_t address = data + kSaveBerries;
    int32_t now = static_cast<int32_t>(rd32(address));
    if (!have) {
        have = true;
        last = now;
        return;
    }
    int32_t change = now - last;
    if (change == 0) {
        return;
    }
    int percent = change > 0 ? gained() : prices();
    int32_t scaled = static_cast<int32_t>(static_cast<int64_t>(change) * percent / 100);
    last = std::clamp(last + scaled, 0, kMaxBerries);
    if (scaled != change) {
        log::write("cheats", "berries %+d become %+d", change, scaled);
        wr32(address, static_cast<uint32_t>(last));
    }
}

void chase_step(Cpu& c, HleFunction original) {
    uint32_t accessor = c.r[4];
    Chased* slot = &g_chased[0];
    for (Chased& entry : g_chased) {
        if (entry.accessor == accessor) {
            slot = &entry;
            break;
        }
        if (slot->accessor != 0 && (entry.accessor == 0 || entry.age > slot->age)) {
            slot = &entry;
        }
    }
    if (slot->accessor != accessor) {
        log::write("cheats", "accessor %08x is being chased", accessor);
    }
    slot->accessor = accessor;
    slot->age = 0;
    original(c);
}

void note_field_camera(uint32_t camera) {
    g_field_camera = camera;
}

void player_move(Cpu& c, HleFunction original) {
    static const settings::LiveNumber speed("cheats.run_speed", "WP_CHEAT_RUN_SPEED");
    static const settings::LiveNumber jump("cheats.jump", "WP_CHEAT_JUMP");
    static uint32_t last_player = 0;
    static std::array<float, 3> last{};
    uint32_t player = c.r[3];
    uint32_t body = rd32(player) == kPlayerTable ? rd32(player + kPlayerBody) : 0;
    if (!body) {
        original(c);
        return;
    }
    g_player = player;
    g_frames_since_move = 0;
    uint32_t now = body + kBodyPosition;
    static const bool from_environment = (online::start_from_environment(), true);
    (void)from_environment;
    if (online::running()) {
        float distance = 0.0f;
        bool followed = g_field_camera && rd32(g_field_camera + kCameraFollowed) == rd32(player + kObjectHandle);
        if (followed) {
            distance = -1.0f;
            g_controlled_accessor = rd32(player + kPlayerAccessor);
            g_sound_module = module_at(g_controlled_accessor, kSoundSlot);
            g_effect_module = module_at(g_controlled_accessor, kEffectSlot);
            g_controlled_position = vector_at(now);
        } else if (g_field_camera) {
            uint32_t target = g_field_camera + kCameraTarget;
            for (uint32_t i = 0; i < 3; i++) {
                float gap = rdf32(now + 4 * i) - rdf32(target + 4 * i);
                distance += gap * gap;
            }
        }
        if (!g_shown.valid || distance < g_shown.distance) {
            uint32_t turned = body + kBodyRotation;
            g_shown = {true, distance, {rdf32(now), rdf32(now + 4), rdf32(now + 8)}, {rdf32(turned), rdf32(turned + 4), rdf32(turned + 8)}, rd16(player + kPlayerCharacter)};
        }
    }
    int run = speed();
    int rise = jump();
    uint32_t wanted = c.r[4];
    std::array<float, 3> step{};
    for (uint32_t i = 0; i < 3; i++) {
        step[i] = rdf32(wanted + 4 * i) - last[i];
    }
    bool scale = last_player == player && (run != 100 || rise != 100) && run > 0 && rise > 0 &&
                 step[0] * step[0] + step[2] * step[2] < kLongestStep * kLongestStep && std::fabs(step[1]) < kLongestStep;
    if (!scale) {
        original(c);
    } else {
        uint32_t stack = c.r[1];
        uint32_t frame = stack - kMoveFrame;
        wr32(frame, stack);
        uint32_t moved = frame + 0x10;
        for (uint32_t i = 0; i < 3; i++) {
            float k = i == 1 ? (step[i] > 0.0f ? rise / 100.0f : 1.0f) : run / 100.0f;
            wrf32(moved + 4 * i, last[i] + step[i] * k);
        }
        c.r[1] = frame;
        c.r[4] = moved;
        original(c);
        c.r[1] = stack;
    }
    for (uint32_t i = 0; i < 3; i++) {
        last[i] = rdf32(now + 4 * i);
    }
    last_player = player;
}

void motion_start(Cpu& c, HleFunction original) {
    if (c.r[4] == g_controlled_accessor && c.r[6] && c.r[8] == kWholeBody) {
        std::string name;
        for (uint32_t i = 0; i < kLongestMotion && rd8(c.r[6] + i); i++) {
            name.push_back(static_cast<char>(rd8(c.r[6] + i)));
        }
        g_motion = name;
        g_motion_flags = c.r[7];
        g_motion_serial++;
    }
    original(c);
}

void sound_play(Cpu& c, HleFunction original) {
    if (c.r[3] == g_sound_module && online::running()) {
        presence::Event event;
        event.kind = 0;
        event.id = c.r[5];
        event.f1 = static_cast<float>(c.f[1]);
        event.f2 = static_cast<float>(c.f[2]);
        record(event);
        if (std::find(g_playing.begin(), g_playing.end(), event.id) == g_playing.end() && g_playing.size() < kMostPlaying) {
            g_playing.push_back(event.id);
        }
    }
    original(c);
}

void sound_stop(Cpu& c, HleFunction original) {
    if (g_sound_module && c.r[5] == rd32(g_sound_module + kSoundOwner) && online::running()) {
        presence::Event event;
        event.kind = 4;
        event.id = c.r[4];
        event.b = c.r[6];
        record(event);
    }
    original(c);
}

void sound_stop_all(Cpu& c, HleFunction original) {
    if (g_sound_module && c.r[4] == rd32(g_sound_module + kSoundOwner) && online::running()) {
        presence::Event event;
        event.kind = 5;
        event.b = c.r[5];
        record(event);
    }
    original(c);
}

void effect_at(Cpu& c, HleFunction original) {
    if (c.r[3] == g_effect_module && online::running()) {
        presence::Event event;
        event.kind = 1;
        event.id = c.r[4];
        event.offset = from_player(vector_at(c.r[5]));
        event.vector = vector_at(c.r[6]);
        event.f1 = rdf32(c.r[7]);
        record(event);
    }
    original(c);
}

void note_effect_stop(Cpu& c, uint32_t slot) {
    if (c.r[3] == g_effect_module && online::running()) {
        presence::Event event;
        event.kind = 3;
        event.id = c.r[4];
        event.a = slot;
        event.b = c.r[5];
        record(event);
    }
}

void effect_on(Cpu& c, HleFunction original) {
    if (c.r[3] == g_effect_module && online::running()) {
        presence::Event event;
        event.kind = 2;
        event.id = c.r[4];
        event.a = c.r[6];
        event.b = c.r[7];
        event.c = c.r[8];
        event.f1 = static_cast<float>(c.f[1]);
        event.f2 = static_cast<float>(c.f[2]);
        record(event);
    }
    original(c);
}

void life_damage(Cpu& c, HleFunction original) {
    static const settings::LiveNumber dealt("cheats.damage_dealt", "WP_CHEAT_DAMAGE_DEALT");
    static const settings::LiveNumber taken("cheats.damage_taken", "WP_CHEAT_DAMAGE_TAKEN");
    bool hurt = g_player && rd32(g_player) == kPlayerTable && rd32(rd32(g_player + kPlayerAccessor) + kAccessorLife) == c.r[3];
    int percent = hurt ? taken() : dealt();
    if (percent != 100 && percent >= 0) {
        double damage = c.f[1];
        c.f[1] = static_cast<float>(damage * percent / 100.0);
        log::write("cheats", "damage %s %g becomes %g", hurt ? "taken" : "dealt", damage, c.f[1]);
    }
    original(c);
}

void kinetic_energy_update(Cpu& c, HleFunction original) {
    static const settings::LiveNumber speed("cheats.chase_speed", "WP_CHEAT_CHASE_SPEED");
    int percent = speed();
    uint32_t energy = c.r[3];
    if (percent == 100 || percent <= 0 || !chased(c.r[4])) {
        original(c);
        return;
    }
    float k = percent / 100.0f;
    std::array<uint32_t, kSpeedFields.size()> saved{};
    for (size_t i = 0; i < kSpeedFields.size(); i++) {
        uint32_t field = energy + kSpeedFields[i];
        saved[i] = rd32(field);
        if (kSpeedFields[i] != kTopSpeed || rdf32(field) > 0.0f) {
            wrf32(field, rdf32(field) * k);
        }
    }
    original(c);
    for (size_t i = 0; i < kSpeedFields.size(); i++) {
        wr32(energy + kSpeedFields[i], saved[i]);
    }
}

}
