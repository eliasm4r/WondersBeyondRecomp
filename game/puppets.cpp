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
constexpr uint32_t kFirstHero = 204;
constexpr uint32_t kHeroes = 4;
constexpr uint32_t kCharacterKind = 4;
constexpr uint32_t kFrameSize = 0x400;
constexpr uint32_t kParameters = 0x100;
constexpr uint32_t kHandleSpace = 0x20;
constexpr int kLoadPatience = 900;
constexpr int kCharacterPatience = 120;
constexpr int kRemoveCooldown = 120;
constexpr float kFollow = 0.35f;
constexpr float kTeleport = 30.0f;
constexpr float kRunning = 0.06f;

struct Puppet {
    uint32_t peer = 0;
    uint32_t object = 0;
    uint32_t handle = 0;
    uint32_t species = 0;
    uint32_t motion = 0;
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

void follow(Cpu& c, Puppet& puppet, const presence::Peer& peer, uint32_t frame) {
    uint32_t accessor = guest(c, kAccessor, puppet.object);
    uint32_t posture = method(c, accessor, kPostureSlot);
    uint32_t position = method(c, posture, kPositionSlot);
    uint32_t rotation = method(c, posture, kRotationSlot);
    float distance = 0.0f;
    for (int i = 0; i < 3; i++) {
        float gap = peer.position[i] - puppet.position[i];
        distance += i == 1 ? 0.0f : gap * gap;
    }
    distance = std::sqrt(distance);
    for (uint32_t i = 0; i < 3; i++) {
        puppet.position[i] = distance > kTeleport ? peer.position[i] : puppet.position[i] + (peer.position[i] - puppet.position[i]) * kFollow;
        wrf32(position + 4 * i, puppet.position[i]);
        wrf32(rotation + 4 * i, peer.rotation[i]);
    }
    play(c, puppet, distance > kRunning ? kRun : kWait, frame);
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
    peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const presence::Peer& peer) { return peer.zone == 0 || peer.zone != zone || peer.character >= kHeroes; }), peers.end());
    for (auto puppet = g_puppets.begin(); puppet != g_puppets.end();) {
        auto peer = std::find_if(peers.begin(), peers.end(), [&](const presence::Peer& entry) { return entry.id == puppet->peer; });
        bool stale = !alive(*puppet);
        puppet->changed = peer != peers.end() && kFirstHero + peer->character != puppet->species ? puppet->changed + 1 : 0;
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
        puppet.species = kFirstHero + peer.character;
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
