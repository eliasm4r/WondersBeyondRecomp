#include "puppets.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "wp/disc.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/online.h"
#include "wp/settings.h"

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
constexpr uint32_t kHideReasons = 0x81;
constexpr uint32_t kShowForReason = 0xcc;
constexpr uint32_t kReasonCount = 8;
constexpr uint32_t kObjectHandle = 0x4;
constexpr uint32_t kObjectState = 0x18;
constexpr std::array<uint32_t, 4> kHeroSpecies = {204, 207, 205, 206};
constexpr uint32_t kCharacterKind = 4;
constexpr uint32_t kSceneManager = 0x804bae98;
constexpr uint32_t kSceneChanging = 0x2c;
constexpr uint32_t kFrameSize = 0x400;
constexpr uint32_t kParameters = 0x100;
constexpr uint32_t kHandleSpace = 0x20;
constexpr int kLoadPatience = 900;
constexpr int kCharacterPatience = 3;
constexpr int kRemoveCooldown = 2;
constexpr int kMissingCooldown = 60;
constexpr uint32_t kModelNames = 0x800fcef0;
constexpr uint32_t kResources = 0x80072700;
constexpr uint32_t kCopyText = 0x804065b4;
constexpr uint32_t kResourcePools = 4;
constexpr uint32_t kPoolSize = 0x24;
constexpr uint32_t kPoolLookup = 0x14;
constexpr uint32_t kFindSlot = 0x24;
constexpr uint32_t kResourceKey = 0x30;
constexpr uint32_t kResourceKeyName = 0x38;
constexpr uint32_t kFindOnDisc = 0x802abf00;
constexpr uint32_t kModelNameSize = 0x2a;
constexpr uint32_t kModelNameCount = 4;
constexpr uint32_t kLoadFile = 0x800169f0;
constexpr uint32_t kFileReady = 0x80017290;
constexpr uint32_t kReleaseFile = 0x80017a30;
constexpr uint32_t kReactionSystem = 0x3618;
constexpr uint32_t kReactionsWaiting = 0x40;
constexpr uint32_t kReactionsActive = 0x48;
constexpr uint32_t kReactionHandle = 0x4;
constexpr uint32_t kReactionNodes = 0x50;
constexpr uint32_t kReactionFreeSlots = 0x54;
constexpr uint32_t kReactionFreeHead = 0x5c;
constexpr uint32_t kReactionUsed = 0x5e;
constexpr uint32_t kReactionNodeSize = 0x24;
constexpr uint32_t kFieldPool = 3;
constexpr uint32_t kLoadStackFirst = 0x8;
constexpr uint32_t kLoadStackSecond = 0xc;
constexpr int kReportEvery = 1200;
constexpr double kShownBehind = 0.08;
constexpr double kClockRelax = 0.0002;
constexpr size_t kKeptSnapshots = 32;
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
constexpr uint32_t kEffectPlaced = 0x78;
constexpr uint32_t kShotSlot = 0xa0;
constexpr uint32_t kShotStart = 0x8009d920;
constexpr uint32_t kShotModes = 6;
constexpr uint32_t kShotTable = 0x74;
constexpr uint32_t kShotTableRoom = 0x78;
constexpr uint32_t kShotTableFirst = 0x7c;
constexpr uint32_t kShotTableCount = 0x80;
constexpr uint32_t kShotEntrySize = 12;
constexpr uint32_t kShotsAtOnce = 4;
constexpr uint32_t kNoShot = 0xffffffffu;
constexpr uint32_t kFindObject = 0x800d4c30;
constexpr uint32_t kObjectManager = 0xffffc9d0;
constexpr uint32_t kPlacedFlag = 0xb;
constexpr uint32_t kPlacedExtra = 0xc;
constexpr uint32_t kPlacedFlagShift = 16;
constexpr uint32_t kPlacedJoint = 0xffff;
constexpr uint32_t kEventPoint = 0x240;
constexpr uint32_t kEventVector = 0x250;
constexpr uint32_t kEventScale = 0x260;
constexpr uint32_t kEffectManager = 0x800769e0;
constexpr uint32_t kModelObject = 0xc;
constexpr uint32_t kLocalHeight = 0x28;
constexpr uint32_t kWorldHeight = 0x58;
constexpr uint32_t kNoBeam = 0xffffffffu;
constexpr uint32_t kBeamEffect = 0x20010005;
constexpr uint32_t kBeamCreate = 0x58;
constexpr uint32_t kBeamSet = 0x8005dbd0;
constexpr uint32_t kBeamMove = 0x8005dc40;
constexpr uint32_t kBeamStop = 0x8005dde0;
constexpr uint32_t kSmallPool = 0x70;
constexpr uint32_t kSmallEntry = 0x40;
constexpr uint32_t kLargePool = 0x84;
constexpr uint32_t kLargeEntry = 0xa8;
constexpr int kBeamPatience = 60;
constexpr uint32_t kPoolNext = 8;
constexpr uint32_t kPoolHead = 0x10;
constexpr uint32_t kPoolEnd = 0xffff;
constexpr uint32_t kSmallSlots = 0x6c;
constexpr uint32_t kLargeSlots = 0x80;
constexpr uint32_t kBeamSlots = 0x150;
constexpr uint32_t kKeepEffects = 16;
constexpr uint32_t kBeamNow = 1;
constexpr uint32_t kEffectGroups = 0x24;
constexpr uint32_t kGroupMatches = 0x80062570;
constexpr uint32_t kStopOneSound = 0x64;
constexpr uint32_t kStopAllSounds = 0x68;
constexpr uint32_t kLongestFade = 600;
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
    uint32_t last_event = 0;
    uint32_t beam = 0xffffffffu;
    int beam_age = 0;
    uint32_t sequence = 0;
    double clock_gap = 0.0;
    std::vector<presence::Snapshot> snapshots;
    bool heard = false;
    bool ready = false;
    int age = 0;
    int changed = 0;
    std::array<float, 3> position{};
    uint32_t accessor = 0;
};

