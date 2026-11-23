#include "puppets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "wp/disc.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/online.h"

namespace wp::game {

namespace {

constexpr uint32_t kManagerOffset = 0x3630;
constexpr uint32_t kCreate = 0x800d1650;
constexpr uint32_t kLoadModel = 0x800fd030;
constexpr uint32_t kSetKind = 0x800fed60;
constexpr uint32_t kSpawnParameters = 0x800ca7d0;
constexpr uint32_t kSetup = 0x800fc220;
constexpr uint32_t kAccessor = 0x800c5af0;
constexpr uint32_t kMakeHandle = 0x800d56a0;
constexpr uint32_t kFreeHandle = 0x800c64c0;
constexpr uint32_t kPlayMotion = 0x800c6c50;
constexpr uint32_t kUnregister = 0x800d8040;
constexpr uint32_t kDestroy = 0x800d3d30;
constexpr uint32_t kPokemonTable = 0x8044f740;
constexpr uint32_t kWait = 0x804af8b8;
constexpr uint32_t kRun = 0x804ada48;
constexpr uint32_t kReadySlot = 0xc;
constexpr uint32_t kModelSlot = 0x2c;
constexpr uint32_t kPostureSlot = 0x40;
constexpr uint32_t kPositionSlot = 0x54;
constexpr uint32_t kRotationSlot = 0x60;
constexpr uint32_t kShowSlot = 0x70;
constexpr uint32_t kObjectHandle = 0x4;
constexpr uint32_t kObjectState = 0x18;
constexpr std::array<uint32_t, 4> kHeroSpecies = {204, 207, 205, 206};
constexpr uint32_t kCharacterKind = 4;
constexpr uint32_t kFrameSize = 0x400;
constexpr uint32_t kParameters = 0x100;
constexpr uint32_t kHandleSpace = 0x20;
constexpr int kLoadPatience = 900;
constexpr int kCharacterPatience = 20;
constexpr int kRemoveCooldown = 120;
constexpr float kFollow = 0.35f;
constexpr float kTeleport = 30.0f;
constexpr float kRunning = 0.06f;
constexpr uint32_t kNameSpace = 0x200;
constexpr uint32_t kHitSlot = 0x68;
constexpr uint32_t kJostleSlot = 0x5c;
constexpr uint32_t kSetHittable = 0x3c;
constexpr uint32_t kSetPushable = 0x6c;
constexpr uint32_t kSoundSlot = 0x38;
constexpr uint32_t kEffectSlot = 0x3c;
constexpr uint32_t kPlaySound = 0x60;
constexpr uint32_t kEffectAt = 0x60;
constexpr uint32_t kEffectOn = 0x70;
constexpr uint32_t kEventPoint = 0x240;
constexpr uint32_t kEventVector = 0x250;
constexpr uint32_t kEventScale = 0x260;
constexpr uint32_t kEffectManager = 0x800769e0;
constexpr uint32_t kEffectGroups = 0x24;
constexpr uint32_t kGroupMatches = 0x80062570;
constexpr uint32_t kMostJoints = 0x100;
constexpr std::array<uint32_t, 5> kStopSlots = {0xa4, 0xa8, 0x98, 0x9c, 0xc0};
constexpr uint32_t kFirstPointer = 0x80000000;

struct Puppet {
    uint32_t peer = 0;
    uint32_t object = 0;
    uint32_t handle = 0;
    uint32_t species = 0;
    uint32_t motion = 0;
    uint32_t serial = 0;
    uint32_t position_address = 0;
    uint32_t last_event = 0;
    bool heard = false;
    bool ready = false;
    int age = 0;
    int changed = 0;
    std::array<float, 3> position{};
};

std::vector<Puppet> g_puppets;
uint32_t g_zone = 0;
int g_cooldown = 0;

uint32_t guest(Cpu& c, uint32_t function, uint32_t a, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0) {
    c.r[3] = a;
    c.r[4] = b;
    c.r[5] = d;
    c.r[6] = e;
    call(c, function);
    return c.r[3];
}

uint32_t method(Cpu& c, uint32_t object, uint32_t slot, uint32_t b = 0) {
    return guest(c, rd32(rd32(object) + slot), object, b);
}

bool alive(const Puppet& puppet) {
    return rd32(puppet.object) == kPokemonTable && rd32(puppet.object + kObjectHandle) == puppet.handle && rd8(puppet.object + kObjectState) >= 2 && rd8(puppet.object + kObjectState) <= 4;
}

void remove(Cpu& c, const Puppet& puppet) {
    if (alive(puppet)) {
        uint32_t manager = rd32(c.r[13] - kManagerOffset);
        guest(c, kUnregister, manager, puppet.handle);
        guest(c, kDestroy, manager, puppet.handle);
    }
}

void play(Cpu& c, Puppet& puppet, uint32_t motion, uint32_t frame) {
    if (puppet.motion == motion) {
        return;
    }
    uint32_t handle = frame + kHandleSpace;
    guest(c, kMakeHandle, handle, rd32(c.r[13] - kManagerOffset), puppet.handle, 0);
    guest(c, kPlayMotion, handle, motion, 0);
    guest(c, kFreeHandle, handle, 0xffffffffu);
    puppet.motion = motion;
}

void finish(Cpu& c, Puppet& puppet, const presence::Peer& peer, uint32_t frame) {
    uint32_t parameters = frame + kParameters;
    guest(c, kSpawnParameters, parameters, 0);
    for (uint32_t i = 0; i < 3; i++) {
        wrf32(parameters + 8 + 4 * i, peer.position[i]);
    }
    guest(c, kSetup, puppet.object, parameters);
    uint32_t accessor = guest(c, kAccessor, puppet.object);
    method(c, method(c, accessor, kModelSlot), kShowSlot, 1);
    puppet.position = peer.position;
    puppet.ready = true;
    play(c, puppet, kWait, frame);
    log::write("online", "showing %s as a character", peer.name.c_str());
}

bool effect_loaded(Cpu& c, uint32_t id) {
    c.r[3] = 0;
    call(c, kEffectManager);
    for (uint32_t group = rd32(c.r[3] + kEffectGroups); group; group = rd32(group)) {
        if (guest(c, kGroupMatches, group, id >> 16) == 1) {
            return true;
        }
    }
    return false;
}

void replay(Cpu& c, Puppet& puppet, const presence::Peer& peer, uint32_t accessor, uint32_t frame) {
    uint32_t newest = puppet.last_event;
    for (const presence::Event& event : peer.pose.events) {
        if (!puppet.heard || static_cast<int32_t>(event.serial - puppet.last_event) <= 0) {
            if (static_cast<int32_t>(event.serial - newest) > 0) {
                newest = event.serial;
            }
            continue;
        }
        if (static_cast<int32_t>(event.serial - newest) > 0) {
            newest = event.serial;
        }
        if (event.kind == 3) {
            bool known = std::find(kStopSlots.begin(), kStopSlots.end(), event.a) != kStopSlots.end();
            bool plain = (event.id < kFirstPointer || event.id == 0xffffffffu) && (event.b < kFirstPointer || event.b == 0xffffffffu);
            if (known && plain) {
                uint32_t effects = method(c, accessor, kEffectSlot);
                guest(c, rd32(rd32(effects) + event.a), effects, event.id, event.b);
            }
            continue;
        }
        if (event.kind != 0 && (!effect_loaded(c, event.id) || (event.kind == 2 && event.a >= kMostJoints))) {
            continue;
        }
        uint32_t point = frame + kEventPoint;
        uint32_t vector = frame + kEventVector;
        uint32_t scale = frame + kEventScale;
        for (uint32_t i = 0; i < 3; i++) {
            wrf32(point + 4 * i, puppet.position[i] + event.offset[i]);
            wrf32(vector + 4 * i, event.vector[i]);
            wrf32(scale + 4 * i, event.f1);
        }
        uint32_t module = method(c, accessor, event.kind == 0 ? kSoundSlot : kEffectSlot);
        uint32_t table = rd32(module);
        if (event.kind == 0) {
            c.r[7] = 0;
            c.r[8] = 0;
            c.f[1] = event.f1;
            c.f[2] = event.f2;
            guest(c, rd32(table + kPlaySound), module, accessor, event.id, 0xffffffffu);
        } else if (event.kind == 1) {
            c.r[7] = scale;
            guest(c, rd32(table + kEffectAt), module, event.id, point, vector);
        } else {
            uint32_t model = method(c, accessor, kModelSlot);
            c.r[7] = event.b;
            c.r[8] = event.c;
            c.f[1] = event.f1;
            c.f[2] = event.f2;
            guest(c, rd32(table + kEffectOn), module, event.id, model, event.a);
        }
    }
    puppet.last_event = newest;
    puppet.heard = true;
}

void follow(Cpu& c, Puppet& puppet, const presence::Peer& peer, uint32_t frame) {
    uint32_t accessor = guest(c, kAccessor, puppet.object);
    uint32_t posture = method(c, accessor, kPostureSlot);
    uint32_t position = method(c, posture, kPositionSlot);
    uint32_t rotation = method(c, posture, kRotationSlot);
    puppet.position_address = position;
    float distance = 0.0f;
    for (int i = 0; i < 3; i++) {
        float gap = peer.position[i] - puppet.position[i];
        distance += i == 1 ? 0.0f : gap * gap;
    }
    distance = std::sqrt(distance);
    for (uint32_t i = 0; i < 3; i++) {
        puppet.position[i] = distance > kTeleport ? peer.position[i] : puppet.position[i] + (peer.position[i] - puppet.position[i]) * kFollow;
        wrf32(position + 4 * i, puppet.position[i]);
        wrf32(rotation + 4 * i, peer.pose.rotation[i]);
    }
    replay(c, puppet, peer, accessor, frame);
    method(c, method(c, accessor, kHitSlot), kSetHittable, 0);
    method(c, method(c, accessor, kJostleSlot), kSetPushable, 0);
    if (peer.pose.motion.empty()) {
        play(c, puppet, distance > kRunning ? kRun : kWait, frame);
    } else if (peer.pose.motion_serial != puppet.serial) {
        puppet.serial = peer.pose.motion_serial;
        uint32_t name = frame + kNameSpace;
        for (uint32_t i = 0; i <= peer.pose.motion.size(); i++) {
            wr8(name + i, static_cast<uint8_t>(peer.pose.motion.c_str()[i]));
        }
        uint32_t handle = frame + kHandleSpace;
        guest(c, kMakeHandle, handle, rd32(c.r[13] - kManagerOffset), puppet.handle, 0);
        guest(c, kPlayMotion, handle, name, peer.pose.motion_flags);
        guest(c, kFreeHandle, handle, 0xffffffffu);
        puppet.motion = 0;
    }
}

void step(Cpu& c, bool active, uint32_t frame) {
    uint32_t zone = disc::current_field();
    if (zone != g_zone) {
        g_puppets.clear();
        g_zone = zone;
    }
    if (!active) {
        return;
    }
    std::vector<presence::Peer> peers = online::running() ? online::peers() : std::vector<presence::Peer>{};
    peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const presence::Peer& peer) { return peer.zone == 0 || peer.zone != zone || peer.pose.character >= kHeroSpecies.size(); }), peers.end());
    for (auto puppet = g_puppets.begin(); puppet != g_puppets.end();) {
        auto peer = std::find_if(peers.begin(), peers.end(), [&](const presence::Peer& entry) { return entry.id == puppet->peer; });
        bool stale = !alive(*puppet);
        puppet->changed = peer != peers.end() && kHeroSpecies[peer->pose.character] != puppet->species ? puppet->changed + 1 : 0;
        bool wrong = peer == peers.end() || puppet->changed > kCharacterPatience || (!puppet->ready && ++puppet->age > kLoadPatience);
        if (stale || wrong) {
            if (!stale) {
                remove(c, *puppet);
                g_cooldown = kRemoveCooldown;
            }
            puppet = g_puppets.erase(puppet);
            continue;
        }
        if (puppet->ready) {
            follow(c, *puppet, *peer, frame);
        } else if (method(c, puppet->object, kReadySlot) == 1) {
            finish(c, *puppet, *peer, frame);
        }
        ++puppet;
    }
    if (g_cooldown > 0) {
        g_cooldown--;
        return;
    }
    for (const presence::Peer& peer : peers) {
        if (std::any_of(g_puppets.begin(), g_puppets.end(), [&](const Puppet& puppet) { return puppet.peer == peer.id; })) {
            continue;
        }
        Puppet puppet;
        puppet.peer = peer.id;
        puppet.species = kHeroSpecies[peer.pose.character];
        puppet.object = guest(c, kCreate, rd32(c.r[13] - kManagerOffset), 1, puppet.species);
        if (puppet.object) {
            puppet.handle = rd32(puppet.object + kObjectHandle);
            guest(c, kLoadModel, puppet.object, puppet.species, 0);
            guest(c, kSetKind, puppet.object, kCharacterKind);
            g_puppets.push_back(puppet);
        }
        break;
    }
}

}

bool hold_puppet(uint32_t object) {
    for (const Puppet& puppet : g_puppets) {
        if (puppet.object == object && puppet.position_address && alive(puppet)) {
            for (uint32_t i = 0; i < 3; i++) {
                wrf32(puppet.position_address + 4 * i, puppet.position[i]);
            }
            return true;
        }
    }
    return false;
}

void puppets_frame(Cpu& c, bool park, bool active) {
    if (!park) {
        g_puppets.clear();
        g_zone = 0;
        return;
    }
    if (g_puppets.empty() && !online::running()) {
        return;
    }
    Cpu saved = c;
    uint32_t stack = c.r[1];
    uint32_t frame = stack - kFrameSize;
    wr32(frame, stack);
    c.r[1] = frame;
    step(c, active, frame);
    std::memcpy(c.r, saved.r, sizeof c.r);
    std::memcpy(c.f, saved.f, sizeof c.f);
    std::memcpy(c.ps1, saved.ps1, sizeof c.ps1);
    std::memcpy(c.cr, saved.cr, sizeof c.cr);
    c.lr = saved.lr;
    c.ctr = saved.ctr;
}

}
