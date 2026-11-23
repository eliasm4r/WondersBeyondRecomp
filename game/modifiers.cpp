#include "modifiers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "wp/disc.h"
#include "wp/ghosts.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/presence.h"
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
constexpr float kMarkerHeight = 5.0f;

uint32_t g_player = 0;
uint32_t g_field_camera = 0;

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

void modifiers_frame(Cpu& c, bool park) {
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
    uint32_t now = body + kBodyPosition;
    static const char* const online = std::getenv("WP_ONLINE");
    if (online) {
        static presence::Link link;
        static const bool ready = [] {
            const char* given = std::getenv("WP_ONLINE_NAME");
            const std::string name = given ? given : "Player";
            const std::string text = online;
            size_t last = text.rfind(':');
            int port = last == std::string::npos ? 0 : std::atoi(text.c_str() + last + 1);
            bool ok = false;
            if (port > 0 && port < 65536) {
                if (text.rfind("host:", 0) == 0) {
                    ok = link.host(static_cast<uint16_t>(port), name);
                } else if (text.rfind("join:", 0) == 0 && last > 5) {
                    ok = link.join(text.substr(5, last - 5), static_cast<uint16_t>(port), name);
                }
            }
            log::write("online", ok ? "connected as %s (%s)" : "could not start for %s (%s)", name.c_str(), text.c_str());
            return ok;
        }();
        static uint32_t steps = 0;
        if (ready) {
            static const auto started = std::chrono::steady_clock::now();
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            link.pump(seconds);
            if (++steps % 2 == 0) {
                link.send({rdf32(now), rdf32(now + 4), rdf32(now + 8)}, disc::current_field());
            }
            if (g_field_camera) {
                std::vector<ghosts::Marker> markers;
                for (const presence::Peer& peer : link.peers()) {
                    if (peer.zone == disc::current_field()) {
                        markers.push_back({{peer.position[0], peer.position[1] + kMarkerHeight, peer.position[2]}, peer.name});
                    }
                }
                uint32_t eye = g_field_camera + kCameraShownEye;
                uint32_t target = g_field_camera + kCameraTarget;
                ghosts::set_view({rdf32(eye), rdf32(eye + 4), rdf32(eye + 8)}, {rdf32(target), rdf32(target + 4), rdf32(target + 8)}, std::move(markers));
            }
            if (steps % 60 == 0) {
                for (const presence::Peer& peer : link.peers()) {
                    log::write("online", "%s in zone %04u (here %04u) is at %.2f %.2f %.2f", peer.name.c_str(), peer.zone, disc::current_field(), peer.position[0], peer.position[1], peer.position[2]);
                }
            }
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