std::vector<Puppet> g_puppets;
uint32_t g_zone = 0;
int g_cooldown = 0;
int g_idle = 0;
constexpr int kIdleForget = 30;
bool g_starting_motion = false;

uint32_t guest(Cpu& c, uint32_t function, uint32_t a, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0) {
    c.r[3] = a;
    c.r[4] = b;
    c.r[5] = d;
    c.r[6] = e;
    call(c, function);
    return c.r[3];
}

std::vector<uint32_t> g_new_shots;
std::vector<uint32_t> g_starting_shots;

bool knows_shot(uint32_t shots, uint32_t id) {
    uint32_t table = rd32(shots + kShotTable);
    uint32_t room = rd32(shots + kShotTableRoom);
    uint32_t first = rd32(shots + kShotTableFirst);
    uint32_t count = rd32(shots + kShotTableCount);
    if (table < kFirstPointer || room == 0 || count > room) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (rd32(table + (first + i) % room * kShotEntrySize) == id) {
            return true;
        }
    }
    if (count == room) {
        return false;
    }
    uint32_t entry = table + (first + count) % room * kShotEntrySize;
    wr32(entry, id);
    wr32(entry + 4, kShotsAtOnce);
    wr32(entry + 8, 0);
    wr32(shots + kShotTableCount, count + 1);
    return true;
}

uint32_t method(Cpu& c, uint32_t object, uint32_t slot, uint32_t b = 0) {
    return guest(c, rd32(rd32(object) + slot), object, b);
}

bool alive(const Puppet& puppet) {
    return rd32(puppet.object) == kPokemonTable && rd32(puppet.object + kObjectHandle) == puppet.handle && rd8(puppet.object + kObjectState) >= 2 && rd8(puppet.object + kObjectState) <= 4;
}

bool g_making_beam = false;

uint32_t free_slots(uint32_t manager, uint32_t pool, uint32_t enough) {
    uint32_t next = rd32(manager + pool + kPoolNext);
    uint32_t slot = rd16(manager + pool + kPoolHead);
    uint32_t count = 0;
    while (slot != kPoolEnd && count < enough) {
        count++;
        slot = rd16(next + slot * 2);
    }
    return count;
}

bool room_for_effect(uint32_t manager) {
    return free_slots(manager, kSmallSlots, kKeepEffects + 1) > kKeepEffects && free_slots(manager, kLargeSlots, kKeepEffects + 1) > kKeepEffects;
}

bool beam_alive(uint32_t manager, uint32_t beam) {
    if (beam == kNoBeam) {
        return false;
    }
    uint32_t index = (beam >> 16) & 0x7fff;
    uint32_t entry = beam >> 31 ? rd32(manager + kLargePool) + index * kLargeEntry : rd32(manager + kSmallPool) + index * kSmallEntry;
    return rd32(entry + 8) == beam;
}

void stop_beam(Cpu& c, Puppet& puppet, uint32_t how = 0) {
    uint32_t manager = guest(c, kEffectManager, 0);
    if (beam_alive(manager, puppet.beam)) {
        guest(c, kBeamStop, manager, puppet.beam, how);
    }
    puppet.beam = kNoBeam;
}

void forget_reactions(Cpu& c, uint32_t handle) {
    uint32_t reactions = rd32(c.r[13] - kReactionSystem);
    if (reactions < kFirstPointer) {
        return;
    }
    for (uint32_t list : {kReactionsWaiting, kReactionsActive}) {
        uint32_t link = reactions + list;
        while (uint32_t node = rd32(link)) {
            if (rd32(node + kReactionHandle) != handle) {
                link = node;
                continue;
            }
            wr32(link, rd32(node));
            uint32_t index = (node - rd32(reactions + kReactionNodes)) / kReactionNodeSize;
            wr16(rd32(reactions + kReactionFreeSlots) + index * 2, rd16(reactions + kReactionFreeHead));
            wr16(reactions + kReactionFreeHead, static_cast<uint16_t>(index));
            wr16(reactions + kReactionUsed, static_cast<uint16_t>(rd16(reactions + kReactionUsed) - 1));
        }
    }
}

void remove(Cpu& c, Puppet& puppet) {
    if (alive(puppet)) {
        stop_beam(c, puppet);
        uint32_t sounds = method(c, guest(c, kAccessor, puppet.object), kSoundSlot);
        guest(c, rd32(rd32(sounds) + kStopAllSounds), sounds, 0);
        uint32_t manager = rd32(c.r[13] - kManagerOffset);
        guest(c, kUnregister, manager, puppet.handle);
        forget_reactions(c, puppet.handle);
        guest(c, kDestroy, manager, puppet.handle);
    }
}

void play(Cpu& c, Puppet& puppet, uint32_t motion, uint32_t frame) {
    if (puppet.motion == motion) {
        return;
    }
    uint32_t handle = frame + kHandleSpace;
    guest(c, kMakeHandle, handle, rd32(c.r[13] - kManagerOffset), puppet.handle, 0);
    g_starting_motion = true;
    guest(c, kPlayMotion, handle, motion, 0);
    g_starting_motion = false;
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
        if (event.kind == 10) {
            uint32_t shots = method(c, accessor, kShotSlot);
            if (shots && event.a < kShotModes && knows_shot(shots, event.id) && room_for_effect(guest(c, kEffectManager, 0))) {
                uint32_t point = frame + kEventPoint;
                uint32_t way = frame + kEventVector;
                for (uint32_t i = 0; i < 3; i++) {
                    wrf32(point + 4 * i, puppet.position[i] + event.offset[i]);
                    wrf32(way + 4 * i, event.vector[i]);
                }
                c.r[7] = event.a;
                uint32_t shot = guest(c, kShotStart, shots, event.id, point, way);
                uint32_t shot_object = shot == kNoShot ? 0 : guest(c, kFindObject, rd32(c.r[13] + kObjectManager), shot);
                if (shot_object) {
                    g_new_shots.push_back(guest(c, kAccessor, shot_object));
                }
            }
            continue;
        }
        if (event.kind == 9) {
            uint32_t joint = event.a & kPlacedJoint;
            if (joint < kMostJoints && event.c < kFirstPointer && effect_loaded(c, event.id) && room_for_effect(guest(c, kEffectManager, 0))) {
                uint32_t point = frame + kEventPoint;
                uint32_t turn = frame + kEventVector;
                uint32_t scale = frame + kEventScale;
                for (uint32_t i = 0; i < 3; i++) {
                    wrf32(point + 4 * i, event.offset[i]);
                    wrf32(turn + 4 * i, event.vector[i]);
                    wrf32(scale + 4 * i, i == 0 ? event.f1 : event.f2);
                }
                uint32_t effects = method(c, accessor, kEffectSlot);
                wr8(frame + kPlacedFlag, static_cast<uint8_t>(event.a >> kPlacedFlagShift));
                wr32(frame + kPlacedExtra, event.c);
                c.r[7] = point;
                c.r[8] = turn;
                c.r[9] = scale;
                c.r[10] = event.b < kFirstPointer ? event.b : 0;
                guest(c, rd32(rd32(effects) + kEffectPlaced), effects, event.id, method(c, accessor, kModelSlot), joint);
            }
            continue;
        }
        if (event.kind >= 6) {
            puppet.beam_age = 0;
            if (event.kind == 8) {
                stop_beam(c, puppet);
                continue;
            }
            bool finite = true;
            for (int i = 0; i < 3; i++) {
                finite = finite && std::isfinite(event.offset[i]) && std::isfinite(event.vector[i]);
            }
            if (!finite || !effect_loaded(c, kBeamEffect)) {
                continue;
            }
            uint32_t start = frame + kEventPoint;
            uint32_t end = frame + kEventVector;
            uint32_t ends = frame + kEventScale;
            for (uint32_t i = 0; i < 3; i++) {
                wrf32(start + 4 * i, puppet.position[i] + event.offset[i]);
                wrf32(end + 4 * i, puppet.position[i] + event.vector[i]);
            }
            uint32_t manager = guest(c, kEffectManager, 0);
            if (event.kind == 6) {
                stop_beam(c, puppet);
                if (free_slots(manager, kBeamSlots, 1) == 0 || !room_for_effect(manager)) {
                    continue;
                }
                uint32_t effects = method(c, accessor, kEffectSlot);
                g_making_beam = true;
                puppet.beam = guest(c, rd32(rd32(effects) + kBeamCreate), effects, kBeamEffect, start, 0);
                g_making_beam = false;
                if (beam_alive(manager, puppet.beam)) {
                    wr32(ends, start);
                    wr32(ends + 4, end);
                    guest(c, kBeamSet, manager, puppet.beam, ends);
                } else {
                    puppet.beam = kNoBeam;
                }
            } else if (beam_alive(manager, puppet.beam)) {
                guest(c, kBeamMove, manager, puppet.beam, start, end);
            }
            continue;
        }
        if (event.kind == 4 || event.kind == 5) {
            uint32_t sounds = method(c, accessor, kSoundSlot);
            uint32_t fade = event.b <= kLongestFade ? event.b : 0;
            if (event.kind == 5) {
                guest(c, rd32(rd32(sounds) + kStopAllSounds), sounds, fade);
            } else {
                guest(c, rd32(rd32(sounds) + kStopOneSound), sounds, event.id, fade);
            }
            continue;
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
        if (event.kind != 0 && (!effect_loaded(c, event.id) || (event.kind == 2 && event.a >= kMostJoints) || !room_for_effect(guest(c, kEffectManager, 0)))) {
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
            static const settings::LiveNumber volume("online.volume", "WP_ONLINE_VOLUME");
            int percent = volume();
            if (percent <= 0) {
                continue;
            }
            c.f[1] = event.f1 * percent / 100.0f;
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
    puppet.accessor = accessor;
    uint32_t shown_model = method(c, accessor, kModelSlot);
    method(c, shown_model, kShowSlot, 1);
    for (uint32_t reason = 0; reason < kReasonCount; reason++) {
        if (rd8(shown_model + kHideReasons) >> reason & 1) {
            guest(c, rd32(rd32(shown_model) + kShowForReason), shown_model, 1, reason);
        }
    }
    uint32_t posture = method(c, accessor, kPostureSlot);
    uint32_t position = method(c, posture, kPositionSlot);
    uint32_t rotation = method(c, posture, kRotationSlot);
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (peer.sequence != puppet.sequence) {
        double sent = peer.pose.sent_ms / 1000.0;
        double gap_now = now - sent;
        puppet.clock_gap = puppet.snapshots.empty() ? gap_now : std::min(gap_now, puppet.clock_gap + kClockRelax);
        if (!puppet.snapshots.empty() && sent < puppet.snapshots.back().time) {
            puppet.snapshots.clear();
            puppet.clock_gap = gap_now;
        }
        puppet.sequence = peer.sequence;
        if (puppet.snapshots.empty() || sent > puppet.snapshots.back().time) {
            puppet.snapshots.push_back({sent, peer.position, peer.pose.rotation});
        }
        if (puppet.snapshots.size() > kKeptSnapshots) {
            puppet.snapshots.erase(puppet.snapshots.begin());
        }
    }
    presence::Snapshot shown = presence::snapshot_at(puppet.snapshots, now - puppet.clock_gap - kShownBehind, kTeleport);
    float distance = 0.0f;
    for (int i = 0; i < 3; i++) {
        float gap = shown.position[i] - puppet.position[i];
        distance += i == 1 ? 0.0f : gap * gap;
    }
    distance = std::sqrt(distance);
    for (uint32_t i = 0; i < 3; i++) {
        puppet.position[i] = shown.position[i];
        wrf32(position + 4 * i, puppet.position[i]);
        wrf32(rotation + 4 * i, shown.rotation[i]);
    }
    if (++puppet.age % kReportEvery == 1) {
        log::write("online", "%s shown at %.1f %.1f %.1f, packet %u", peer.name.c_str(), shown.position[0], shown.position[1], shown.position[2], peer.sequence);
    }
    uint32_t model_object = rd32(method(c, accessor, kModelSlot) + kModelObject);
    float ground = rdf32(model_object + kWorldHeight) - rdf32(model_object + kLocalHeight);
    wrf32(model_object + kLocalHeight, std::max(0.0f, shown.position[1] - ground));
    replay(c, puppet, peer, accessor, frame);
    if (puppet.beam != kNoBeam && ++puppet.beam_age > kBeamPatience) {
        stop_beam(c, puppet);
    }
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
        g_starting_motion = true;
        guest(c, kPlayMotion, handle, name, peer.pose.motion_flags);
        g_starting_motion = false;
        guest(c, kFreeHandle, handle, 0xffffffffu);
        puppet.motion = 0;
    }
}

struct Requested {
    std::string name;
    uint32_t handle;
};

std::vector<Requested> g_requested;

std::vector<uint32_t> g_releasing;

void release_requested(Cpu& c) {
    for (const Requested& request : g_requested) {
        g_releasing.push_back(request.handle);
    }
    g_requested.clear();
    uint32_t resources = guest(c, kResources, 0);
    g_releasing.erase(std::remove_if(g_releasing.begin(), g_releasing.end(), [&](uint32_t handle) {
        if (guest(c, kFileReady, resources, handle) != 1) {
            return false;
        }
        guest(c, kReleaseFile, resources, handle, 1, 0);
        return true;
    }), g_releasing.end());
}

bool model_files_present(Cpu& c, uint32_t species, uint32_t frame, uint32_t zone) {
    uint32_t names = frame + kNameSpace;
    guest(c, kModelNames, species, names, 0);
    uint32_t resources = guest(c, kResources, 0);
    for (uint32_t i = 0; i < kModelNameCount; i++) {
        uint32_t name = names + i * kModelNameSize;
        if (rd8(name) == 0 || static_cast<int32_t>(guest(c, kFindOnDisc, name)) >= 0) {
            continue;
        }
        guest(c, kCopyText, resources + kResourceKeyName, name);
        bool loaded = false;
        for (uint32_t pool = 0; pool < kResourcePools && !loaded; pool++) {
            uint32_t lookup = rd32(resources) + pool * kPoolSize + kPoolLookup;
            loaded = guest(c, rd32(rd32(lookup) + kFindSlot), lookup, resources + kResourceKey) != 0;
        }
        std::string missing;
        for (uint32_t at = name; rd8(at) != 0 && at < name + kModelNameSize; at++) {
            missing += static_cast<char>(rd8(at));
        }
        auto request = std::find_if(g_requested.begin(), g_requested.end(), [&](const Requested& r) { return r.name == missing; });
        if (request == g_requested.end() && loaded) {
            continue;
        }
        if (request == g_requested.end()) {
            wr32(c.r[1] + kLoadStackFirst, 0);
            wr32(c.r[1] + kLoadStackSecond, 0);
            c.r[7] = 1;
            c.r[8] = 1;
            c.r[9] = 0;
            c.r[10] = 0;
            uint32_t handle = guest(c, kLoadFile, resources, name, kFieldPool, 1);
            g_requested.push_back({missing, handle});
            log::write("online", "field %04u loads %s for other players (%08x)", zone, missing.c_str(), handle);
            return false;
        }
        if (guest(c, kFileReady, resources, request->handle) == 1) {
            continue;
        }
        return false;
    }
    return true;
}

void step(Cpu& c, bool active, uint32_t frame) {
    g_starting_shots.swap(g_new_shots);
    g_new_shots.clear();
    if (rd8(kSceneManager + kSceneChanging) != 0) {
        return;
    }
    uint32_t zone = disc::current_field();
    if (zone != g_zone) {
        g_puppets.clear();
        g_requested.clear();
        g_zone = zone;
    }
    if (!active) {
        g_idle++;
        return;
    }
    if (g_idle > kIdleForget) {
        for (Puppet& puppet : g_puppets) {
            puppet.heard = false;
        }
    }
    g_idle = 0;
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
        if (!model_files_present(c, puppet.species, frame, zone)) {
            g_cooldown = kMissingCooldown;
            break;
        }
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

bool puppets_shot_starting(uint32_t accessor) {
    return std::find(g_starting_shots.begin(), g_starting_shots.end(), accessor) != g_starting_shots.end() ||
           std::find(g_new_shots.begin(), g_new_shots.end(), accessor) != g_new_shots.end();
}

bool puppets_blocks_motion(uint32_t accessor) {
    return !g_starting_motion && std::any_of(g_puppets.begin(), g_puppets.end(), [&](const Puppet& puppet) { return puppet.ready && puppet.accessor == accessor; });
}

void puppets_free_beam(Cpu& c) {
    bool held = std::any_of(g_puppets.begin(), g_puppets.end(), [](const Puppet& puppet) { return puppet.beam != kNoBeam; });
    if (g_making_beam || !held) {
        return;
    }
    Cpu saved = c;
    for (Puppet& puppet : g_puppets) {
        stop_beam(c, puppet, kBeamNow);
    }
    std::memcpy(c.r, saved.r, sizeof c.r);
    std::memcpy(c.f, saved.f, sizeof c.f);
    std::memcpy(c.ps1, saved.ps1, sizeof c.ps1);
    std::memcpy(c.cr, saved.cr, sizeof c.cr);
    c.lr = saved.lr;
    c.ctr = saved.ctr;
}

void puppets_scene_change(Cpu& c) {
    if ((g_puppets.empty() && g_requested.empty() && g_releasing.empty()) || rd8(kSceneManager + kSceneChanging) == 0) {
        return;
    }
    Cpu saved = c;
    uint32_t stack = c.r[1];
    c.r[1] = stack - kFrameSize;
    wr32(c.r[1], stack);
    for (Puppet& puppet : g_puppets) {
        remove(c, puppet);
    }
    g_puppets.clear();
    release_requested(c);
    g_cooldown = kRemoveCooldown;
    std::memcpy(c.r, saved.r, sizeof c.r);
    std::memcpy(c.f, saved.f, sizeof c.f);
    std::memcpy(c.ps1, saved.ps1, sizeof c.ps1);
    std::memcpy(c.cr, saved.cr, sizeof c.cr);
    c.lr = saved.lr;
    c.ctr = saved.ctr;
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
